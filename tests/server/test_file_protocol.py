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
            self.assertEqual(client.command(f"REGISTER alice {PASSWORD}"), b"200")
        self.owner = self.peer(20, 9020)
        self.viewer = self.peer(10, 9010)

    def peer(self, client_id, port=9001):
        client = self.server.connect()
        self.addCleanup(client.close)
        self.assertEqual(client.command(f"LOGIN alice {PASSWORD}"), b"200")
        self.assertEqual(client.command(f"HELLO {client_id} {port}"), b"200")
        return client

    def search(self, client, filename="notes.txt"):
        header = client.command(f"SEARCH {filename}")
        self.assertRegex(header, rb"^200 [0-9]+$")
        results = [client.line().split() for _ in range(int(header.split()[1]))]
        for record in results:
            self.assertEqual(len(record), 3)
            self.assertEqual(record[1], b"127.0.0.1")
        ids = [int(record[0]) for record in results]
        self.assertEqual(ids, sorted(set(ids)))
        return results

    def test_authentication_hello_and_validation_precedence(self):
        with self.server.connect() as client:
            commands = ["SHARE 20 notes.txt", "UNSHARE notes.txt", "SEARCH notes.txt",
                        "FAIL 20 notes.txt 503", "FAIL 20 notes.txt 404"]
            for command in commands:
                self.assertEqual(client.command(command), b"401")
            self.assertEqual(client.command(f"LOGIN alice {PASSWORD}"), b"200")
            for command in commands:
                self.assertEqual(client.command(command), b"403")
            self.assertEqual(client.command("SHARE 20 ../bad"), b"400")

    def test_startup_hello_stages_identity_until_login(self):
        with self.server.connect() as first, self.server.connect() as second:
            self.assertEqual(first.command("HELLO 30 9030"), b"200")
            self.assertEqual(first.command("SHARE 30 notes.txt"), b"401")
            self.assertEqual(first.command("LOGIN alice WrongPass!"), b"401")
            self.assertEqual(second.command("HELLO 30 9031"), b"200")
            self.assertEqual(second.command(f"LOGIN alice {PASSWORD}"), b"200")
            self.assertEqual(first.command(f"LOGIN alice {PASSWORD}"), b"409")
            self.assertEqual(first.command("SEARCH notes.txt"), b"401")
            self.assertEqual(second.command("SHARE 31 notes.txt"), b"403")
            self.assertEqual(second.command("SHARE 30 notes.txt"), b"200")
            self.assertEqual(self.search(self.viewer)[0][2], b"9031")
            self.assertEqual(second.command("LOGOUT"), b"200")
            self.assertEqual(first.command(f"LOGIN alice {PASSWORD}"), b"200")
            self.assertEqual(first.command("SHARE 30 notes.txt"), b"200")
            self.assertEqual(self.search(self.viewer)[0][2], b"9030")

    def test_share_update_search_and_owner_only_removal(self):
        self.assertEqual(self.owner.command("SHARE 20 notes.txt"), b"200")
        self.assertEqual(self.search(self.owner), [])
        row, = self.search(self.viewer)
        self.assertEqual(row, [b"20", b"127.0.0.1", b"9020"])
        self.assertEqual(self.viewer.command("UNSHARE notes.txt"), b"200")
        self.assertEqual(self.search(self.viewer), [row])
        self.assertEqual(self.owner.command("SHARE 20 notes.txt"), b"200")
        self.assertEqual(self.search(self.viewer), [row])
        self.assertEqual(self.owner.command("UNSHARE notes.txt"), b"200")
        self.assertEqual(self.search(self.viewer), [])
        self.assertEqual(self.owner.command("UNSHARE notes.txt"), b"200")

    def test_multiple_peers_numeric_order_and_exact_matching(self):
        maximum = self.peer(4294967295)
        zero = self.peer(0)
        for client, identity in ((maximum, 4294967295), (self.owner, 20), (zero, 0), (self.viewer, 10)):
            self.assertEqual(client.command(f"SHARE {identity} notes.txt"), b"200")
        self.assertEqual([int(row[0]) for row in self.search(self.viewer)], [0, 20, 4294967295])
        self.assertEqual(self.search(self.viewer, "Notes.txt"), [])
        self.assertEqual(self.search(self.viewer, "notes"), [])
        self.assertEqual(self.search(self.viewer, "%2A"), [])

    def test_only_confirmed_owner_failure_withdraws_a_share(self):
        backup = self.peer(30)
        for client, identity in ((self.owner, 20), (backup, 30)):
            self.assertEqual(client.command(f"SHARE {identity} notes.txt"), b"200")

        self.assertEqual(self.viewer.command("FAIL 20 notes.txt 404"),
                         b"403")
        self.assertEqual(self.viewer.command("FAIL 20 notes.txt 503"), b"200")
        self.assertEqual(self.owner.command("FAIL 20 notes.txt 503"), b"200")
        self.assertEqual([int(row[0]) for row in self.search(self.viewer)], [20, 30])

        self.assertEqual(self.owner.command("fAiL 20 notes.txt 404"), b"200")
        self.assertEqual(self.owner.command("FAIL 20 notes.txt 404"), b"200")
        self.assertEqual([int(row[0]) for row in self.search(self.viewer)], [30])

    def test_encoded_filename_and_maximum_length(self):
        for name in ("my%20notes.txt", "100%25.txt", "a%2Bb.txt", "b%C3%A1o%20c%C3%A1o.txt", "%20" * 255):
            self.assertEqual(self.owner.command(f"SHARE 20 {name}"), b"200")
            self.assertEqual(self.search(self.viewer, name)[0][0], b"20")
            self.assertEqual(self.owner.command(f"UNSHARE {name}"), b"200")

    def test_malformed_file_requests_and_connection_recovery(self):
        names = [".", "..", "../file", "a/b", "a\\b", "%2F", "%5C", "%00", "%0A", "%7F",
                 "%41", "%2E", "%2e", "%2b", "%", "%2", "%GG", "a+b", "x" * 256, "%20" * 256]
        cases = [f"SHARE 20 {name}" for name in names]
        cases += [f"SHARE {identity} notes.txt" for identity in ("-1", "+20", "4294967296")]
        cases += ["SHARE", "SHARE notes.txt", "SHARE notes.txt 1", "SHARE 20 notes.txt 1 extra",
                  "SEARCH", "SEARCH a b",
                  "UNSHARE", "UNSHARE a b",
                  "SHARE  20 notes.txt 1", "SHARE 20 notes.txt 1 ",
                  "FAIL 20 notes.txt", "FAIL 20 notes.txt 500", "FAIL 20 notes.txt 0450",
                  "FAIL -1 notes.txt 503", "FAIL 20 ../notes.txt 404", "FAIL 20 notes.txt 404 extra"]
        for request in cases:
            with self.subTest(request=request[:80]):
                self.assertEqual(self.owner.command(request), b"400")
        for request in ("NOPE", "NOPE a b", "NOPE a b c d e"):
            self.assertEqual(self.owner.command(request), b"400")
        self.assertEqual(self.owner.command("SHARE 20 valid.txt"), b"200")

    def test_over_128_shares_support_updates_and_removal(self):
        for i in range(140):
            self.assertEqual(self.owner.command(f"SHARE 20 file{i}"), b"200")
        self.assertEqual(self.owner.command("SHARE 20 file0"), b"200")
        self.assertEqual(self.search(self.viewer, "file0"), [[b"20", b"127.0.0.1", b"9020"]])
        self.assertEqual(self.owner.command("UNSHARE file0"), b"200")
        self.assertEqual(self.owner.command("SHARE 20 extra"), b"200")

    def test_hello_updates_and_rejected_identity_preserve_shares(self):
        self.assertEqual(self.owner.command("SHARE 20 notes.txt"), b"200")
        self.assertEqual(self.owner.command("HELLO 20 9999"), b"200")
        self.assertEqual(self.search(self.viewer)[0][2], b"9999")
        self.assertEqual(self.owner.command("HELLO 10 8000"), b"409")
        self.assertEqual(self.search(self.viewer)[0][2], b"9999")
        self.assertEqual(self.owner.command("HELLO 30 8000"), b"200")
        self.assertEqual(self.search(self.viewer), [])
        self.assertEqual(self.owner.command("SHARE 30 notes.txt"), b"200")
        self.assertEqual(self.search(self.viewer)[0][0], b"30")

    def test_logout_clears_shares_and_retains_announcement_for_relogin(self):
        self.assertEqual(self.owner.command("SHARE 20 notes.txt"), b"200")
        self.assertEqual(self.owner.command("LOGOUT"), b"200")
        self.assertEqual(self.search(self.viewer), [])
        self.assertEqual(self.owner.command(f"LOGIN alice {PASSWORD}"), b"200")
        self.assertEqual(self.search(self.viewer), [])
        self.assertEqual(self.owner.command("SHARE 20 notes.txt"), b"200")
        row, = self.search(self.viewer)
        self.assertEqual(row, [b"20", b"127.0.0.1", b"9020"])

    def test_abrupt_disconnect_cleans_index_before_id_reuse(self):
        self.assertEqual(self.owner.command("SHARE 20 notes.txt"), b"200")
        self.owner.socket.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        self.owner.close()
        eventually(lambda: self.search(self.viewer) == [])
        self.peer(20)
        self.assertEqual(self.search(self.viewer), [])

    def test_restart_keeps_accounts_and_discards_shares(self):
        self.assertEqual(self.owner.command("SHARE 20 notes.txt"), b"200")
        self.server.stop()
        self.server = Server(self.directory)
        self.addCleanup(self.server.stop)
        self.peer(20)
        self.assertEqual(self.search(self.peer(10)), [])

    def test_counted_search_with_fragmented_and_pipelined_requests(self):
        self.assertEqual(self.owner.command("SHARE 20 notes.txt"), b"200")
        self.viewer.socket.sendall(b"SEARCH notes.txt\r")
        self.viewer.socket.sendall(b"\nSEARCH absent\r\nLOGOUT\r\n")
        self.assertEqual([self.viewer.line() for _ in range(4)],
                         [b"200 1", b"20 127.0.0.1 9020", b"200 0", b"200"])

    def test_pipelined_searches_span_multiple_network_reads(self):
        self.assertEqual(self.owner.command("SHARE 20 notes.txt"), b"200")
        self.viewer.socket.sendall(b"SEARCH notes.txt\r\nSEARCH missing\r\n" * 160)
        for _ in range(160):
            self.assertEqual(self.viewer.line(), b"200 1")
            self.assertEqual(self.viewer.line(), b"20 127.0.0.1 9020")
            self.assertEqual(self.viewer.line(), b"200 0")

    def test_search_snapshot_survives_concurrent_registration_and_cleanup(self):
        self.assertEqual(self.owner.command("SHARE 20 notes.txt"), b"200")
        peers = [self.peer(i + 100, i + 9100) for i in range(6)]
        barrier = threading.Barrier(len(peers))

        def worker(pair):
            index, client = pair
            barrier.wait(timeout=10)
            for _ in range(30):
                self.assertEqual(client.command(f"SHARE {index + 100} notes.txt"), b"200")
                ids = [int(row[0]) for row in self.search(client)]
                self.assertIn(20, ids)
                self.assertNotIn(index + 100, ids)
                self.assertEqual(client.command("UNSHARE notes.txt"), b"200")
            self.assertEqual(client.command(f"SHARE {index + 100} notes.txt"), b"200")
            if index % 2:
                self.assertEqual(client.command("LOGOUT"), b"200")
            else:
                self.assertEqual(client.command(f"HELLO {index + 200} 9999"), b"200")

        with concurrent.futures.ThreadPoolExecutor(max_workers=len(peers)) as pool:
            list(pool.map(worker, enumerate(peers)))
        self.assertEqual([int(row[0]) for row in self.search(self.viewer)], [20])

    def test_log_is_private_append_only_and_excludes_credentials_and_raw_input(self):
        marker = "NeverLogThis!"
        self.assertEqual(self.owner.command("LOGOUT"), b"200")
        self.assertEqual(self.owner.command(f"LOGIN alice {marker}"), b"401")
        self.assertEqual(self.owner.command(f"LOGIN alice {marker} extra"), b"400")
        self.assertEqual(self.owner.command(marker), b"400")
        self.server.stop()
        path = self.directory / "accounts.db.log"
        original = path.read_bytes()
        self.assertNotIn(PASSWORD.encode(), original)
        self.assertNotIn(marker.encode(), original)
        self.assertIn(b"command=LOGIN result=401", original)
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
            self.assertEqual(client.command("LOGOUT"), b"401")
        custom_server.stop()
        self.assertIn("command=LOGOUT result=401", custom.read_text())
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
