"""Real TCP integration tests; all data and processes are local and temporary."""

import concurrent.futures
import hashlib
import os
from pathlib import Path
import pty
import random
import re
import resource
import select
import signal
import socket
import stat
import struct
import subprocess
import tempfile
import termios
import threading
import time
import unittest


BINARY = Path(__file__).resolve().parents[2] / "server"
CLIENT_BINARY = BINARY.with_name("client")
PASSWORD = "LocalTest!42"


def available_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def eventually(predicate, timeout=8):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.02)
    raise AssertionError("condition did not become true before timeout")


class Connection:
    def __init__(self, port):
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=20)
        self.pending = b""

    def line(self):
        while b"\n" not in self.pending:
            chunk = self.socket.recv(4096)
            if not chunk:
                if self.pending:
                    raise AssertionError("truncated response: %r" % self.pending)
                return b""
            self.pending += chunk
        result, self.pending = self.pending.split(b"\n", 1)
        if not result.endswith(b"\r"):
            raise AssertionError("response missing CRLF: %r" % result)
        line = result[:-1]
        if re.fullmatch(rb"[245][0-9]{2}[ -][!-~](?:[ -~]*[!-~])?", line) is None:
            raise AssertionError("invalid numeric reply line: %r" % line)
        return line

    def command(self, command):
        if isinstance(command, str):
            command = command.encode("ascii")
        self.socket.sendall(command + b"\r\n")
        return self.line()

    def close(self):
        self.socket.close()

    def __enter__(self):
        return self

    def __exit__(self, *unused):
        self.close()


class Server:
    def __init__(self, directory, accounts=None, port=None, preexec_fn=None, defaults=False, log=None):
        self.port = available_port() if port is None else port
        self.accounts = Path(directory) / "accounts.db" if accounts is None else Path(accounts)
        self.stderr = tempfile.TemporaryFile()
        command = [str(BINARY), "--bind", "127.0.0.1", "--port", str(self.port)]
        if not defaults:
            command += ["--accounts", str(self.accounts)]
        else:
            self.accounts = Path(directory) / "data/server/accounts.db"
        if log is not None:
            command += ["--log", str(log)]
        self.process = subprocess.Popen(command, cwd=directory, stdin=subprocess.DEVNULL,
                                        stdout=subprocess.PIPE, stderr=self.stderr,
                                        preexec_fn=preexec_fn)
        output = b""
        deadline = time.monotonic() + 15
        try:
            while b"READY\n" not in output and time.monotonic() < deadline:
                readable, _, _ = select.select([self.process.stdout], [], [], 0.1)
                if readable:
                    chunk = os.read(self.process.stdout.fileno(), 4096)
                    if not chunk:
                        break
                    output += chunk
            if b"READY\n" not in output:
                self.process.kill()
                self.process.wait(timeout=5)
                raise AssertionError("server failed startup: %r %s" % (output, self.errors()))
        except BaseException:
            self.process.stdout.close()
            self.stderr.close()
            raise

    def connect(self):
        return Connection(self.port)

    def errors(self):
        self.stderr.seek(0)
        return self.stderr.read().decode("utf-8", "replace")

    def stop(self, signum=signal.SIGTERM):
        if self.stderr.closed:
            return
        try:
            if self.process.poll() is None:
                self.process.send_signal(signum)
            try:
                code = self.process.wait(timeout=20)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
                raise AssertionError("server deadlocked during shutdown")
            errors = self.errors()
            if code != 0 or errors:
                raise AssertionError("server exited %s: %s" % (code, errors))
        finally:
            self.process.stdout.close()
            self.stderr.close()


class ServerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="p2p-test-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)

    def start(self, **kwargs):
        server = Server(self.directory, **kwargs)
        self.addCleanup(server.stop)
        return server

    def register(self, client, username="alice", password=PASSWORD):
        self.assertEqual(client.command("REGISTER %s %s" % (username, password)), b"200 OK")

    def login(self, client, username="alice", password=PASSWORD):
        self.assertEqual(client.command("LOGIN %s %s" % (username, password)), b"200 OK")

    def assert_startup_error(self, path, preexec_fn=None):
        result = subprocess.run([str(BINARY), "--accounts", str(path), "--port", str(available_port())],
                                cwd=self.directory, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                timeout=10, preexec_fn=preexec_fn)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn(b"account storage", result.stderr)
        self.assertNotIn(b"AddressSanitizer", result.stderr)
        self.assertNotIn(b"runtime error:", result.stderr)

    def test_cli_validation(self):
        help_result = subprocess.run([str(BINARY), "--help"], capture_output=True, timeout=5)
        self.assertEqual(help_result.returncode, 0)
        self.assertIn(b"127.0.0.1:9000", help_result.stdout)
        cases = [["--port", value] for value in ("0", "65536", "-1", "+1", "1x", "", "9" * 80)]
        cases += [["--bind", "localhost"], ["--bind", "::1"], ["--bind", "256.0.0.1"],
                  ["--port"], ["--unknown", "1"]]
        for args in cases:
            with self.subTest(args=args):
                result = subprocess.run([str(BINARY)] + args, cwd=self.directory,
                                        capture_output=True, timeout=5)
                self.assertEqual(result.returncode, 1, result.stderr)

    def test_default_storage_directory(self):
        server = self.start(defaults=True)
        self.assertTrue(server.accounts.is_file())
        with server.connect() as client:
            self.register(client)
        self.assertEqual(stat.S_IMODE(server.accounts.stat().st_mode), 0o600)
        self.assertEqual(stat.S_IMODE(server.accounts.parent.stat().st_mode), 0o700)

    def test_startup_and_simultaneous_connections(self):
        server = self.start()
        with concurrent.futures.ThreadPoolExecutor(max_workers=24) as pool:
            def work(index):
                with server.connect() as client:
                    return client.command("UNKNOWN_%d" % index)
            self.assertEqual(list(pool.map(work, range(48))), [b"500 Invalid or unknown command"] * 48)

    def test_fragmentation_and_pipelining(self):
        server = self.start()
        with server.connect() as client:
            wire = ("HELLO 4294967295 65535\r\nREGISTER alice %s\r\nLOGIN alice %s\r\nLOGOUT\r\n" %
                    (PASSWORD, PASSWORD)).encode()
            for fragment in (wire[:2], wire[2:11], wire[11:23], wire[23:]):
                client.socket.sendall(fragment)
                time.sleep(0.01)
            self.assertEqual([client.line() for _ in range(4)],
                             [b"200 OK", b"200 OK", b"200 OK", b"200 OK"])
            self.assertEqual(client.command("HELLO 1 1"), b"200 OK")

    def test_malformed_requests_and_recovery(self):
        server = self.start()
        with server.connect() as client:
            invalid_commands = [b"", b" LOGOUT", b"HELLO\t1 2", b"LOGOUT\r",
                                b"LOGOUT\x00", b"\xff", b"\x01", b"GET file.bin"]
            invalid_arguments = [b"LOGOUT ", b"LOGOUT x", b"HELLO", b"HELLO 1",
                                 b"HELLO 1 2 3", b"HELLO  1 2", b"REGISTER", b"LOGIN alice",
                                 b"REGISTER bad-name password", b"LOGIN alice short",
                                 b"LOGIN alice pass word"]
            for request in invalid_commands:
                with self.subTest(request=request):
                    self.assertEqual(client.command(request), b"500 Invalid or unknown command")
            for request in invalid_arguments:
                with self.subTest(request=request):
                    self.assertEqual(client.command(request), b"501 Invalid arguments")
            self.assertEqual(client.command(f"rEgIsTeR Alice {PASSWORD}"), b"200 OK")
            self.assertEqual(client.command(f"login alice {PASSWORD}"), b"530 Invalid credentials")
            self.assertEqual(client.command(f"lOgIn Alice {PASSWORD}"), b"200 OK")
            self.assertEqual(client.command("logout"), b"200 OK")
            self.assertEqual(client.command("logout"), b"530 Not logged in")

    def test_long_lines_and_invalid_framing_recovery(self):
        server = self.start()
        with server.connect() as client:
            self.assertEqual(client.command(b"x" * 16400), b"500 Invalid or unknown command")
            client.socket.sendall(b"LOGOUT\nLOGOUT\r")
            self.assertEqual(client.line(), b"500 Invalid or unknown command")
            client.socket.sendall(b"\n")
            self.assertEqual(client.line(), b"530 Not logged in")

    def test_incomplete_eof_is_discarded(self):
        server = self.start()
        with server.connect() as client:
            client.socket.sendall(("REGISTER phantom %s" % PASSWORD).encode())
            client.socket.shutdown(socket.SHUT_WR)
            self.assertEqual(client.line(), b"")
        with server.connect() as client:
            self.assertEqual(client.command("LOGIN phantom " + PASSWORD), b"530 Invalid credentials")

    def test_registration_credentials_and_session_transitions(self):
        server = self.start()
        with server.connect() as client:
            self.register(client)
            self.assertEqual(client.command("HELLO 1 1000"), b"200 OK")
            self.assertEqual(client.command("REGISTER alice " + PASSWORD), b"550 Account already exists")
            self.assertEqual(client.command("LOGIN alice Incorrect!42"), b"530 Invalid credentials")
            self.assertEqual(client.command("LOGIN nobody " + PASSWORD), b"530 Invalid credentials")
            self.assertEqual(client.command("LOGIN Alice " + PASSWORD), b"530 Invalid credentials")
            self.login(client)
            self.assertEqual(client.command("LOGIN alice " + PASSWORD), b"503 Bad command sequence")
            self.assertEqual(client.command("REGISTER other " + PASSWORD), b"503 Bad command sequence")
            self.assertEqual(client.command("HELLO 1 1000"), b"200 OK")
            self.assertEqual(client.command("LOGOUT"), b"200 OK")
            self.assertEqual(client.command("LOGOUT"), b"530 Not logged in")
            self.login(client)

    def test_long_credentials_persist_across_restarts(self):
        server = self.start()
        username, password = "Z" * 5000, "~" * 6000
        with server.connect() as client:
            self.register(client, "a", "!" * 8)
            self.register(client, username, password)
        server.stop()
        restarted = self.start()
        with restarted.connect() as client:
            self.login(client, username, password)
        log = restarted.accounts.with_suffix(".db.log").read_text()
        self.assertIn("command=LOGIN result=200 username=" + username + "\n", log)
        self.assertNotIn(password, log)

    def test_client_id_and_port_validation(self):
        server = self.start()
        with server.connect() as client:
            self.register(client)
            self.login(client)
            for client_id, port in (("0", "1"), ("4294967295", "65535"), ("0001", "00080")):
                self.assertEqual(client.command("HELLO %s %s" % (client_id, port)), b"200 OK")
            for client_id in ("-1", "+1", "4294967296", "9" * 100, "1x", "0x10", "1.0"):
                self.assertEqual(client.command("HELLO %s 80" % client_id), b"501 Invalid arguments")
            for port in ("0", "65536", "-1", "+80", "1x", "9" * 100):
                self.assertEqual(client.command("HELLO 1 %s" % port), b"501 Invalid arguments")

    def test_duplicate_ids_updates_and_logout_release(self):
        server = self.start()
        with server.connect() as first, server.connect() as second:
            self.register(first)
            self.login(first)
            self.login(second)
            self.assertEqual(first.command("HELLO 10 1"), b"200 OK")
            self.assertEqual(second.command("HELLO 20 2"), b"200 OK")
            self.assertEqual(first.command("HELLO 20 3"), b"550 Client ID in use")
            self.assertEqual(second.command("HELLO 10 4"), b"550 Client ID in use")
            self.assertEqual(first.command("HELLO 10 65535"), b"200 OK")
            self.assertEqual(first.command("HELLO 30 5"), b"200 OK")
            self.assertEqual(second.command("HELLO 10 6"), b"200 OK")
            self.assertEqual(second.command("LOGOUT"), b"200 OK")
            self.assertEqual(first.command("HELLO 10 7"), b"200 OK")
            self.assertEqual(second.command("HELLO 50 8"), b"200 OK")

    def test_abrupt_disconnect_releases_id(self):
        server = self.start()
        with server.connect() as first:
            self.register(first)
            self.login(first)
            self.assertEqual(first.command("HELLO 99 99"), b"200 OK")
            first.socket.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        with server.connect() as second:
            self.login(second)
            def claimed():
                response = second.command("HELLO 99 100")
                self.assertIn(response, (b"200 OK", b"550 Client ID in use"))
                return response == b"200 OK"
            eventually(claimed)

    def test_concurrent_duplicate_registration(self):
        server = self.start()
        barrier = threading.Barrier(8)
        def register_same(_):
            with server.connect() as client:
                barrier.wait(timeout=10)
                return client.command("REGISTER shared " + PASSWORD)
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            results = list(pool.map(register_same, range(8)))
        self.assertEqual(results.count(b"200 OK"), 1)
        self.assertEqual(results.count(b"550 Account already exists"), 7)
        self.assertEqual(len(server.accounts.read_bytes().splitlines()), 1)

    def test_concurrent_registration_and_session_isolation(self):
        server = self.start()
        barrier = threading.Barrier(8)
        def worker(index):
            with server.connect() as client:
                barrier.wait(timeout=10)
                username = "user_%d" % index
                self.register(client, username)
                self.login(client, username)
                self.assertEqual(client.command("HELLO %d %d" % (index, index + 1)), b"200 OK")
                barrier.wait(timeout=15)
                if index % 2 == 0:
                    self.assertEqual(client.command("LOGOUT"), b"200 OK")
                else:
                    self.assertEqual(client.command("HELLO %d 65535" % index), b"200 OK")
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            list(pool.map(worker, range(8)))
        self.assertEqual(len(server.accounts.read_bytes().splitlines()), 8)

    def test_concurrent_id_claim(self):
        server = self.start()
        with server.connect() as client:
            self.register(client)
        clients = [server.connect() for _ in range(6)]
        for client in clients:
            self.addCleanup(client.close)
            self.login(client)
        barrier = threading.Barrier(len(clients))
        def claim(client):
            barrier.wait(timeout=10)
            return client.command("HELLO 12345 80")
        with concurrent.futures.ThreadPoolExecutor(max_workers=len(clients)) as pool:
            results = list(pool.map(claim, clients))
        self.assertEqual(results.count(b"200 OK"), 1)
        self.assertEqual(results.count(b"550 Client ID in use"), 5)

    def test_persistence_hashes_salts_and_permissions(self):
        server = self.start()
        with server.connect() as client:
            self.register(client, "alice")
            self.register(client, "bobby")
        raw = server.accounts.read_bytes()
        self.assertNotIn(PASSWORD.encode(), raw)
        records = [line.decode().split(":") for line in raw.splitlines()]
        self.assertEqual(len(records), 2)
        self.assertNotEqual(records[0][2], records[1][2])
        self.assertNotEqual(records[0][3], records[1][3])
        for username, iterations, salt_hex, hash_hex in records:
            self.assertIn(username, ("alice", "bobby"))
            self.assertEqual(int(iterations), 600000)
            salt_bytes = bytes.fromhex(salt_hex)
            self.assertEqual(len(salt_bytes), 16)
            expected = hashlib.pbkdf2_hmac("sha256", PASSWORD.encode(), salt_bytes, int(iterations))
            self.assertEqual(expected, bytes.fromhex(hash_hex))
        self.assertEqual(stat.S_IMODE(server.accounts.stat().st_mode), 0o600)
        port = server.port
        server.stop()
        server.accounts.chmod(0o666)
        restarted = self.start(port=port)
        self.assertEqual(stat.S_IMODE(restarted.accounts.stat().st_mode), 0o600)
        with restarted.connect() as client:
            self.login(client, "alice")
            self.assertEqual(client.command("HELLO 0 1"), b"200 OK")
            self.assertEqual(client.command("LOGOUT"), b"200 OK")
            self.login(client, "bobby")

    def test_corrupted_account_storage_fails_closed(self):
        server = self.start()
        with server.connect() as client:
            self.register(client)
        valid = server.accounts.read_bytes()
        server.stop()
        cases = [b"garbage\n", valid[:-1], valid + valid, valid.replace(b"600000", b"1"),
                 valid[:-2] + b"z\n",
                 valid.replace(b":", b":z", 1), b"\x00\n", b"x" * 10000 + b"\n", b"\n",
                 valid.replace(b"\n", b"\r\n"), valid + b"truncated"]
        for raw in cases:
            with self.subTest(raw=raw[:40]):
                server.accounts.write_bytes(raw)
                self.assert_startup_error(server.accounts)
                self.assertEqual(server.accounts.read_bytes(), raw)

    def test_account_lock_and_unsafe_paths(self):
        server = self.start()
        self.assert_startup_error(server.accounts)
        self.assert_startup_error(self.directory / "missing" / "accounts.db")
        self.assert_startup_error(self.directory)
        symlink = self.directory / "link"
        symlink.symlink_to(server.accounts)
        self.assert_startup_error(symlink)
        fifo = self.directory / "fifo"
        os.mkfifo(fifo)
        self.assert_startup_error(fifo)
        hardlink = self.directory / "hardlink"
        os.link(server.accounts, hardlink)
        self.assert_startup_error(hardlink)

    @unittest.skipUnless(os.geteuid() == 0, "permission denial test requires privilege drop")
    def test_inaccessible_storage(self):
        path = self.directory / "private.db"
        path.write_bytes(b"")
        path.chmod(0o600)
        self.directory.chmod(0o755)
        def drop_privileges():
            os.setgroups([])
            os.setgid(65534)
            os.setuid(65534)
        self.assert_startup_error(path, preexec_fn=drop_privileges)

    def test_failed_append_does_not_create_account(self):
        def limit_file_size():
            signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
            resource.setrlimit(resource.RLIMIT_FSIZE, (20, 20))
        server = self.start(preexec_fn=limit_file_size)
        with server.connect() as client:
            self.assertEqual(client.command("REGISTER alice " + PASSWORD), b"451 Local processing error")
            self.assertEqual(client.command("LOGIN alice " + PASSWORD), b"530 Invalid credentials")
            self.assertEqual(server.accounts.read_bytes(), b"")

    def test_over_128_simultaneous_connections(self):
        server = self.start()
        clients = []
        try:
            for _ in range(140):
                client = server.connect()
                clients.append(client)
                self.assertEqual(client.command("LOGOUT"), b"530 Not logged in")
            self.assertEqual(clients[0].command("LOGOUT"), b"530 Not logged in")
        finally:
            for client in clients:
                client.close()

    def test_descriptors_and_threads_reclaimed(self):
        server = self.start()
        fd_path = Path("/proc/%d/fd" % server.process.pid)
        task_path = Path("/proc/%d/task" % server.process.pid)
        initial_fds = len(list(fd_path.iterdir()))
        initial_threads = len(list(task_path.iterdir()))
        for index in range(160):
            with server.connect() as client:
                if index % 3:
                    self.assertEqual(client.command("NOPE"), b"500 Invalid or unknown command")
                else:
                    client.socket.sendall(b"incomplete")
        eventually(lambda: len(list(fd_path.iterdir())) == initial_fds and
                   len(list(task_path.iterdir())) == initial_threads)

    def test_random_malformed_input_stability(self):
        server = self.start()
        generator = random.Random(2337)
        with server.connect() as client:
            for _ in range(300):
                data = b"?" + bytes(generator.choice([c for c in range(256) if c != 10])
                                     for _ in range(generator.randrange(150)))
                self.assertEqual(client.command(data), b"500 Invalid or unknown command")
            self.assertEqual(client.command("HELLO 0 1"), b"200 OK")

    def test_graceful_shutdown_with_idle_and_partial_clients(self):
        for signum in (signal.SIGINT, signal.SIGTERM):
            server = self.start()
            clients = [server.connect() for _ in range(12)]
            try:
                for client in clients:
                    client.socket.sendall(b"partial")
                started = time.monotonic()
                server.stop(signum)
                self.assertLess(time.monotonic() - started, 5)
                for client in clients:
                    try:
                        self.assertEqual(client.socket.recv(1), b"")
                    except ConnectionResetError:
                        pass  # accept backlog sockets may close with unread data
            finally:
                for client in clients:
                    client.close()

    def test_repeated_startup_shutdown_same_port(self):
        port = available_port()
        for _ in range(6):
            server = self.start(port=port)
            with server.connect() as client:
                self.assertEqual(client.command("LOGOUT"), b"530 Not logged in")
            server.stop()

    def test_shutdown_during_account_operations(self):
        server = self.start()
        clients = [server.connect() for _ in range(6)]
        try:
            for index, client in enumerate(clients):
                client.socket.sendall(("REGISTER user_%d %s\r\n" % (index, PASSWORD)).encode())
            # The server must wait for any KDF/storage operations already started.
            # Requests still in the accept/receive queue may be discarded.
            time.sleep(0.02)
            server.stop()
            raw = server.accounts.read_bytes()
            self.assertTrue(not raw or raw.endswith(b"\n"))
            restarted = self.start()
            with restarted.connect() as client:
                for record in raw.splitlines():
                    username = record.split(b":", 1)[0].decode()
                    self.login(client, username)
                    self.assertEqual(client.command("LOGOUT"), b"200 OK")
        finally:
            for client in clients:
                client.close()


class ClientCoreTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="p2p-client-test-")
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.config = self.directory / "config.txt"
        self.server = Server(self.directory)
        self.addCleanup(self.server.stop)
        self.peer_port = available_port()

    def client_args(self, port=None, peer_port=None):
        port = self.server.port if port is None else port
        peer_port = self.peer_port if peer_port is None else peer_port
        return [str(CLIENT_BINARY), "--port", str(port), "--peer-port", str(peer_port)]

    def run_client(self, commands, *, port=None, peer_port=None):
        return subprocess.run(self.client_args(port, peer_port), cwd=self.directory,
                              input=commands, text=True, capture_output=True, timeout=20)

    def test_account_commands_and_reuse_of_saved_id(self):
        first = self.run_client(f"register alice\n{PASSWORD}\nlogin alice\nWrongPass!\n"
                                f"hello\nlogin alice\n{PASSWORD}\nlogout\nquit\n")
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertEqual(first.stdout.splitlines(), ["200 OK", "530 Invalid credentials",
                                                      "200 HELLO", "200 OK",
                                                      "200 HELLO", "200 LOGGED_OUT"])
        saved = self.config.read_text()
        self.assertRegex(saved, r"^[0-9]{1,10}\n\Z")

        with self.server.connect() as peer:
            self.assertEqual(peer.command(f"LOGIN alice {PASSWORD}"), b"200 OK")
            self.assertEqual(peer.command(f"HELLO {saved.strip()} 12345"), b"200 OK")
            second = self.run_client(f"login alice\n{PASSWORD}\nlogout\nquit\n")
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertEqual(second.stdout.splitlines(), ["550 Client ID in use", "530 Not logged in"])
        self.assertEqual(self.config.read_text(), saved)

    def test_cli_accepts_long_account_commands_and_passwords(self):
        username, password = "user_" + "x" * 5000, "!" * 6000
        result = self.run_client(f"register {username}\n{password}\nlogin {username}\n{password}\nquit\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.splitlines(), ["200 OK", "200 OK", "200 HELLO"])

    def test_script_does_not_treat_password_as_a_command_after_invalid_username(self):
        client = self.run_client(f"register bad-name\nquit\nregister alice\n{PASSWORD}\nquit\n")
        self.assertEqual(client.returncode, 0, client.stderr)
        self.assertEqual(client.stdout, "200 OK\n")
        self.assertIn("Username must contain", client.stderr)

    def test_hello_announces_a_reachable_listener_and_rejects_a_malformed_reply(self):
        self.config.write_text("4294967295\n")
        with socket.socket() as listener, concurrent.futures.ThreadPoolExecutor(1) as pool:
            listener.bind(("127.0.0.1", 0))
            listener.listen(1)
            listener.settimeout(5)
            peer_port = available_port()
            pending = pool.submit(self.run_client, f"login alice\n{PASSWORD}\nquit\n",
                                  port=listener.getsockname()[1], peer_port=peer_port)
            connection, _ = listener.accept()
            with connection, connection.makefile("rb") as requests:
                connection.settimeout(5)
                self.assertEqual(requests.readline(), f"HELLO 4294967295 {peer_port}\r\n".encode())
                with socket.create_connection(("127.0.0.1", peer_port), timeout=5):
                    pass
                connection.sendall(b"200 OK\x00ignored\r\n")
            client = pending.result()
        self.assertNotEqual(client.returncode, 0)
        self.assertEqual(client.stdout, "")
        self.assertIn("Invalid response", client.stderr)

    def test_client_fails_to_start_when_peer_port_is_in_use(self):
        with socket.socket() as occupied:
            occupied.bind(("127.0.0.1", 0))
            occupied.listen(1)
            client = self.run_client("quit\n", peer_port=occupied.getsockname()[1])
        self.assertNotEqual(client.returncode, 0)
        self.assertIn("Starting peer listener", client.stderr)
        self.assertEqual(client.stdout, "")

    def test_terminal_rejects_binary_password_and_restores_echo_on_interrupt(self):
        terminal, child_tty = pty.openpty()
        self.addCleanup(os.close, terminal)
        self.addCleanup(os.close, child_tty)

        def read_until(marker):
            deadline = time.monotonic() + 5
            output = bytearray()
            while marker not in output:
                readable, _, _ = select.select([terminal], [], [], max(0, deadline - time.monotonic()))
                self.assertTrue(readable, f"Missing terminal output {marker!r}: {output!r}")
                output.extend(os.read(terminal, 4096))
            return output

        with subprocess.Popen(self.client_args(), cwd=self.directory,
                              stdin=child_tty, stdout=child_tty,
                              stderr=child_tty) as client:
            try:
                read_until(b"p2p> ")
                os.write(terminal, b"register alice\n")
                read_until(b"Password: ")
                self.assertFalse(termios.tcgetattr(child_tty)[3] & termios.ECHO)
                os.write(terminal, PASSWORD.encode() + b"\x00junk\n\n")
                self.assertNotIn(PASSWORD.encode(), read_until(b"Password must contain"))
                self.assertTrue(termios.tcgetattr(child_tty)[3] & termios.ECHO)
                with self.server.connect() as peer:
                    self.assertEqual(peer.command(f"LOGIN alice {PASSWORD}"), b"530 Invalid credentials")

                os.write(terminal, b"login alice\n")
                read_until(b"Password: ")
                client.send_signal(signal.SIGINT)
                self.assertEqual(client.wait(timeout=5), 128 + signal.SIGINT)
                self.assertTrue(termios.tcgetattr(child_tty)[3] & termios.ECHO)
            finally:
                if client.poll() is None:
                    client.kill()

    def test_invalid_config_is_preserved(self):
        invalid = b"4294967296\n"
        self.config.write_bytes(invalid)
        client = self.run_client("quit\n")
        self.assertNotEqual(client.returncode, 0)
        self.assertIn("config.txt", client.stderr)
        self.assertEqual(self.config.read_bytes(), invalid)


if __name__ == "__main__":
    unittest.main(verbosity=2)
