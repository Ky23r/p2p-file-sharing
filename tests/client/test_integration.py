"""Exercise the complete CLI/discovery/P2P flow against the production server."""

from pathlib import Path
import socket
import sys
import tempfile
import unittest

from test_client import Client

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "server"))
from test_server import PASSWORD, Server, eventually


class ClientServerIntegrationTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="p2p-integration-")
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.server = Server(self.directory)
        self.addCleanup(self.server.stop)
        self.owner = self.client("owner", 20)
        self.owner.send(f"register alice\n{PASSWORD}")
        self.owner.expect("Registered")
        self.owner.login()
        self.viewer = self.client("viewer", 10)
        self.viewer.login()

    def client(self, name, identity):
        client = Client(self.directory / name, self.server.port, identity)
        self.addCleanup(client.close)
        return client

    def search(self, client, name):
        client.send("search " + name)
        return client.search_results()

    def expected_row(self, name, identity=20, port=None, number=1):
        port = self.owner.peer_port if port is None else port
        return f"{number}: {name} (client {identity}, 127.0.0.1:{port})"

    def share(self, name, contents):
        (self.owner.directory / name).write_bytes(contents)
        self.owner.send("share " + name)
        self.owner.expect("Shared")

    def test_two_clients_transfer_encoded_binary_and_empty_files(self):
        binary = bytes(range(256)) * 4097
        for name, contents in (("sample % +.bin", binary), ("empty +%.bin", b""),
                               ("two chunks.bin", binary[:2 * 65536])):
            with self.subTest(name=name):
                self.share(name, contents)
                self.assertEqual(self.search(self.owner, name), [])
                self.assertEqual(self.search(self.viewer, name.split()[0]), [])
                self.assertEqual(self.search(self.viewer, name),
                                 [self.expected_row(name)])
                self.viewer.send("download 1")
                self.viewer.expect("Downloaded " + name)
                self.assertEqual((self.viewer.directory / name).read_bytes(), contents)
                self.assertEqual(list(self.viewer.directory.glob(".p2p-download-*")), [])

                self.owner.send("unshare " + name)
                self.owner.expect("Unshared")
                self.assertEqual(self.search(self.viewer, name), [])

    def test_logout_disconnect_and_restart_republish_catalog(self):
        name, contents = "lifecycle.bin", b"persistent local share\x00\n"
        self.share(name, contents)
        self.assertEqual(self.search(self.viewer, name),
                         [self.expected_row(name)])

        self.owner.send("logout")
        self.owner.expect("Logged out")
        self.assertEqual(self.search(self.viewer, name), [])
        self.owner.login()
        self.owner.expect("Shared")
        self.assertEqual(self.search(self.viewer, name),
                         [self.expected_row(name)])
        log = self.server.accounts.with_suffix(".db.log").read_text()
        self.assertEqual(log.count("command=HELLO result=200 client_id=20 "), 1)

        self.owner.close()
        self.assertEqual(self.owner.process.returncode, 0)
        eventually(lambda: self.search(self.viewer, name) == [])
        self.owner = self.client("owner", None)
        self.owner.login()
        self.owner.expect("Shared")
        self.assertEqual(self.search(self.viewer, name),
                         [self.expected_row(name)])
        self.viewer.send("download 1")
        self.viewer.expect("Downloaded " + name)
        self.assertEqual((self.viewer.directory / name).read_bytes(), contents)

    def test_idle_connections_survive_while_partial_request_times_out(self):
        self.share("idle.bin", b"data")
        with self.server.connect() as stalled:
            self.assertEqual(stalled.command(f"LOGIN alice {PASSWORD}"), b"200")
            self.assertEqual(stalled.command("HELLO 90 9090"), b"200")
            self.assertEqual(stalled.command("SHARE 90 stalled.bin"), b"200")
            stalled.socket.settimeout(40)
            stalled.socket.sendall(b"SEARCH unfinished")
            self.assertEqual(stalled.line(), b"")

        self.assertEqual(self.search(self.viewer, "stalled.bin"), [])
        self.assertEqual(self.search(self.viewer, "idle.bin"), [self.expected_row("idle.bin")])
        self.viewer.send("download 1")
        self.viewer.expect("Downloaded idle.bin")
        self.assertEqual((self.viewer.directory / "idle.bin").read_bytes(), b"data")
        log = self.server.accounts.with_suffix(".db.log").read_text()
        self.assertEqual(log.count("command=HELLO result=200 "), 3)

    def advertised_peer(self, filename, identity=90):
        peer = self.server.connect()
        self.addCleanup(peer.close)
        self.assertEqual(peer.command(f"LOGIN alice {PASSWORD}"), b"200")
        unavailable = socket.socket()
        self.addCleanup(unavailable.close)
        unavailable.bind(("127.0.0.1", 0))
        port = unavailable.getsockname()[1]  # Reserved without listening: connections are refused.
        self.assertEqual(peer.command(f"HELLO {identity} {port}"), b"200")
        self.assertEqual(peer.command(f"SHARE {identity} {filename}"), b"200")
        return peer, port

    def test_refused_download_keeps_advertisement(self):
        name, token = "missing peer %.bin", "missing%20peer%20%25.bin"
        _, port = self.advertised_peer(token)
        self.assertEqual(self.search(self.viewer, name),
                         [self.expected_row(name, identity=90, port=port)])
        self.viewer.send("download 1")
        self.assertEqual(self.search(self.viewer, name),
                         [self.expected_row(name, identity=90, port=port)])
        self.assertIn("could not connect", self.viewer.errors())
        log = self.server.accounts.with_suffix(".db.log").read_text()
        self.assertIn(f"command=FAIL result=200 client_id=90 filename={token} code=503", log)
        self.assertFalse((self.viewer.directory / name).exists())
        self.assertEqual(list(self.viewer.directory.glob(".p2p-download-*")), [])

    def test_owner_withdraws_only_its_stale_share(self):
        backup = self.client("backup", 30)
        backup.login()
        for name in ("missing.bin", "symlink.bin"):
            with self.subTest(name=name):
                self.share(name, b"data")
                (backup.directory / name).write_bytes(b"backup")
                backup.send("share " + name)
                backup.expect("Shared")
                backup_row = self.expected_row(name, identity=30, port=backup.peer_port)
                initial_rows = [self.expected_row(name),
                                self.expected_row(name, identity=30, port=backup.peer_port, number=2)]
                self.assertEqual(self.search(self.viewer, name), initial_rows)

                source = self.owner.directory / name
                source.unlink()
                if name == "symlink.bin":
                    source.symlink_to(backup.directory / name)
                with socket.create_connection(("127.0.0.1", self.owner.peer_port), timeout=4) as peer:
                    peer.sendall(f"GET {name}\r\n".encode())
                    with peer.makefile("rb") as reply:
                        self.assertEqual(reply.read(), b"404\r\n")

                self.assertEqual(self.search(self.viewer, name), [backup_row])
                self.owner.send("shares")
                self.owner.expect(f"{name} (pending)")
                log = self.server.accounts.with_suffix(".db.log").read_text()
                self.assertIn(f"command=FAIL result=200 client_id=20 filename={name} code=404", log)
                self.viewer.send("download 1")
                self.viewer.expect("Downloaded " + name)
                self.assertEqual((self.viewer.directory / name).read_bytes(), b"backup")
                if source.is_symlink():
                    source.unlink()
                self.share(name, b"restored")
                self.assertEqual(self.search(self.viewer, name), initial_rows)
                self.owner.send("unshare " + name)
                self.owner.expect("Unshared")

    def test_source_read_failure_is_reported_without_withdrawing(self):
        # Linux exposes this as a regular file, but offset zero is unmapped.
        self.owner.send("share /proc/self/mem")
        self.owner.expect("Shared")
        with socket.create_connection(("127.0.0.1", self.owner.peer_port), timeout=4) as peer:
            peer.sendall(b"GET mem\r\n")
            with peer.makefile("rb") as reply:
                self.assertEqual(reply.read(), b"200\r\n")
        log = self.server.accounts.with_suffix(".db.log").read_text()
        self.assertIn("command=FAIL result=200 client_id=20 filename=mem code=503", log)
        self.assertEqual(self.search(self.viewer, "mem"), [self.expected_row("mem")])

    def test_search_accepts_maximum_client_id(self):
        maximum = 4294967295
        _, port = self.advertised_peer("huge.bin", maximum)
        self.assertEqual(self.search(self.viewer, "huge.bin"),
                         [self.expected_row("huge.bin", identity=maximum, port=port)])
        self.assertIsNone(self.viewer.process.poll())
        self.assertEqual(self.search(self.viewer, "absent.bin"), [])

    def test_existing_non_ascii_catalog_entry_can_be_downloaded_and_removed(self):
        directory = self.directory / "legacy"
        directory.mkdir()
        name = "báo cáo.bin"
        source = directory / name
        contents = b"file saved before the protocol was integrated"
        source.write_bytes(contents)
        original = f"{name.encode().hex()}\t{str(source).encode().hex()}\n".encode()
        catalog = directory / "index.txt"
        catalog.write_bytes(original)

        client = self.client("legacy", 30)
        client.login()
        client.expect("Shared")
        client.send("shares")
        client.expect(f"{name} (shared)")
        self.assertEqual(catalog.read_bytes(), original)
        self.assertEqual(self.search(self.viewer, name),
                         [self.expected_row(name, identity=30, port=client.peer_port)])
        self.viewer.send("download 1")
        self.viewer.expect("Downloaded " + name)
        self.assertEqual((self.viewer.directory / name).read_bytes(), contents)
        client.send("unshare " + name)
        client.expect("Unshared")
        self.assertEqual(catalog.read_bytes(), b"")
        self.assertTrue(source.is_file())


if __name__ == "__main__":
    unittest.main(verbosity=2)
