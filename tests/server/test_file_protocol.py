"""Control protocol and file-index tests against the real TCP server."""

import concurrent.futures
from pathlib import Path
import re
import socket
import stat
import struct
import subprocess
import tempfile
import threading
import unittest

from test_server import BINARY, PASSWORD, Server, available_port, eventually


class FileProtocolTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="p2p-file-protocol-")
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.server = Server(self.directory)
        self.addCleanup(self.server.stop)
        with self.server.connect() as client:
            self.assertEqual(client.command(f"REGISTER alice {PASSWORD}"), b"200 OK")
        self.owner = self.peer(20, 9020)
        self.viewer = self.peer(10, 9010)

    def peer(self, client_id, port=9001):
        client = self.server.connect()
        self.addCleanup(client.close)
        self.assertEqual(client.command(f"LOGIN alice {PASSWORD}"), b"200 OK")
        self.assertEqual(client.command(f"HELLO {client_id} {port}"), b"200 OK")
        return client

    def search(self, client, filename="notes.txt"):
        client.socket.sendall(f"SEARCH {filename}\r\n".encode())
        results = []
        while (line := client.line()) != b"200 OK":
            self.assertTrue(line.startswith(b"200-"), line)
            results.append(line[4:].split())
        for record in results:
            self.assertEqual(len(record), 5)
            self.assertEqual(record[0], filename.encode())
            self.assertEqual(record[3], b"127.0.0.1")
        ids = [int(record[2]) for record in results]
        self.assertEqual(ids, sorted(set(ids)))
        return results

    def test_authentication_hello_and_validation_precedence(self):
        with self.server.connect() as client:
            commands = ["SHARE 20 notes.txt 1", "UNSHARE notes.txt", "SEARCH notes.txt"]
            for command in commands:
                self.assertEqual(client.command(command), b"530 Not logged in")
            self.assertEqual(client.command(f"LOGIN alice {PASSWORD}"), b"200 OK")
            for command in commands:
                self.assertEqual(client.command(command), b"503 Bad command sequence")
            self.assertEqual(client.command("SHARE 20 ../bad -1"), b"501 Invalid arguments")

    def test_startup_hello_stages_identity_until_login(self):
        with self.server.connect() as first, self.server.connect() as second:
            self.assertEqual(first.command("HELLO 30 9030"), b"200 OK")
            self.assertEqual(first.command("SHARE 30 notes.txt 1"), b"530 Not logged in")
            self.assertEqual(first.command("LOGIN alice WrongPass!"), b"530 Invalid credentials")
            self.assertEqual(second.command("HELLO 30 9031"), b"200 OK")
            self.assertEqual(second.command(f"LOGIN alice {PASSWORD}"), b"200 OK")
            self.assertEqual(first.command(f"LOGIN alice {PASSWORD}"), b"550 Client ID in use")
            self.assertEqual(first.command("SEARCH notes.txt"), b"530 Not logged in")
            self.assertEqual(second.command("SHARE 31 notes.txt 1"), b"550 Client ID does not match session")
            self.assertEqual(second.command("SHARE 30 notes.txt 1"), b"200 OK")
            self.assertEqual(self.search(self.viewer)[0][4], b"9031")
            self.assertEqual(second.command("LOGOUT"), b"200 OK")
            self.assertEqual(first.command(f"LOGIN alice {PASSWORD}"), b"200 OK")
            self.assertEqual(first.command("SHARE 30 notes.txt 2"), b"200 OK")
            self.assertEqual(self.search(self.viewer)[0][4], b"9030")

    def test_share_update_search_and_owner_only_removal(self):
        self.assertEqual(self.owner.command("SHARE 20 notes.txt 0"), b"200 OK")
        row, = self.search(self.viewer)
        self.assertEqual(row, [b"notes.txt", b"0", b"20", b"127.0.0.1", b"9020"])
        self.assertEqual(self.viewer.command("UNSHARE notes.txt"), b"550 File not shared")
        self.assertEqual(self.owner.command("SHARE 20 notes.txt 18446744073709551615"), b"200 OK")
        self.assertEqual(self.search(self.viewer)[0][1], b"18446744073709551615")
        self.assertEqual(self.owner.command("UNSHARE notes.txt"), b"200 OK")
        self.assertEqual(self.search(self.viewer), [])
        self.assertEqual(self.owner.command("UNSHARE notes.txt"), b"550 File not shared")

    def test_multiple_peers_numeric_order_and_exact_matching(self):
        maximum = self.peer(4294967295)
        zero = self.peer(0)
        for client, identity in ((maximum, 4294967295), (self.owner, 20), (zero, 0), (self.viewer, 10)):
            self.assertEqual(client.command(f"SHARE {identity} notes.txt 42"), b"200 OK")
        self.assertEqual([int(row[2]) for row in self.search(self.viewer)], [0, 10, 20, 4294967295])
        self.assertEqual(self.search(self.viewer, "Notes.txt"), [])
        self.assertEqual(self.search(self.viewer, "notes"), [])
        self.assertEqual(self.search(self.viewer, "%2A"), [])

    def test_encoded_filename_and_maximum_length(self):
        for name in ("my%20notes.txt", "100%25.txt", "a%2Bb.txt", "b%C3%A1o%20c%C3%A1o.txt", "%20" * 255):
            self.assertEqual(self.owner.command(f"SHARE 20 {name} 0005"), b"200 OK")
            self.assertEqual(self.search(self.viewer, name)[0][1], b"5")
            self.assertEqual(self.owner.command(f"UNSHARE {name}"), b"200 OK")

    def test_malformed_file_requests_and_connection_recovery(self):
        names = [".", "..", "../file", "a/b", "a\\b", "%2F", "%5C", "%00", "%0A", "%7F",
                 "%41", "%2E", "%2e", "%2b", "%", "%2", "%GG", "a+b", "x" * 256, "%20" * 256]
        cases = [f"SHARE 20 {name} 1" for name in names]
        cases += [f"SHARE 20 notes.txt {size}" for size in ("-1", "+1", "1x", "18446744073709551616")]
        cases += [f"SHARE {identity} notes.txt 1" for identity in ("-1", "+20", "4294967296")]
        cases += ["SHARE", "SHARE notes.txt", "SHARE notes.txt 1", "SHARE 20 notes.txt 1 extra",
                  "SEARCH", "SEARCH a b",
                  "UNSHARE", "UNSHARE a b",
                  "SHARE  20 notes.txt 1", "SHARE 20 notes.txt 1 "]
        for request in cases:
            with self.subTest(request=request[:80]):
                self.assertEqual(self.owner.command(request), b"501 Invalid arguments")
        for request in ("NOPE", "NOPE a b", "NOPE a b c d e"):
            self.assertEqual(self.owner.command(request), b"500 Invalid or unknown command")
        self.assertEqual(self.owner.command("SHARE 20 valid.txt 1"), b"200 OK")

    def test_over_128_shares_support_updates_and_removal(self):
        for i in range(140):
            self.assertEqual(self.owner.command(f"SHARE 20 file{i} 1"), b"200 OK")
        self.assertEqual(self.owner.command("SHARE 20 file0 2"), b"200 OK")
        self.assertEqual(self.search(self.viewer, "file0")[0][1], b"2")
        self.assertEqual(self.owner.command("UNSHARE file0"), b"200 OK")
        self.assertEqual(self.owner.command("SHARE 20 extra 1"), b"200 OK")

    def test_hello_updates_and_rejected_identity_preserve_shares(self):
        self.assertEqual(self.owner.command("SHARE 20 notes.txt 1"), b"200 OK")
        self.assertEqual(self.owner.command("HELLO 20 9999"), b"200 OK")
        self.assertEqual(self.search(self.viewer)[0][4], b"9999")
        self.assertEqual(self.owner.command("HELLO 10 8000"), b"550 Client ID in use")
        self.assertEqual(self.search(self.viewer)[0][4], b"9999")
        self.assertEqual(self.owner.command("HELLO 30 8000"), b"200 OK")
        self.assertEqual(self.search(self.viewer), [])
        self.assertEqual(self.owner.command("SHARE 30 notes.txt 2"), b"200 OK")
        self.assertEqual(self.search(self.viewer)[0][2], b"30")

    def test_logout_clears_shares_and_requires_registration_after_login(self):
        self.assertEqual(self.owner.command("SHARE 20 notes.txt 1"), b"200 OK")
        self.assertEqual(self.owner.command("LOGOUT"), b"200 OK")
        self.assertEqual(self.search(self.viewer), [])
        self.assertEqual(self.owner.command(f"LOGIN alice {PASSWORD}"), b"200 OK")
        self.assertEqual(self.owner.command("SHARE 20 notes.txt 1"), b"503 Bad command sequence")
        self.assertEqual(self.owner.command("HELLO 20 9020"), b"200 OK")
        self.assertEqual(self.search(self.viewer), [])

    def test_abrupt_disconnect_cleans_index_before_id_reuse(self):
        self.assertEqual(self.owner.command("SHARE 20 notes.txt 1"), b"200 OK")
        self.owner.socket.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        self.owner.close()
        eventually(lambda: self.search(self.viewer) == [])
        replacement = self.peer(20)
        self.assertEqual(self.search(replacement), [])

    def test_restart_keeps_accounts_and_discards_shares(self):
        self.assertEqual(self.owner.command("SHARE 20 notes.txt 1"), b"200 OK")
        self.server.stop()
        self.server = Server(self.directory)
        self.addCleanup(self.server.stop)
        replacement = self.peer(20)
        self.assertEqual(self.search(replacement), [])

    def test_search_end_marker_with_fragmented_and_pipelined_requests(self):
        self.assertEqual(self.owner.command("SHARE 20 notes.txt 42"), b"200 OK")
        self.viewer.socket.sendall(b"SEARCH notes.txt\r")
        self.viewer.socket.sendall(b"\nSEARCH absent\r\nLOGOUT\r\n")
        self.assertEqual([self.viewer.line() for _ in range(4)],
                         [b"200-notes.txt 42 20 127.0.0.1 9020", b"200 OK", b"200 OK", b"200 OK"])

    def test_search_results_larger_than_a_network_read_buffer(self):
        filename = "%20" * 255
        peers = [self.owner, self.viewer] + [self.peer(i + 100) for i in range(4)]
        for client, identity in zip(peers, (20, 10, 100, 101, 102, 103)):
            self.assertEqual(client.command(f"SHARE {identity} {filename} 18446744073709551615"), b"200 OK")
        self.viewer.socket.sendall(f"SEARCH {filename}\r\nSEARCH missing\r\n".encode())
        rows = [self.viewer.line() for _ in peers]
        self.assertGreater(sum(len(row) + 2 for row in rows), 4096)
        self.assertEqual([int(row[4:].split()[2]) for row in rows], [10, 20, 100, 101, 102, 103])
        self.assertTrue(all(row.startswith(f"200-{filename} ".encode()) for row in rows))
        self.assertEqual(self.viewer.line(), b"200 OK")
        self.assertEqual(self.viewer.line(), b"200 OK")

    def test_search_snapshot_survives_concurrent_registration_and_cleanup(self):
        peers = [self.peer(i + 100, i + 9100) for i in range(6)]
        barrier = threading.Barrier(len(peers))

        def worker(pair):
            index, client = pair
            barrier.wait(timeout=10)
            for size in range(30):
                self.assertEqual(client.command(f"SHARE {index + 100} notes.txt {size}"), b"200 OK")
                rows = self.search(client)
                self.assertTrue(any(int(row[2]) == index + 100 for row in rows))
                self.assertEqual(client.command("UNSHARE notes.txt"), b"200 OK")
            self.assertEqual(client.command(f"SHARE {index + 100} notes.txt 99"), b"200 OK")
            if index % 2:
                self.assertEqual(client.command("LOGOUT"), b"200 OK")
            else:
                self.assertEqual(client.command(f"HELLO {index + 200} 9999"), b"200 OK")

        with concurrent.futures.ThreadPoolExecutor(max_workers=len(peers)) as pool:
            list(pool.map(worker, enumerate(peers)))
        self.assertEqual(self.search(self.viewer), [])

    def test_log_is_private_append_only_and_excludes_credentials_and_raw_input(self):
        marker = "NeverLogThis!"
        self.assertEqual(self.owner.command("LOGOUT"), b"200 OK")
        self.assertEqual(self.owner.command(f"LOGIN alice {marker}"), b"530 Invalid credentials")
        self.assertEqual(self.owner.command(f"LOGIN alice {marker} extra"), b"501 Invalid arguments")
        self.assertEqual(self.owner.command(marker), b"500 Invalid or unknown command")
        self.server.stop()
        path = self.directory / "accounts.db.log"
        original = path.read_bytes()
        self.assertNotIn(PASSWORD.encode(), original)
        self.assertNotIn(marker.encode(), original)
        self.assertIn(b"command=LOGIN result=530", original)
        self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)
        self.assertTrue(all(re.match(rb"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ ", line)
                            for line in original.splitlines()))
        self.server = Server(self.directory)
        self.addCleanup(self.server.stop)
        self.server.stop()
        self.assertTrue(path.read_bytes().startswith(original))

    def test_custom_log_and_unsafe_log_paths(self):
        custom = self.directory / "custom.log"
        custom_server = Server(self.directory, accounts=self.directory / "custom.db", log=custom)
        self.addCleanup(custom_server.stop)
        with custom_server.connect() as client:
            self.assertEqual(client.command("LOGOUT"), b"530 Not logged in")
        custom_server.stop()
        self.assertIn("command=LOGOUT result=530", custom.read_text())
        self.assertFalse(self.directory.joinpath("custom.db.log").exists())

        def run(log_path):
            return subprocess.run([str(BINARY), "--port", str(available_port()), "--accounts",
                                   str(self.directory / "other.db"), "--log", str(log_path)],
                                  cwd=self.directory, capture_output=True, timeout=10)

        account_copy = self.directory / "other.db"
        account_copy.write_bytes(self.server.accounts.read_bytes())
        before = account_copy.read_bytes()
        result = run(account_copy)
        self.assertEqual(result.returncode, 1)
        self.assertIn(b"service log", result.stderr)
        self.assertEqual(account_copy.read_bytes(), before)
        symlink = self.directory / "linked.log"
        symlink.symlink_to(self.server.accounts)
        for path in (symlink, self.directory, self.directory / "missing" / "log"):
            result = run(path)
            self.assertEqual(result.returncode, 1)
            self.assertIn(b"service log", result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
