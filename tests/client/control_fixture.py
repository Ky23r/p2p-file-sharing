"""Controllable replies for client discovery and error-handling tests."""

import socketserver
import threading
from urllib.parse import quote


def filename_token(name):
    return quote(name, safe="")


class ControlFixture:
    def __init__(self):
        self.lock = threading.Lock()
        self.peers = {}
        self.requests = []
        self.search_response = None
        self.share_response = b"200 OK\r\n"
        self.logout_response = b"200 OK\r\n"
        self.close_after_search = False
        self.hello_responses = []
        self.unshare_rejections = []
        self.close_after_unshare = False
        self.unshare_received = threading.Event()
        fixture = self

        class Handler(socketserver.StreamRequestHandler):
            def handle(self):
                authenticated = False
                staged = None
                identity = None
                shares = {}
                try:
                    while True:
                        line = self.rfile.readline()
                        if not line:
                            break
                        if not line.endswith(b"\r\n"):
                            self.wfile.write(b"500 Invalid or unknown command\r\n")
                            continue
                        fields = line[:-2].split(b" ")
                        command, args = fields[0].upper(), fields[1:]
                        response = b"501 Invalid arguments\r\n"
                        if command == b"UNSHARE":
                            fixture.unshare_received.set()
                        with fixture.lock:
                            fixture.requests.append(line)
                            if command == b"REGISTER" and len(args) == 2:
                                response = b"200 OK\r\n"
                            elif command == b"LOGIN" and len(args) == 2:
                                if staged is not None and staged[0] in fixture.peers:
                                    response = b"550 Client ID in use\r\n"
                                else:
                                    authenticated = True
                                    identity = staged
                                    if identity is not None:
                                        fixture.peers[identity[0]] = (self, identity, shares)
                                    response = b"200 OK\r\n"
                            elif command == b"HELLO" and len(args) == 2:
                                candidate = (int(args[0]), self.client_address[0], int(args[1]))
                                holder = fixture.peers.get(candidate[0])
                                override = fixture.hello_responses.pop(0) if fixture.hello_responses else b"200 OK\r\n"
                                if override != b"200 OK\r\n":
                                    response = override
                                elif not authenticated:
                                    staged = candidate
                                    response = b"200 OK\r\n"
                                elif holder is not None and holder[0] is not self:
                                    response = b"550 Client ID in use\r\n"
                                else:
                                    if identity is None or candidate[0] != identity[0]:
                                        if identity is not None:
                                            fixture.peers.pop(identity[0], None)
                                        shares.clear()
                                    identity = candidate
                                    fixture.peers[identity[0]] = (self, identity, shares)
                                    response = b"200 OK\r\n"
                            elif not authenticated:
                                response = b"530 Not logged in\r\n"
                            elif command == b"LOGOUT" and not args:
                                response = fixture.logout_response
                                if response == b"200 OK\r\n":
                                    if identity is not None:
                                        fixture.peers.pop(identity[0], None)
                                    identity, authenticated = None, False
                                    staged = None
                                    shares.clear()
                            elif identity is None:
                                response = b"503 Bad command sequence\r\n"
                            elif command == b"SHARE" and len(args) == 3:
                                response = fixture.share_response if int(args[0]) == identity[0] else b"550 Client ID does not match session\r\n"
                                if response == b"200 OK\r\n":
                                    shares[args[1]] = int(args[2])
                            elif command == b"UNSHARE" and len(args) == 1:
                                if fixture.unshare_rejections:
                                    response = fixture.unshare_rejections.pop(0)
                                elif args[0] in shares:
                                    del shares[args[0]]
                                    response = b"200 OK\r\n"
                                else:
                                    response = b"550 File not shared\r\n"
                            elif command == b"SEARCH" and len(args) == 1:
                                if fixture.search_response is not None:
                                    response = fixture.search_response
                                else:
                                    name = args[0]
                                    rows = []
                                    for _, peer, files in fixture.peers.values():
                                        if name in files:
                                            row = b"200-" + name + f" {files[name]} {peer[0]} {peer[1]} {peer[2]}\r\n".encode()
                                            rows.append((peer[0], row))
                                    rows.sort()
                                    response = b"".join(row for _, row in rows) + b"200 OK\r\n"
                        if command == b"UNSHARE" and fixture.close_after_unshare:
                            return
                        self.wfile.write(response)
                        self.wfile.flush()
                        if command == b"SEARCH" and fixture.close_after_search:
                            return
                finally:
                    with fixture.lock:
                        if identity is not None:
                            fixture.peers.pop(identity[0], None)

        class Server(socketserver.ThreadingTCPServer):
            allow_reuse_address = True
            daemon_threads = True

        self.server = Server(("127.0.0.1", 0), Handler)
        self.port = self.server.server_address[1]
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
