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
        self.owner.expect("OK REGISTERED")
        self.owner.login()
        self.viewer = self.client("viewer", 10)
        self.viewer.login()

    def client(self, name, identity):
        client = Client(self.directory / name, self.server.port, identity)
        self.addCleanup(client.close)
        return client

    def search(self, client, name):
        client.send("search " + name)
        header = client.lines.get(timeout=8)
        self.assertIsInstance(header, str, client.errors())
        self.assertRegex(header, r"^OK RESULTS [0-9]+$")
        count = int(header.split()[2])
        self.assertLessEqual(count, 128)
        return [client.lines.get(timeout=8) for _ in range(count)]

    def expected_row(self, name, size, identity=20, port=None):
        port = self.owner.peer_port if port is None else port
        return f"1: {name} (client {identity}, 127.0.0.1:{port}, {size} bytes)"

    def share(self, name, contents):
        (self.owner.directory / name).write_bytes(contents)
        self.owner.send("share " + name)
        self.owner.expect("OK SHARED")

    def test_two_clients_transfer_encoded_binary_and_empty_files(self):
        binary = bytes(range(256)) * 4097
        for name, contents in (("report % +.bin", binary), ("empty +%.bin", b"")):
            with self.subTest(name=name):
                self.share(name, contents)
                self.assertEqual(self.search(self.viewer, name.split()[0]), [])
                self.assertEqual(self.search(self.viewer, name),
                                 [self.expected_row(name, len(contents))])
                self.viewer.send("download 1")
                self.viewer.expect("OK DOWNLOADED " + name)
                self.assertEqual((self.viewer.directory / name).read_bytes(), contents)
                self.assertEqual(list(self.viewer.directory.glob(".p2p-download-*")), [])

                self.owner.send("unshare " + name)
                self.owner.expect("OK UNSHARED")
                self.assertEqual(self.search(self.viewer, name), [])

    def test_logout_disconnect_and_restart_republish_catalog(self):
        name, contents = "lifecycle.bin", b"persistent local share\x00\n"
        self.share(name, contents)
        self.assertEqual(self.search(self.viewer, name),
                         [self.expected_row(name, len(contents))])

        self.owner.send("logout")
        self.owner.expect("OK LOGGED_OUT")
        self.assertEqual(self.search(self.viewer, name), [])
        self.owner.login()
        self.owner.expect("OK SHARED")
        self.assertEqual(self.search(self.viewer, name),
                         [self.expected_row(name, len(contents))])

        self.owner.close()
        self.assertEqual(self.owner.process.returncode, 0)
        eventually(lambda: self.search(self.viewer, name) == [])
        self.owner = self.client("owner", None)
        self.owner.login()
        self.owner.expect("OK SHARED")
        self.assertEqual(self.search(self.viewer, name),
                         [self.expected_row(name, len(contents))])
        self.viewer.send("download 1")
        self.viewer.expect("OK DOWNLOADED " + name)
        self.assertEqual((self.viewer.directory / name).read_bytes(), contents)

    def advertised_peer(self, filename, size):
        peer = self.server.connect()
        self.addCleanup(peer.close)
        self.assertEqual(peer.command(f"LOGIN alice {PASSWORD}"), b"OK LOGGED_IN")
        unavailable = socket.socket()
        self.addCleanup(unavailable.close)
        unavailable.bind(("127.0.0.1", 0))
        port = unavailable.getsockname()[1]  # Reserved without listening: connections are refused.
        self.assertEqual(peer.command(f"HELLO 90 {port}"), b"OK HELLO")
        self.assertEqual(peer.command(f"SHARE {filename} {size}"), b"OK SHARED")
        return peer, port

    def test_refused_download_is_reported_to_real_server_and_keeps_advertisement(self):
        name, token = "missing peer %.bin", "missing%20peer%20%25.bin"
        _, port = self.advertised_peer(token, 1)
        self.assertEqual(self.search(self.viewer, name),
                         [self.expected_row(name, 1, identity=90, port=port)])
        self.viewer.send("download 1")
        self.viewer.expect("OK REPORTED")
        self.assertIn("failed: CONNECT_FAILED", self.viewer.errors())
        self.assertFalse((self.viewer.directory / name).exists())
        self.assertEqual(list(self.viewer.directory.glob(".p2p-download-*")), [])
        self.assertEqual(self.search(self.viewer, name),
                         [self.expected_row(name, 1, identity=90, port=port)])
        log = self.directory.joinpath("accounts.db.log").read_text()
        self.assertRegex(log, r"command=REPORT result=OK REPORTED target=90 "
                              r"filename=missing%20peer%20%25\.bin reason=CONNECT_FAILED")
        self.assertNotIn(PASSWORD, log)

    def test_search_preserves_full_uint64_metadata(self):
        maximum = 18446744073709551615
        _, port = self.advertised_peer("huge.bin", maximum)
        self.assertEqual(self.search(self.viewer, "huge.bin"),
                         [self.expected_row("huge.bin", maximum, identity=90, port=port)])
        self.assertIsNone(self.viewer.process.poll())
        self.assertEqual(self.search(self.viewer, "absent.bin"), [])

    def test_legacy_non_ascii_catalog_entry_stays_pending_and_can_be_removed(self):
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
        client.send("shares")
        client.expect(f"SHARE {name} {len(contents)} PENDING")
        self.assertEqual(catalog.read_bytes(), original)
        self.assertIn("filenames must be printable ASCII", client.errors())
        client.send("unshare " + name)
        client.expect("OK UNSHARED")
        self.assertEqual(catalog.read_bytes(), b"")
        self.assertTrue(source.is_file())


if __name__ == "__main__":
    unittest.main(verbosity=2)
