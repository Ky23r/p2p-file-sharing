"""Test-only implementation of the pending discovery contract in docs/PROTOCOL.md."""

import socketserver
import threading


class ControlFixture:
    def __init__(self):
        self.lock = threading.Lock()
        self.peers = {}
        self.reports = []
        self.requests = []
        self.search_response = None
        self.share_response = b"OK SHARED\n"
        self.logout_response = b"OK LOGGED_OUT\n"
        self.report_response = b"OK REPORTED\n"
        self.close_after_search = False
        fixture = self

        class Handler(socketserver.StreamRequestHandler):
            def handle(self):
                authenticated = False
                identity = None
                shares = {}
                try:
                    while True:
                        line = self.rfile.readline(4098)
                        if not line:
                            break
                        fields = line.rstrip(b"\n").split(b" ")
                        command, args = fields[0], fields[1:]
                        response = b"ERR INVALID_REQUEST\n"
                        with fixture.lock:
                            fixture.requests.append(line)
                            if command == b"REGISTER" and len(args) == 2:
                                response = b"OK REGISTERED\n"
                            elif command == b"LOGIN" and len(args) == 2:
                                authenticated = True
                                response = b"OK LOGGED_IN\n"
                            elif not authenticated:
                                response = b"ERR AUTH_REQUIRED\n"
                            elif command == b"HELLO" and len(args) == 2:
                                candidate = (int(args[0]), self.client_address[0], int(args[1]))
                                holder = fixture.peers.get(candidate[0])
                                if holder is not None and holder[0] is not self:
                                    response = b"ERR CLIENT_ID_IN_USE\n"
                                else:
                                    if candidate != identity:
                                        if identity is not None:
                                            fixture.peers.pop(identity[0], None)
                                        shares.clear()
                                    identity = candidate
                                    fixture.peers[identity[0]] = (self, identity, shares)
                                    response = b"OK HELLO\n"
                            elif command == b"LOGOUT" and not args:
                                response = fixture.logout_response
                                if response == b"OK LOGGED_OUT\n":
                                    if identity is not None:
                                        fixture.peers.pop(identity[0], None)
                                    identity, authenticated = None, False
                                    shares.clear()
                            elif identity is None:
                                response = b"ERR PEER_REQUIRED\n"
                            elif command == b"SHARE" and len(args) == 2:
                                response = fixture.share_response
                                if response == b"OK SHARED\n":
                                    shares[args[0]] = int(args[1])
                            elif command == b"UNSHARE" and len(args) == 1:
                                shares.pop(args[0], None)
                                response = b"OK UNSHARED\n"
                            elif command == b"SEARCH" and len(args) == 1:
                                if fixture.search_response is not None:
                                    response = fixture.search_response
                                else:
                                    query = bytes.fromhex(args[0].decode())
                                    rows = []
                                    for _, peer, files in fixture.peers.values():
                                        for name, size in files.items():
                                            if query in bytes.fromhex(name.decode()):
                                                row = (f"FILE {peer[0]} {peer[1]} {peer[2]} {size} ".encode()
                                                       + name + b"\n")
                                                rows.append((bytes.fromhex(name.decode()), peer[0], row))
                                    rows.sort()
                                    response = f"OK RESULTS {len(rows)}\n".encode() + b"".join(r[2] for r in rows)
                            elif command == b"REPORT_FAIL" and len(args) == 3:
                                fixture.reports.append(tuple(args))
                                response = fixture.report_response
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
