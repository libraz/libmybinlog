# libmybinlog

[![CI](https://img.shields.io/github/actions/workflow/status/libraz/libmybinlog/ci.yml?branch=main&label=CI)](https://github.com/libraz/libmybinlog/actions)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue)](https://github.com/libraz/libmybinlog/blob/main/LICENSE)
[![C++](https://img.shields.io/badge/C%2B%2B-17-blue?logo=c%2B%2B)](https://github.com/libraz/libmybinlog)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS-lightgrey)](https://github.com/libraz/libmybinlog)

**A C++17 library that decodes the MySQL and MariaDB binlog wire format.**

It works on bytes you already hold, so it has no network code, no exceptions and no RTTI. It covers MySQL 8.4 and 9.x and MariaDB 10.11 and later. The only dependency is zlib.

> Pre-1.0: the API may still change between tags.

## What's inside

- **Event framing**: common header parsing, event type names, and ROTATE bodies.
- **Checksums**: per-file CRC32 or NONE, derived from each FORMAT_DESCRIPTION_EVENT, with trailer verification.
- **TABLE_MAP**: column types, metadata and the optional metadata block, including column names.
- **QUERY**: fixed fields and status variables.
- **MySQL GTID**: UUID, tag and sequence number, including tagged GTIDs.
- **MariaDB events**: GTID, GTID_LIST and annotate-rows events.
- **Server flavor**: detection from a version string or a `gtid_binlog_pos` probe.

Decoding of row images is not part of this release.

## Installation

Add it with CMake `FetchContent`, pinned to a tag or commit:

```cmake
include(FetchContent)
FetchContent_Declare(
  libmybinlog
  GIT_REPOSITORY https://github.com/libraz/libmybinlog.git
  GIT_TAG <tag-or-commit>
)
FetchContent_MakeAvailable(libmybinlog)

target_link_libraries(your_target PRIVATE mybinlog::mybinlog)
```

For local development against a checkout, skip the download:

```sh
cmake -S . -B build -DFETCHCONTENT_SOURCE_DIR_LIBMYBINLOG=/path/to/libmybinlog
```

The tests and googletest are built only when libmybinlog is the top-level project (`-DMYBINLOG_BUILD_TESTS=ON` forces them on).

## Quick start

```cpp
#include "mybinlog/checksum.h"
#include "mybinlog/event_header.h"

mybinlog::ChecksumTracker tracker;  // one per binlog stream

// `frame` holds one whole event, `len` bytes.
mybinlog::EventHeader header;
if (mybinlog::ParseEventHeader(frame, len, &header) == mybinlog::Status::kOk) {
  mybinlog::FrameLayout layout;
  if (tracker.OnEvent(header, frame, len, &layout) == mybinlog::Status::kOk) {
    // layout.body_length() bytes of event body follow the common header.
  }
}
```

Every function returns a `mybinlog::Status` and writes its result through an out parameter.

## Build and test

```sh
make build         # configure and compile into build/
make test          # run the unit tests (3 parallel jobs by default)
make format        # clang-format, then clang-tidy
make format-check  # the same checks without modifying files
```

Fixtures under `tests/fixtures/` are binlog files captured from real servers; `tests/fixtures/README.md` describes how to regenerate them.

## License

Apache-2.0. See [LICENSE](LICENSE).
