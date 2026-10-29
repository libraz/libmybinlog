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

`-DMYBINLOG_WITH_ZSTD=ON` (the default only at top level) links zstd to expand MySQL's compressed transaction payloads; without it they return `kUnsupported`.

## Quick start

```cpp
#include <mybinlog/mybinlog.h>

mybinlog::Decoder decoder;
decoder.Feed(data, size);  // a whole binlog file, or stream bytes in any split
mybinlog::Event event;
while (decoder.Next(&event) == mybinlog::Status::kOk) {
  if (auto* rows = std::get_if<mybinlog::TableRowsEvent>(&event.body); rows && rows->table) {
    std::printf("%s.%s: %zu rows\n", rows->table->schema.c_str(), rows->table->table.c_str(),
                rows->rows.rows.size() + rows->rows.updates.size());
  }
}
```

`Next` returns `kNeedMoreData` once the buffered bytes are used up; any other status is an error, described by `decoder.last_error()`. [`examples/mybinlog-dump`](examples/mybinlog-dump/main.cpp) prints every event of a binlog file as JSON Lines. The per-event functions under the decoder (`ParseTableMapEvent`, `DecodeRowsEvent`, ...) are public too.

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
