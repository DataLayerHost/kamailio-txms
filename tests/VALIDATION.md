# Validation report — 2026-09-28

Executed locally; GitHub Actions workflows are added but have not run remotely.

| Check | Result |
| --- | --- |
| macOS arm64, Apple Clang 21, CMake, ASan + UBSan | All library/gateway CTest suites passed |
| Debian 12 arm64 container, Clang 14, ASan + UBSan | All library/gateway CTest suites passed |
| TypeScript reference unit suite (temporary copy) | 68 tests passed |
| Flutter reference unit suite (temporary copy) | 35 tests passed |
| C vs TypeScript, Node 24.2.0 / Unicode 16 | 66,543 encode/decode comparisons passed |
| C vs Flutter | 6 matching encodings; 2 matching decodes; 4 documented upstream escape differences verified |
| libFuzzer SMS, UDH, TxMS | 20,000 seeded runs each, no sanitizer findings |
| libFuzzer MIME, final strict transfer decoder | 20,000 seeded runs, no sanitizer findings |
| Native Kamailio 6.0 module, GCC, warnings as errors | txms.so compiled and config validation passed |
| Live Kamailio integration | 2 SIP workers; native RPC stats, timer expiry without SIP traffic, repeat submission after expiry, no database files |
| RPC integration | Success, RPC error, HTTP error, timeout, malformed/oversized/trailing JSON, wrong ID, conflicting result/error, null result, basic/bearer auth, TLS verification |
| Shared libtxms installation | Relocated pkg-config and find_package consumer built and executed |
| GitHub workflow/issue YAML | Parsed successfully |

The C unit suites include exhaustive BMP codec round trips, strict hex/batch errors,
UTF-16 validation, SMS framing/alphabet/alignment, multipart ordering, collisions,
duplicate/conflicting segments, missing/expired segments, memory/queue limits,
transaction rollback, separate-process assembly, MIME attachments and transfer
validation, notification rejection, and in-flight queue claims protected from expiry.

No public blockchain calls were made. The original reference repositories were
left unchanged. Fuzz smoke tests do not substitute for a security audit. Provider
interoperability, sustained production load, and a real Core node broadcast are
not validated. Choose the RPC method and verify its one-hex-string parameter and
transaction-ID result contract before deployment. MMS URL retrieval is unsupported.

## Release and Conan automation follow-up

- GitHub Actions workflows passed `actionlint` 1.7.12 and YAML parsing.
- Release tooling tests passed: tag/version matching, exact-commit archives,
  dirty-tree rejection and dependency metadata.
- libtxms Conan 2.32.0 static/shared packages and their separate C consumer passed
  on macOS arm64 and Debian 12 arm64.
- macOS codec and gateway sanitizer suites passed after the CMake packaging changes.
- No release was published during these local checks. Hosted Windows packaging
  and the actual GitHub publication steps remain to be exercised by CI after push.

## Database removal follow-up

- Replaced the database with allocator/lock callbacks backed by Kamailio shared
  memory, a native lock and timer cleanup; no SQLite build/runtime dependency.
- macOS and Debian ASan/UBSan CTest suites passed with the new store.
- Tests exercise assembly across forked processes, concurrent duplicate enqueue,
  idle expiry, in-flight retention, byte reclamation, allocation failure rollback,
  final-segment rollback when the queue is full, contention, and fresh state reset.
- State is intentionally volatile; restart recovery and permanent deduplication
  are not guarantees of this implementation.
