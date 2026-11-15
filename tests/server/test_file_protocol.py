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
            self.assertEqual(client.command(f"REGISTER alice {PASSWORD}"), b"OK REGISTERED")
        self.owner = self.peer(20, 9020)
        self.viewer = self.peer(10, 9010)

    def peer(self, client_id, port=9001):
        client = self.server.connect()
        self.addCleanup(client.close)
        self.assertEqual(client.command(f"LOGIN alice {PASSWORD}"), b"OK LOGGED_IN")
        self.assertEqual(client.command(f"HELLO {client_id} {port}"), b"OK HELLO")
        return client

    def search(self, client, filename="notes.txt"):
        header = client.command(f"SEARCH {filename}")
        self.assertRegex(header, rb"^OK RESULTS [0-9]+$")
        count = int(header.split()[2])
        self.assertLessEqual(count, 128)
        results = [client.line().split() for _ in range(count)]
        for record in results:
            self.assertEqual(len(record), 6)
            self.assertEqual(record[0], b"FILE")
            self.assertEqual(record[1], filename.encode())
            self.assertEqual(record[4], b"127.0.0.1")
        ids = [int(record[3]) for record in results]
        self.assertEqual(ids, sorted(set(ids)))
        return results

    def test_authentication_hello_and_validation_precedence(self):
        with self.server.connect() as client:
            commands = ["SHARE notes.txt 1", "UNSHARE notes.txt", "SEARCH notes.txt",
                        "REPORT 20 notes.txt TIMEOUT"]
            for command in commands:
                self.assertEqual(client.command(command), b"ERR AUTH_REQUIRED")
            self.assertEqual(client.command(f"LOGIN alice {PASSWORD}"), b"OK LOGGED_IN")
            for command in commands:
                self.assertEqual(client.command(command), b"ERR HELLO_REQUIRED")
            self.assertEqual(client.command("SHARE ../bad -1"), b"ERR INVALID_REQUEST")
            self.assertEqual(client.command("REPORT 20 notes.txt UNKNOWN"), b"ERR INVALID_REQUEST")

    def test_share_update_search_and_owner_only_removal(self):
        self.assertEqual(self.owner.command("SHARE notes.txt 0"), b"OK SHARED")
        self.assertEqual(self.search(self.viewer),
                         [[b"FILE", b"notes.txt", b"0", b"20", b"127.0.0.1", b"9020"]])
        self.assertEqual(self.viewer.command("UNSHARE notes.txt"), b"ERR FILE_NOT_FOUND")
        self.assertEqual(self.owner.command("SHARE notes.txt 18446744073709551615"), b"OK SHARED")
        self.assertEqual(self.search(self.viewer)[0][2], b"18446744073709551615")
        self.assertEqual(self.owner.command("UNSHARE notes.txt"), b"OK UNSHARED")
        self.assertEqual(self.search(self.viewer), [])
        self.assertEqual(self.owner.command("UNSHARE notes.txt"), b"ERR FILE_NOT_FOUND")

    def test_multiple_peers_numeric_order_and_exact_matching(self):
        maximum = self.peer(4294967295)
        zero = self.peer(0)
        for client in (maximum, self.owner, zero, self.viewer):
            self.assertEqual(client.command("SHARE notes.txt 42"), b"OK SHARED")
        self.assertEqual([int(row[3]) for row in self.search(self.viewer)], [0, 10, 20, 4294967295])
        self.assertEqual(self.search(self.viewer, "Notes.txt"), [])
        self.assertEqual(self.search(self.viewer, "notes"), [])
        self.assertEqual(self.search(self.viewer, "%2A"), [])

    def test_encoded_filename_and_maximum_length(self):
        for name in ("my%20notes.txt", "100%25.txt", "a%2Bb.txt", "%20" * 255):
            self.assertEqual(self.owner.command(f"SHARE {name} 0005"), b"OK SHARED")
            self.assertEqual(self.search(self.viewer, name)[0][2], b"5")
            self.assertEqual(self.owner.command(f"UNSHARE {name}"), b"OK UNSHARED")

    def test_malformed_file_requests_and_connection_recovery(self):
        names = [".", "..", "../file", "a/b", "a\\b", "%2F", "%5C", "%00", "%0A", "%FF",
                 "%41", "%2E", "%2e", "%2b", "%", "%2", "%GG", "a+b", "x" * 256, "%20" * 256]
        cases = [f"SHARE {name} 1" for name in names]
        cases += [f"SHARE notes.txt {size}" for size in ("-1", "+1", "1x", "18446744073709551616")]
        cases += ["SHARE", "SHARE notes.txt", "SHARE notes.txt 1 extra", "SEARCH", "SEARCH a b",
                  "UNSHARE", "UNSHARE a b", "REPORT", "REPORT 1 a", "REPORT 1 a TIMEOUT extra",
                  "REPORT -1 a TIMEOUT", "REPORT 4294967296 a TIMEOUT", "REPORT 1 a UNKNOWN",
                  "NOPE a b c", "SHARE  notes.txt 1", "SHARE notes.txt 1 "]
        for request in cases:
            with self.subTest(request=request[:80]):
                self.assertEqual(self.owner.command(request), b"ERR INVALID_REQUEST")
        self.assertEqual(self.owner.command("NOPE a b"), b"ERR UNKNOWN_COMMAND")
        self.assertEqual(self.owner.command("SHARE valid.txt 1"), b"OK SHARED")

    def test_share_limit_allows_updates_and_recovers_after_removal(self):
        for i in range(128):
            self.assertEqual(self.owner.command(f"SHARE file{i} 1"), b"OK SHARED")
        self.assertEqual(self.owner.command("SHARE extra 1"), b"ERR SHARE_LIMIT")
        self.assertEqual(self.owner.command("SHARE file0 2"), b"OK SHARED")
        self.assertEqual(self.search(self.viewer, "file0")[0][2], b"2")
        self.assertEqual(self.owner.command("UNSHARE file0"), b"OK UNSHARED")
        self.assertEqual(self.owner.command("SHARE extra 1"), b"OK SHARED")

    def test_hello_updates_and_rejected_identity_preserve_shares(self):
        self.assertEqual(self.owner.command("SHARE notes.txt 1"), b"OK SHARED")
        self.assertEqual(self.owner.command("HELLO 20 9999"), b"OK HELLO")
        self.assertEqual(self.search(self.viewer)[0][5], b"9999")
        self.assertEqual(self.owner.command("HELLO 10 8000"), b"ERR CLIENT_ID_IN_USE")
        self.assertEqual(self.search(self.viewer)[0][5], b"9999")
        self.assertEqual(self.owner.command("HELLO 30 8000"), b"OK HELLO")
        self.assertEqual(self.search(self.viewer), [])
        self.assertEqual(self.owner.command("SHARE notes.txt 2"), b"OK SHARED")
        self.assertEqual(self.search(self.viewer)[0][3], b"30")

    def test_logout_clears_shares_and_requires_registration_after_login(self):
        self.assertEqual(self.owner.command("SHARE notes.txt 1"), b"OK SHARED")
        self.assertEqual(self.owner.command("LOGOUT"), b"OK LOGGED_OUT")
        self.assertEqual(self.search(self.viewer), [])
        self.assertEqual(self.owner.command(f"LOGIN alice {PASSWORD}"), b"OK LOGGED_IN")
        self.assertEqual(self.owner.command("SHARE notes.txt 1"), b"ERR HELLO_REQUIRED")
        self.assertEqual(self.owner.command("HELLO 20 9020"), b"OK HELLO")
        self.assertEqual(self.search(self.viewer), [])

    def test_abrupt_disconnect_cleans_index_before_id_reuse(self):
        self.assertEqual(self.owner.command("SHARE notes.txt 1"), b"OK SHARED")
        self.owner.socket.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        self.owner.close()
        eventually(lambda: self.search(self.viewer) == [])
        replacement = self.peer(20)
        self.assertEqual(self.search(replacement), [])

    def test_restart_keeps_accounts_and_discards_shares(self):
        self.assertEqual(self.owner.command("SHARE notes.txt 1"), b"OK SHARED")
        self.server.stop()
        self.server = Server(self.directory)
        self.addCleanup(self.server.stop)
        replacement = self.peer(20)
        self.assertEqual(self.search(replacement), [])

    def test_reports_are_validated_logged_and_advisory(self):
        self.assertEqual(self.owner.command("SHARE notes.txt 1"), b"OK SHARED")
        for reason in ("CONNECT_FAILED", "NOT_FOUND", "TIMEOUT", "TRANSFER_FAILED", "SIZE_MISMATCH"):
            self.assertEqual(self.viewer.command(f"REPORT 20 notes.txt {reason}"), b"OK REPORTED")
        self.assertEqual(len(self.search(self.viewer)), 1)
        self.assertEqual(self.owner.command("REPORT 20 notes.txt TIMEOUT"), b"ERR INVALID_REQUEST")
        self.assertEqual(self.viewer.command("REPORT 99 notes.txt TIMEOUT"), b"ERR FILE_NOT_FOUND")
        self.assertEqual(self.viewer.command("REPORT 20 missing TIMEOUT"), b"ERR FILE_NOT_FOUND")
        log = self.directory.joinpath("accounts.db.log").read_text()
        self.assertIn("target=20 filename=notes.txt reason=SIZE_MISMATCH", log)
        self.assertEqual(self.owner.command("UNSHARE notes.txt"), b"OK UNSHARED")
        self.assertEqual(self.viewer.command("REPORT 20 notes.txt TIMEOUT"), b"ERR FILE_NOT_FOUND")

    def test_counted_results_with_fragmented_and_pipelined_requests(self):
        self.assertEqual(self.owner.command("SHARE notes.txt 42"), b"OK SHARED")
        self.viewer.socket.sendall(b"SEAR")
        self.viewer.socket.sendall(b"CH notes.txt\nREPORT 20 notes.txt TIMEOUT\nSEARCH absent\nLOGOUT\n")
        self.assertEqual([self.viewer.line() for _ in range(5)],
                         [b"OK RESULTS 1", b"FILE notes.txt 42 20 127.0.0.1 9020",
                          b"OK REPORTED", b"OK RESULTS 0", b"OK LOGGED_OUT"])

    def test_search_results_larger_than_a_network_read_buffer(self):
        filename = "%20" * 255
        peers = [self.owner, self.viewer] + [self.peer(i + 100) for i in range(4)]
        for client in peers:
            self.assertEqual(client.command(f"SHARE {filename} 18446744073709551615"), b"OK SHARED")
        self.viewer.socket.sendall(f"SEARCH {filename}\nSEARCH missing\n".encode())
        self.assertEqual(self.viewer.line(), b"OK RESULTS 6")
        rows = [self.viewer.line() for _ in peers]
        self.assertGreater(sum(len(row) + 1 for row in rows), 4096)
        self.assertEqual([int(row.split()[3]) for row in rows], [10, 20, 100, 101, 102, 103])
        self.assertTrue(all(row.startswith(f"FILE {filename} ".encode()) for row in rows))
        self.assertEqual(self.viewer.line(), b"OK RESULTS 0")

    def test_search_snapshot_survives_concurrent_registration_and_cleanup(self):
        peers = [self.peer(i + 100, i + 9100) for i in range(6)]
        barrier = threading.Barrier(len(peers))

        def worker(pair):
            index, client = pair
            barrier.wait(timeout=10)
            for size in range(30):
                self.assertEqual(client.command(f"SHARE notes.txt {size}"), b"OK SHARED")
                rows = self.search(client)
                self.assertTrue(any(int(row[3]) == index + 100 for row in rows))
                self.assertEqual(client.command("UNSHARE notes.txt"), b"OK UNSHARED")
            self.assertEqual(client.command("SHARE notes.txt 99"), b"OK SHARED")
            if index % 2:
                self.assertEqual(client.command("LOGOUT"), b"OK LOGGED_OUT")
            else:
                self.assertEqual(client.command(f"HELLO {index + 200} 9999"), b"OK HELLO")

        with concurrent.futures.ThreadPoolExecutor(max_workers=len(peers)) as pool:
            list(pool.map(worker, enumerate(peers)))
        self.assertEqual(self.search(self.viewer), [])

    def test_log_is_private_append_only_and_excludes_credentials_and_raw_input(self):
        marker = "NeverLogThis!"
        self.assertEqual(self.owner.command("LOGOUT"), b"OK LOGGED_OUT")
        self.assertEqual(self.owner.command(f"LOGIN alice {marker}"), b"ERR INVALID_CREDENTIALS")
        self.assertEqual(self.owner.command(f"LOGIN alice {marker} extra"), b"ERR INVALID_REQUEST")
        self.assertEqual(self.owner.command(marker), b"ERR UNKNOWN_COMMAND")
        self.server.stop()
        path = self.directory / "accounts.db.log"
        original = path.read_bytes()
        self.assertNotIn(PASSWORD.encode(), original)
        self.assertNotIn(marker.encode(), original)
        self.assertIn(b"command=LOGIN result=ERR INVALID_CREDENTIALS", original)
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
            self.assertEqual(client.command("LOGOUT"), b"ERR AUTH_REQUIRED")
        custom_server.stop()
        self.assertIn("command=LOGOUT result=ERR AUTH_REQUIRED", custom.read_text())
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
