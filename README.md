# P2P file sharing

The server manages accounts, sessions, and file discovery. Clients publish
local files and transfer them directly over IPv4 TCP. The project uses C11,
POSIX sockets, pthreads, and OpenSSL. Ubuntu 20.04 is the project target.

## Build and run

Install `build-essential`, `libssl-dev`, and `python3`, then run `make`.
Build the server and clients from the same version so they use the same
protocol.

Start the server in its own terminal and wait for `READY`.

```sh
./server
```

To run two clients on the same machine, give them separate ports, catalogs,
ID files, and download directories. Run each client in a separate terminal.

```sh
mkdir -p downloads-a downloads-b
./client --peer-port 9001 --config client-a.txt --index shares-a.txt --downloads downloads-a
./client --peer-port 9002 --config client-b.txt --index shares-b.txt --downloads downloads-b
```

On a LAN, start the server with `--bind <server-LAN-IPv4>` and start each
workstation's client with `--server <server-LAN-IPv4>`. Allow TCP access to
the server's control port and to each peer's listener port. Use a trusted
network. Control traffic, including login credentials, is sent in plaintext.

### Options and defaults

| Program | Option | Default |
| --- | --- | --- |
| Server | `--bind IPv4` | `127.0.0.1` |
| Server | `--port PORT` | `9000` |
| Server | `--accounts PATH` | `data/server/accounts.db` |
| Server | `--log PATH` | `<accounts-path>.log` |
| Client | `--server IPv4` | `127.0.0.1` |
| Client | `--port PORT` | `9000` |
| Client | `--peer-port PORT` | `9001` |
| Client | `--config PATH` | `config.txt` |
| Client | `--index PATH` | `index.txt` |
| Client | `--downloads DIRECTORY` | Current directory |

Both programs accept `--help`. Create download directories and the parent
directories for custom storage paths before starting the programs.

## Client commands

| Command | Action |
| --- | --- |
| `register USER` | Create an account. |
| `login USER` | Log in and publish readable saved shares. |
| `logout` | Release the active client ID and its advertisements. |
| `share PATH` | Save a local file in the catalog and advertise it when logged in. |
| `unshare NAME` | Remove a filename from the catalog and withdraw its advertisement. |
| `shares` | List catalog entries as `shared` or `pending`. |
| `search NAME` | Find other peers advertising the exact filename. |
| `download NUMBER` | Download a numbered result from the latest search. |
| `help` | Show the command list. |
| `quit` | Close the control connection and exit. |

Commands use lowercase. `register` and `login` prompt for a password with
terminal echo disabled. In a script, put the password on the next line.
File arguments can contain spaces and use the remainder of the command line.

Register and log in on each client. Share a readable file with `share PATH`
on one client. On the other, use `search NAME` with the file's basename,
then `download NUMBER` with the number shown for the desired peer. Search
displays `Found <count>` before the numbered results. A completed download
is saved under the requested filename in the download directory.

## Project structure

| Path | Contents |
| --- | --- |
| `src/client/` | Command-line client, local files, and peer transfers |
| `src/server/` | Control server, accounts, session index, and service log |
| `src/common/`, `include/` | Shared protocol parsing and buffered TCP I/O |
| `data/server/` | Default account and log location |
| `tests/client/`, `tests/server/` | C and Python tests |
| `docs/` | Protocol, error handling, and progress report |
| `Makefile` | Builds `server`, `client`, and the test binaries |

## Verification and documentation

Run `make test` to build the programs and run the C and Python suites.
The storage-permissions test runs when started as root and drops privileges
before checking that the server refuses an unreadable account store.

- [Protocol](docs/PROTOCOL.md) covers wire commands, reply codes, and state.
- [Error handling](docs/ERROR_HANDLING.md) covers failures and recovery.
- [Progress report](docs/PROGRESS_REPORT.md) lists team assignments, completed
  requirements, and verification results.
