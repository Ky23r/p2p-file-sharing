"""Exercise client behavior with controlled discovery replies and TCP peers."""

import concurrent.futures
from pathlib import Path
import queue
import socket
import subprocess
import tempfile
import threading
import time
import unittest

from control_fixture import ControlFixture, filename_token

BINARY = Path(__file__).resolve().parents[2] / "client"


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


class Client:
    def __init__(self, directory, server_port, identity=1):
        self.directory = Path(directory)
        self.directory.mkdir(exist_ok=True)
        if identity is not None:
            (self.directory / "config.txt").write_text(f"{identity}\n")
        self.peer_port = free_port()
        self.lines = queue.Queue()
        self.stderr = tempfile.TemporaryFile()
        self.process = subprocess.Popen(
            [str(BINARY), "--port", str(server_port), "--peer-port", str(self.peer_port)],
            cwd=self.directory, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=self.stderr, text=True, bufsize=1)
        self.thread = threading.Thread(target=self._read, daemon=True)
        self.thread.start()

    def _read(self):
        for line in self.process.stdout:
            self.lines.put(line.rstrip("\n"))
        self.lines.put(None)

    def send(self, text):
        self.process.stdin.write(text + "\n")
        self.process.stdin.flush()

    def expect(self, expected, timeout=8):
        line = self.lines.get(timeout=timeout)
        if line != expected:
            raise AssertionError(f"Expected {expected!r}, got {line!r}; stderr={self.errors()!r}")

    def search_results(self):
        rows = []
        while (line := self.lines.get(timeout=8)) != "200 END":
            if line is None:
                raise AssertionError(self.errors())
            rows.append(line)
        return rows

    def errors(self):
        self.stderr.seek(0)
        return self.stderr.read().decode(errors="replace")

    def login(self):
        self.send("login alice\nLocalTest!42")
        self.expect("200")
        self.expect("200 HELLO")

    def close(self):
        if self.stderr.closed:
            return
        if self.process.poll() is None:
            try:
                self.send("quit")
            except (BrokenPipeError, ValueError):
                pass
        try:
            self.process.wait(timeout=8)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
            raise AssertionError("client did not stop its workers")
        finally:
            try:
                self.process.stdin.close()
            except BrokenPipeError:
                pass
            self.thread.join(timeout=2)
            self.process.stdout.close()
            self.stderr.close()


class PeerFixture:
    """A one-shot real TCP peer for adversarial transfer framing."""

    def __init__(self, response, fragments=False):
        self.response = response
        self.fragments = fragments
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)
        self.listener.settimeout(5)
        self.port = self.listener.getsockname()[1]
        self.request = None
        self.error = None
        self.thread = threading.Thread(target=self._serve, daemon=True)
        self.thread.start()

    def _serve(self):
        try:
            connection, _ = self.listener.accept()
            with connection, connection.makefile("rb") as requests:
                connection.settimeout(5)
                self.request = requests.readline()
                if self.fragments:
                    for position in range(0, len(self.response), 7):
                        connection.sendall(self.response[position:position + 7])
                        time.sleep(0.001)
                else:
                    connection.sendall(self.response)
        except (ConnectionResetError, BrokenPipeError):
            pass  # Rejecting a bad header may close before the fixture finishes.
        except Exception as error:
            self.error = error

    def close(self):
        self.thread.join(timeout=6)
        self.listener.close()
        if self.thread.is_alive():
            raise AssertionError("peer fixture did not finish")
        if self.error is not None:
            raise self.error


class ClientFileTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="p2p-transfer-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.server = ControlFixture()
        self.addCleanup(self.server.close)
        self.next_id = 0

    def client(self):
        self.next_id += 1
        client = Client(self.directory / str(self.next_id), self.server.port, self.next_id)
        self.addCleanup(client.close)
        return client

    def share(self, client, name, contents):
        (client.directory / name).write_bytes(contents)
        client.send("share " + name)
        client.expect("200 SHARED")

    def peer_get(self, client, name, request=None):
        with socket.create_connection(("127.0.0.1", client.peer_port), timeout=4) as peer:
            peer.settimeout(4)
            peer.sendall(request if request is not None else f"GET {filename_token(name)}\r\n".encode())
            response = bytearray()
            while True:
                block = peer.recv(65536)
                if not block:
                    return bytes(response)
                response.extend(block)

    def test_peer_upload_binary_empty_and_parallel_requests(self):
        uploader = self.client()
        uploader.login()
        contents = bytes(range(256)) * 4097
        self.share(uploader, "report +%.bin", contents)
        self.share(uploader, "empty", b"")
        self.assertEqual(self.peer_get(uploader, "empty"), b"200 0\r\n")
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            responses = list(pool.map(lambda _: self.peer_get(uploader, "report +%.bin"), range(8)))
        for response in responses:
            header, body = response.split(b"\r\n", 1)
            self.assertEqual(header, f"200 {len(contents)}".encode())
            self.assertEqual(body, contents)

    def test_discovery_download_unshare_and_logout(self):
        uploader, downloader = self.client(), self.client()
        uploader.login()
        downloader.login()
        contents = b"hello\0world\n" * 10000
        self.share(uploader, "report.bin", contents)
        downloader.send("search report.bin")
        row, = downloader.search_results()
        self.assertIn("report.bin", row)
        downloader.send("download 1")
        downloader.expect("Downloaded report.bin")
        self.assertEqual((downloader.directory / "report.bin").read_bytes(), contents)
        uploader.send("unshare report.bin")
        uploader.expect("200 UNSHARED")
        downloader.send("search report.bin")
        self.assertEqual(downloader.search_results(), [])
        self.assertEqual(self.peer_get(uploader, "report.bin"), b"404\r\n")
        uploader.send("logout")
        uploader.expect("200 LOGGED_OUT")

    def test_background_control_failure_exits_without_terminal_input(self):
        client = self.client()
        client.login()
        self.share(client, "file.bin", b"data")
        with self.server.lock:
            handler = self.server.peers[1][0]
            handler.request.shutdown(socket.SHUT_RDWR)
        self.assertEqual(client.process.wait(timeout=13), 1)
        self.assertIn("control connection failed", client.errors())

    def result(self, peer_port, size, name="file.bin", identity=90):
        self.server.search_response = (f"FILE {filename_token(name)} {size} "
                                       f"{identity} 127.0.0.1 {peer_port}\r\n200 END\r\n").encode()

    def search(self, client, query="file.bin"):
        client.send("search " + query)
        row, = client.search_results()
        self.assertIn("1:", row)

    def test_download_coalesced_header_binary_body_and_empty_file(self):
        client = self.client()
        client.login()
        for contents, name in ((b"a\0\nb\xff", "file.bin"), (b"", "file.empty")):
            peer = PeerFixture(f"200 {len(contents)}\r\n".encode() + contents)
            self.addCleanup(peer.close)
            self.result(peer.port, len(contents), name)
            self.search(client, name)
            client.send("download 1")
            client.expect("Downloaded " + name)
            self.assertEqual((client.directory / name).read_bytes(), contents)
        self.assertEqual(list(client.directory.glob(".p2p-download-*")), [])

    def test_download_fragmented_header_and_body(self):
        client = self.client()
        client.login()
        contents = bytes(range(256)) * 8
        peer = PeerFixture(f"200 {len(contents)}\r\n".encode() + contents, fragments=True)
        self.addCleanup(peer.close)
        self.result(peer.port, len(contents))
        self.search(client)
        client.send("download 1")
        client.expect("Downloaded file.bin")
        self.assertEqual((client.directory / "file.bin").read_bytes(), contents)

    def test_remote_failures_remove_partial_downloads(self):
        client = self.client()
        client.login()
        cases = [
            (b"200 4\r\nab", "TRANSFER_FAILED"),
            (b"200 4\r\nabcde", "SIZE_MISMATCH"),
            (b"200 3\r\nabc", "SIZE_MISMATCH"),
            (b"200 4\x00ignored\r\nabcd", "TRANSFER_FAILED"),
            (b"200 -4\r\n", "TRANSFER_FAILED"),
            (b"200 18446744073709551616\r\n", "TRANSFER_FAILED"),
            (b"200 4\nabcd", "TRANSFER_FAILED"),
            (b"404\r\n", "NOT_FOUND"),
            (b"409\r\n", "SIZE_MISMATCH"),
            (b"x" * 4097 + b"\r\n", "TRANSFER_FAILED"),
        ]
        for response, reason in cases:
            with self.subTest(reason=reason, response=response[:40]):
                peer = PeerFixture(response)
                self.addCleanup(peer.close)
                self.result(peer.port, 4)
                self.search(client)
                offset = len(client.errors())
                client.send("download 1\nhello")
                client.expect("200 HELLO")
                self.assertIn(f"failed: {reason}", client.errors()[offset:])
                self.assertFalse((client.directory / "file.bin").exists())
                self.assertEqual(list(client.directory.glob(".p2p-download-*")), [])

    def test_refused_peer_connection_keeps_control_usable(self):
        client = self.client()
        client.login()
        self.result(free_port(), 4)
        self.search(client)
        client.send("download 1\nlogout")
        client.expect("200 LOGGED_OUT")
        self.assertIn("failed: CONNECT_FAILED", client.errors())

    def test_download_never_overwrites_existing_file_or_symlink(self):
        client = self.client()
        client.login()
        destination = client.directory / "file.bin"
        destination.write_bytes(b"keep me")
        self.result(free_port(), 4)
        self.search(client)
        client.send("download 1\nlogout")
        client.expect("200 LOGGED_OUT")
        self.assertEqual(destination.read_bytes(), b"keep me")
        destination.unlink()
        target = client.directory / "target"
        target.write_bytes(b"keep target")
        destination.symlink_to(target)
        client.login()
        self.search(client)
        client.send("download 1\nlogout")
        client.expect("200 LOGGED_OUT")
        self.assertTrue(destination.is_symlink())
        self.assertEqual(target.read_bytes(), b"keep target")
        self.assertEqual(list(client.directory.glob(".p2p-download-*")), [])

    def test_logout_relogin_and_restart_republish_local_catalog(self):
        client = self.client()
        client.login()
        self.share(client, "file.bin", b"data")
        saved_id = (client.directory / "config.txt").read_bytes()
        client.send("logout")
        client.expect("200 LOGGED_OUT")
        self.assertEqual(self.peer_get(client, "file.bin"), b"404\r\n")
        client.login()
        client.expect("200 SHARED")
        self.assertEqual(self.peer_get(client, "file.bin"), b"200 4\r\ndata")
        client.close()
        deadline = time.monotonic() + 3
        while 1 in self.server.peers and time.monotonic() < deadline:
            time.sleep(0.01)
        restarted = Client(client.directory, self.server.port, identity=None)
        self.addCleanup(restarted.close)
        restarted.login()
        restarted.expect("200 SHARED")
        self.assertEqual((client.directory / "config.txt").read_bytes(), saved_id)
        self.assertEqual(self.peer_get(restarted, "file.bin"), b"200 4\r\ndata")

    def test_pending_share_is_not_served_and_manual_hello_retries(self):
        client = self.client()
        (client.directory / "file.bin").write_bytes(b"data")
        client.send("share file.bin")
        client.expect("Saved file.bin")
        self.assertEqual(self.peer_get(client, "file.bin"), b"404\r\n")
        self.server.share_response = b"500\r\n"
        client.login()
        client.expect("500")
        self.assertEqual(self.peer_get(client, "file.bin"), b"404\r\n")
        self.server.share_response = b"200\r\n"
        client.send("hello")
        client.expect("200 HELLO")
        client.expect("200 SHARED")
        self.assertEqual(self.peer_get(client, "file.bin"), b"200 4\r\ndata")

    def test_upload_rejects_bad_requests_changed_missing_and_symlink_files(self):
        client = self.client()
        client.login()
        self.share(client, "file.bin", b"data")
        for request in (b"GET ..\r\n", b"GET a%2Fb\r\n", b"GET %00\r\n", b"GET %66ile.bin\r\n",
                        b"GET  file.bin\r\n", b"GET file.bin\n", b"GET\x00 x\r\n"):
            self.assertEqual(self.peer_get(client, "file.bin", request), b"400\r\n")
        self.assertEqual(self.peer_get(client, "file.bin", b"PUT file.bin\r\n"), b"501\r\n")
        source = client.directory / "file.bin"
        source.write_bytes(b"changed size")
        self.assertEqual(self.peer_get(client, "file.bin"), b"409\r\n")
        source.unlink()
        self.assertEqual(self.peer_get(client, "file.bin"), b"404\r\n")
        outside = self.directory / "outside"
        outside.write_bytes(b"secret")
        source.symlink_to(outside)
        self.assertEqual(self.peer_get(client, "file.bin"), b"404\r\n")

    def test_invalid_search_results_close_control_and_disable_serving(self):
        row = b"FILE file.bin 4 90 127.0.0.1 9001\r\n"
        end = b"200 END\r\n"
        responses = [b"200\r\n", b"200 END\n", b"200 END\x00bad\r\n",
                     row + row + end, row + row.replace(b" 90 ", b" 9 ") + end,
                     row.replace(b"127.0.0.1", b"999.1.1.1") + end,
                     row.replace(b"9001", b"0") + end,
                     row.replace(b"file.bin", b"..") + end,
                     row.replace(b"file.bin", b"file.bin.backup") + end,
                     row.replace(b" 4 ", b" 18446744073709551616 ") + end,
                     row + b"500\r\n"]
        for response in responses:
            with self.subTest(response=response):
                client = self.client()
                client.login()
                self.share(client, "file.bin", b"data")
                self.server.search_response = response
                client.send("search file.bin")
                client.expect(None)
                self.assertEqual(client.process.wait(timeout=5), 1)

    def test_truncated_search_result_is_not_available_for_download(self):
        client = self.client()
        client.login()
        self.server.search_response = b"FILE file.bin 4 90 127.0.0.1 9001\r\n"
        self.server.close_after_search = True
        client.send("search file.bin")
        client.expect(None)
        self.assertEqual(client.process.wait(timeout=5), 1)

    def test_zero_results_clear_the_previous_download_selection(self):
        client = self.client()
        client.login()
        self.result(free_port(), 4)
        self.search(client)
        self.server.search_response = b"200 END\r\n"
        client.send("search missing")
        self.assertEqual(client.search_results(), [])
        client.send("download 1\nlogout")
        client.expect("200 LOGGED_OUT")
        self.assertIn("valid result number", client.errors())

    def test_keepalive_preserves_announcements_while_terminal_is_idle(self):
        client = self.client()
        client.login()
        self.share(client, "file.bin", b"data")
        deadline = time.monotonic() + 13
        while time.monotonic() < deadline:
            with self.server.lock:
                hellos = sum(line.startswith(b"HELLO ") for line in self.server.requests)
            if hellos >= 2:
                break
            time.sleep(0.02)
        self.assertGreaterEqual(hellos, 2)
        with self.server.lock:
            self.assertIn(b"file.bin", self.server.peers[1][2])
        self.assertEqual(self.peer_get(client, "file.bin"), b"200 4\r\ndata")

    def test_rejected_keepalive_preserves_serving_and_automatic_retry(self):
        self.server.hello_responses = [b"200\r\n", b"500\r\n", b"200\r\n"]
        client = self.client()
        client.login()
        self.share(client, "file.bin", b"data")

        def await_hellos(count):
            deadline = time.monotonic() + 13
            while time.monotonic() < deadline:
                with self.server.lock:
                    received = sum(line.startswith(b"HELLO ") for line in self.server.requests)
                if received >= count:
                    return
                time.sleep(0.02)
            self.fail(f"Expected {count} HELLO requests, received {received}")

        await_hellos(2)
        client.send("shares")
        client.expect("SHARE file.bin 4 ANNOUNCED")
        self.assertEqual(self.peer_get(client, "file.bin"), b"200 4\r\ndata")
        self.assertIn("500", client.errors())
        with self.server.lock:
            self.assertIn(b"file.bin", self.server.peers[1][2])
        await_hellos(3)
        self.assertEqual(self.peer_get(client, "file.bin"), b"200 4\r\ndata")

    def test_quit_joins_workers_with_partial_requests(self):
        client = self.client()
        client.login()
        peers = [socket.create_connection(("127.0.0.1", client.peer_port), timeout=4) for _ in range(4)]
        for peer in peers:
            self.addCleanup(peer.close)
            peer.sendall(b"GET partial")
        client.send("quit")
        self.assertEqual(client.process.wait(timeout=5), 0)

    def test_rejected_logout_keeps_announced_files_available(self):
        client = self.client()
        client.login()
        self.share(client, "file.bin", b"data")
        self.server.logout_response = b"500\r\n"
        client.send("logout")
        client.expect("500")
        self.assertEqual(self.peer_get(client, "file.bin"), b"200 4\r\ndata")

    def test_explicit_hello_withdraws_a_missing_previous_share(self):
        client = self.client()
        client.login()
        self.share(client, "file.bin", b"data")
        (client.directory / "file.bin").unlink()
        client.send("hello\nsearch file.bin")
        client.expect("200 HELLO")
        client.expect("200 UNSHARED")
        self.assertEqual(client.search_results(), [])

    def test_selects_between_peers_with_same_filename(self):
        first, second, downloader = self.client(), self.client(), self.client()
        for client in (first, second, downloader):
            client.login()
        self.share(first, "file with spaces.bin", b"first")
        self.share(second, "file with spaces.bin", b"second")
        downloader.send("search file with spaces.bin")
        first_row, second_row = downloader.search_results()
        self.assertIn("client 1,", first_row)
        self.assertIn("client 2,", second_row)
        downloader.send("download 2")
        downloader.expect("Downloaded file with spaces.bin")
        self.assertEqual((downloader.directory / "file with spaces.bin").read_bytes(), b"second")

    def test_search_accepts_more_than_128_results(self):
        client = self.client()
        client.login()
        rows = "".join(f"FILE file.bin 4 {identity} 127.0.0.1 9001\r\n" for identity in range(1, 141))
        self.server.search_response = (rows + "200 END\r\n").encode()
        client.send("search file.bin")
        results = client.search_results()
        self.assertEqual(len(results), 140)
        self.assertTrue(results[-1].startswith("140: file.bin (client 140,"))

    def test_catalog_loads_and_updates_more_than_256_entries(self):
        directory = self.directory / "large-catalog"
        directory.mkdir()
        entries = [directory / f"file-{number}.bin" for number in range(260)]
        catalog = directory / "index.txt"
        catalog.write_text("".join(f"{path.name.encode().hex()}\t{str(path).encode().hex()}\n" for path in entries))
        added = directory / "new.bin"
        added.write_bytes(b"data")
        client = Client(directory, self.server.port)
        self.addCleanup(client.close)
        client.send("share new.bin")
        client.expect("Saved new.bin")
        self.assertEqual(len(catalog.read_bytes().splitlines()), 261)

    def test_catalog_records_larger_than_wire_header_survive_restart(self):
        client = self.client()
        nested = client.directory
        for _ in range(45):
            nested /= "long-directory-name-" + "x" * 32
            nested.mkdir()
        source = nested / "file.bin"
        source.write_bytes(b"data")
        client.send("share " + str(source))
        client.expect("Saved file.bin")
        self.assertGreater((client.directory / "index.txt").stat().st_size, 4096)
        client.close()
        restarted = Client(client.directory, self.server.port, identity=None)
        self.addCleanup(restarted.close)
        restarted.login()
        restarted.expect("200 SHARED")
        self.assertEqual(self.peer_get(restarted, "file.bin"), b"200 4\r\ndata")

    def test_failed_catalog_removal_disables_serving_and_warns_before_republish(self):
        client = self.client()
        client.login()
        self.share(client, "file.bin", b"data")
        catalog = client.directory / "index.txt"
        backup = client.directory / "index.saved"
        catalog.rename(backup)
        catalog.mkdir()  # Atomic replacement must fail instead of deleting this directory.
        client.send("unshare file.bin")
        client.send("shares")
        client.expect("SHARE file.bin 4 PENDING")
        self.assertEqual(self.peer_get(client, "file.bin"), b"404\r\n")
        self.assertIn("Retry unshare before hello/login", client.errors())
        self.assertTrue(catalog.is_dir())
        catalog.rmdir()
        backup.rename(catalog)
        client.send("unshare file.bin")
        client.expect("200 UNSHARED")
        self.assertEqual(catalog.read_bytes(), b"")

    def test_failed_reannouncement_withdraws_previous_server_metadata(self):
        client = self.client()
        client.login()
        self.share(client, "file.bin", b"data")
        (client.directory / "file.bin").write_bytes(b"different length")
        self.server.share_response = b"500\r\n"
        client.send("share file.bin")
        client.expect("500")
        client.expect("200 UNSHARED")
        client.send("search file.bin")
        self.assertEqual(client.search_results(), [])
        self.assertEqual(self.peer_get(client, "file.bin"), b"404\r\n")


if __name__ == "__main__":
    unittest.main(verbosity=2)
