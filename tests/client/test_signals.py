"""Exercise cancellation through real terminals, transfers, and peer workers."""

import os
from pathlib import Path
import pty
import select
import signal
import socket
import subprocess
import tempfile
import termios
import threading
import time
import unittest

from control_fixture import ControlFixture
from test_client import BINARY, free_port


class InteractiveClient:
    def __init__(self, directory, server_port, identity):
        self.directory = Path(directory)
        self.directory.mkdir()
        (self.directory / "config.txt").write_text(f"{identity}\n")
        self.peer_port = free_port()
        self.terminal, self.child_tty = pty.openpty()
        self.original_echo = termios.tcgetattr(self.child_tty)[3] & termios.ECHO
        self.pending = bytearray()
        self.output = bytearray()
        try:
            self.process = subprocess.Popen(
                [str(BINARY), "--port", str(server_port), "--peer-port", str(self.peer_port)],
                cwd=self.directory, stdin=self.child_tty,
                stdout=self.child_tty, stderr=self.child_tty)
        except BaseException:
            os.close(self.terminal)
            os.close(self.child_tty)
            raise

    def send(self, text):
        os.write(self.terminal, text.encode() + b"\n")

    def expect(self, marker, timeout=5):
        deadline = time.monotonic() + timeout
        while marker not in self.pending:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise AssertionError(f"Missing terminal output {marker!r}: {self.output!r}")
            readable, _, _ = select.select([self.terminal], [], [], remaining)
            if not readable:
                continue
            try:
                block = os.read(self.terminal, 4096)
            except OSError as error:
                raise AssertionError(f"Terminal closed before {marker!r}: {self.output!r}") from error
            if not block:
                raise AssertionError(f"Terminal closed before {marker!r}: {self.output!r}")
            self.pending.extend(block)
            self.output.extend(block)
        end = self.pending.index(marker) + len(marker)
        consumed = bytes(self.pending[:end])
        del self.pending[:end]
        return consumed

    def login(self):
        self.expect(b"p2p> ")
        self.send("login alice")
        self.expect(b"Password: ")
        self.send("LocalTest!42")
        self.expect(b"OK LOGGED_IN\r\n")
        self.expect(b"OK HELLO\r\n")
        self.expect(b"p2p> ")

    def close(self):
        try:
            if self.process.poll() is None:
                self.process.kill()
            self.process.wait(timeout=3)
        finally:
            os.close(self.terminal)
            os.close(self.child_tty)


class StalledPeer:
    """Send a valid header and partial body, keeping the transfer socket open."""

    def __init__(self):
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)
        self.listener.settimeout(0.1)
        self.port = self.listener.getsockname()[1]
        self.stop = threading.Event()
        self.sent = threading.Event()
        self.request = None
        self.error = None
        self.thread = threading.Thread(target=self._serve, daemon=True)
        self.thread.start()

    def _serve(self):
        try:
            while not self.stop.is_set():
                try:
                    connection, _ = self.listener.accept()
                    break
                except socket.timeout:
                    continue
            else:
                return
            with connection:
                connection.settimeout(3)
                request = bytearray()
                while not request.endswith(b"\n") and len(request) <= 4096:
                    block = connection.recv(4096)
                    if not block:
                        raise AssertionError("Client closed without a complete GET request")
                    request.extend(block)
                self.request = bytes(request)
                connection.sendall(b"OK FILE 10\nabc")
                self.sent.set()
                self.stop.wait()
        except Exception as error:
            if not self.stop.is_set():
                self.error = error

    def close(self):
        self.stop.set()
        self.listener.close()
        self.thread.join(timeout=4)
        if self.thread.is_alive():
            raise AssertionError("Stalled peer fixture did not stop")
        if self.error is not None:
            raise self.error


class ClientSignalTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="p2p-signal-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.server = ControlFixture()
        self.addCleanup(self.server.close)
        self.next_id = 0

    def client(self):
        self.next_id += 1
        client = InteractiveClient(self.directory / str(self.next_id), self.server.port, self.next_id)
        self.addCleanup(client.close)
        return client

    def assert_signal_exit(self, client, signum):
        client.process.send_signal(signum)
        try:
            status = client.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.fail(f"Client did not stop promptly after {signal.Signals(signum).name}")
        self.assertEqual(status, 128 + signum)
        self.assertEqual(termios.tcgetattr(client.child_tty)[3] & termios.ECHO,
                         client.original_echo, "Cancellation changed terminal echo")

    def cancel_stalled_download(self, signum):
        client = self.client()
        client.login()
        peer = StalledPeer()
        self.addCleanup(peer.close)
        filename = "cancelled.bin"
        encoded = filename.encode().hex()
        self.server.search_response = (
            f"OK RESULTS 1\nFILE 90 127.0.0.1 {peer.port} 10 {encoded}\n".encode())
        client.send("search cancelled")
        client.expect(b"OK RESULTS 1\r\n")
        client.expect(b"p2p> ")
        client.send("download 1")
        self.assertTrue(peer.sent.wait(timeout=3), f"Peer never sent a partial body: {peer.error!r}")
        self.assertEqual(peer.request, f"GET {encoded}\n".encode())

        deadline = time.monotonic() + 3
        while True:
            partials = list(client.directory.glob(".p2p-download-*"))
            if len(partials) == 1 and partials[0].read_bytes() == b"abc":
                break
            if time.monotonic() >= deadline:
                self.fail(f"Client never saved the partial download: {client.output!r}")
            time.sleep(0.01)
        self.assertIsNone(client.process.poll(), "Client exited before cancellation")
        self.assertFalse((client.directory / filename).exists())

        self.assert_signal_exit(client, signum)

        self.assertFalse((client.directory / filename).exists(), "Cancellation published an incomplete file")
        self.assertEqual(list(client.directory.glob(".p2p-download-*")), [],
                         "Cancellation left a partial download on disk")

    def test_sigint_removes_partial_stalled_download(self):
        self.cancel_stalled_download(signal.SIGINT)

    def test_sigterm_removes_partial_stalled_download(self):
        self.cancel_stalled_download(signal.SIGTERM)

    def test_signals_stop_idle_client_with_partial_peer_requests(self):
        for signum in (signal.SIGINT, signal.SIGTERM):
            with self.subTest(signal=signal.Signals(signum).name):
                client = self.client()
                client.login()
                peers = []
                try:
                    for _ in range(4):
                        peer = socket.create_connection(("127.0.0.1", client.peer_port), timeout=3)
                        peers.append(peer)
                        peer.sendall(b"GET partial")
                    self.assert_signal_exit(client, signum)
                finally:
                    for peer in peers:
                        peer.close()

    def test_sigterm_restores_echo_during_password_entry(self):
        client = self.client()
        client.expect(b"p2p> ")
        client.send("login alice")
        client.expect(b"Password: ")
        self.assertFalse(termios.tcgetattr(client.child_tty)[3] & termios.ECHO)

        self.assert_signal_exit(client, signal.SIGTERM)


if __name__ == "__main__":
    unittest.main(verbosity=2)
