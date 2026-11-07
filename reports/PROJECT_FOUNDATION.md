# Project foundation

- Added a modular C11 Makefile, protocol contract, usage-only server entry point,
  persistent-data directory placeholder, and generated/runtime-file exclusions.
- `include/net_io.h` and `src/common/net_io.c` provide buffered newline reads and
  complete writes with partial-I/O handling, EINTR retries, and SIGPIPE suppression.
- `include/protocol.h` and `README.md` define the 4096-byte limit, validation,
  responses, authentication requirements, and deterministic connection behavior.
- Verified in a temporary `ubuntu:20.04` Docker container with `build-essential`,
  `libssl-dev`, and `python3`: `make clean`, `make`, and `make test` all exited 0.
  GCC compiled with the required C11/pthread/warning flags without warnings.
- All five C socket-test groups passed: coalesced/empty/binary lines, exact and
  oversized limits, closure/error paths, fragmented reads with EINTR, and a 2 MiB
  partial/interrupted send. `server --help` displayed the expected usage.
- No defects were exposed by this milestone's tests. Networking startup,
  authentication, persistence, and sessions are intentionally deferred until the
  separately committed server milestone. TCP is blocking and plaintext.
