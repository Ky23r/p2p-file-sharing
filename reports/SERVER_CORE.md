# Centralized server core

- `src/server/server.c` now implements configurable IPv4 TCP binding, strict
  newline command dispatch, 128 concurrent client workers, 30-second socket
  inactivity timeouts, and SIGINT/SIGTERM shutdown through a self-pipe.
- `auth.c`/`auth.h` implement concurrent registration/login with OpenSSL
  PBKDF2-HMAC-SHA256 (600,000 iterations, random 16-byte salts, 32-byte hashes),
  constant-time comparisons, and persistent mode-0600 account storage. An
  exclusive lifetime file lock prevents competing server writers. Corrupt,
  inaccessible, linked, or non-regular storage fails startup cleanly.
- `session.c`/`session.h` maintain connection-local authentication, usernames,
  socket descriptors, observed IPs, client IDs, ports, and peer registration.
  HELLO requires login, covers all uint32 IDs, rejects duplicate active IDs,
  and releases IDs on logout/disconnect. Every worker is joined before shared
  state is destroyed. Mutexes protect accounts and sessions independently.
- Updated `Makefile` and `README.md`; added `tests/server/test_server.py`.
  File search, indexing, sharing, and transfer functionality remain out of scope.
- In a temporary `ubuntu:20.04` container, GCC 9.4 compiled `make clean && make
  && make test` without warnings: all five C socket-test groups and all 26 Python
  TCP integration tests passed (normal run: 6.452 seconds for Python tests).
- The same complete suite passed with AddressSanitizer, UndefinedBehaviorSanitizer,
  leak detection, and `-Werror` (Python tests: 9.433 seconds); no sanitizer errors.
  Tests exercised 128-client capacity/recovery, 160 connection churn cycles,
  concurrent registration/ID claims/session isolation, restart persistence,
  independently verified PBKDF2 hashes/salts, malformed input, storage corruption,
  permission denial, partial-write failure, and shutdown during account operations.
- Review fixes replaced detached-worker completion with explicit joining before
  library/shared-state cleanup and blocked shutdown signals in workers. The
  forced partial-append test confirmed rollback leaves no phantom account.
- Optional ThreadSanitizer could not execute: GCC 9 lacked `libtsan_preinit.o`;
  GCC 10 linked but aborted with `unexpected memory mapping` on this Docker/WSL2
  host. The container also denied the `setarch -R` workaround. No TSan pass claimed.
- Limitations: plaintext TCP requires encrypted transport for external deployment;
  IPv4 only; no rate limiting/account recovery; trusted storage directory required.
  Crash-truncated account records require offline repair. Socket timeouts limit
  inactivity, not total request duration. No Docker project files were added.
