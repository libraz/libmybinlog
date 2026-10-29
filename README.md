# libmybinlog

[![CI](https://img.shields.io/github/actions/workflow/status/libraz/libmybinlog/ci.yml?branch=main&label=CI)](https://github.com/libraz/libmybinlog/actions)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue)](https://github.com/libraz/libmybinlog/blob/main/LICENSE)
[![C++](https://img.shields.io/badge/C%2B%2B-17-blue?logo=c%2B%2B)](https://github.com/libraz/libmybinlog)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS-lightgrey)](https://github.com/libraz/libmybinlog)

**A C++17 library that decodes the MySQL and MariaDB binlog wire format.**

It works on bytes you already hold, from a binlog file or a replication stream, so it has no network code, no exceptions and no RTTI. It depends on zlib, and optionally on zstd.

**[Documentation (English)](https://github.com/libraz/libmybinlog/blob/main/docs/en/introduction.md)** · **[ドキュメント (日本語)](https://github.com/libraz/libmybinlog/blob/main/docs/ja/introduction.md)** · **[Supported servers](https://github.com/libraz/libmybinlog/blob/main/docs/en/compatibility.md)**

> Pre-1.0: the API may still change between minor releases.

## What's inside

- **Streaming decoder**: feed bytes in any split, take typed events one at a time ([guide](https://github.com/libraz/libmybinlog/blob/main/docs/en/decoder.md)).
- **Row images**: FULL, MINIMAL and NOBLOB images decoded into structured values ([guide](https://github.com/libraz/libmybinlog/blob/main/docs/en/rows.md)).
- **JSON**: MySQL binary JSON to server-identical text, and partial JSON updates applied ([guide](https://github.com/libraz/libmybinlog/blob/main/docs/en/json.md)).
- **Compression**: MariaDB zlib events and MySQL zstd transaction payloads expanded ([guide](https://github.com/libraz/libmybinlog/blob/main/docs/en/compression.md)).
- **Checksums and GTIDs**: per-file CRC32 handling, MySQL GTID sets with tags, MariaDB positions ([checksums](https://github.com/libraz/libmybinlog/blob/main/docs/en/checksums.md), [GTIDs](https://github.com/libraz/libmybinlog/blob/main/docs/en/gtids.md)).
- **Low-level API**: every per-event parser the decoder uses is public ([guide](https://github.com/libraz/libmybinlog/blob/main/docs/en/low-level-api.md)).

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

An installed copy is found with `find_package(mybinlog)` or `pkg-config mybinlog`. Build options and zstd support are described in [Getting started](https://github.com/libraz/libmybinlog/blob/main/docs/en/getting-started.md).

## Quick start

```cpp
#include <mybinlog/mybinlog.h>

#include <cstdio>
#include <variant>

void PrintRowCounts(const uint8_t* data, size_t size) {
  mybinlog::Decoder decoder;
  decoder.Feed(data, size);  // a whole binlog file, or stream bytes in any split
  mybinlog::Event event;
  mybinlog::Status status;
  while ((status = decoder.Next(&event)) == mybinlog::Status::kOk) {
    const auto* rows = std::get_if<mybinlog::TableRowsEvent>(&event.body);
    if (rows != nullptr && rows->decoded) {
      std::printf("%s.%s: %zu rows\n", rows->table->schema.c_str(), rows->table->table.c_str(),
                  rows->rows.rows.size() + rows->rows.updates.size());
    }
  }
  if (status == mybinlog::Status::kNeedMoreData) status = decoder.Finish();
  if (status != mybinlog::Status::kOk) {
    std::fprintf(stderr, "%s\n", decoder.last_error().message.c_str());
  }
}
```

[`examples/mybinlog-dump`](https://github.com/libraz/libmybinlog/blob/main/examples/mybinlog-dump/main.cpp) prints every event of a binlog file as JSON Lines.

## Build and test

```sh
make build         # configure and compile into build/
make test          # run the unit tests
make format        # clang-format, then clang-tidy
make format-check  # the same checks without modifying files
```

## License

Apache-2.0. See [LICENSE](https://github.com/libraz/libmybinlog/blob/main/LICENSE).
