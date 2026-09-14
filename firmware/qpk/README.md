# QPK1 parser (firmware side)

**Status: implemented — Phase 1, Milestone 2.**

Read-only, validating parser for the container specified in
[docs/qpk-format.md](../../docs/qpk-format.md).

| File | Role |
|---|---|
| `crc32.cpp` | CRC-32/ISO-HDLC, nibble table |
| `qpk_format.cpp` | record sizes and names |
| `qpk_reader.cpp` | validation and direct access |

Headers live in `firmware/include/qpk/`.

Properties this code is required to keep:

- **No scanning.** Every lookup is an index computation, never a search.
- **No dynamic allocation.** Only the section table (≤ 32 entries) is cached;
  records are read on demand into caller storage.
- **No struct casting.** All fields are decoded byte-wise little-endian, so
  unaligned fields, padding and host endianness never matter — and the same
  code runs in the host tests.
- **Subtraction-form bounds checks.** `offset + length` overflows on a hostile
  file; `length <= limit - offset` does not.
- **No Arduino.** The parser reads through `hal::IFile`, so it works against
  an SD card on the device and a memory buffer on the host.

Tests: `firmware/test/test_qpk/`, one case per validation rule.
