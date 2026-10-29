# libmybinlog

[![CI](https://img.shields.io/github/actions/workflow/status/libraz/libmybinlog/ci.yml?branch=main&label=CI)](https://github.com/libraz/libmybinlog/actions)
[![Version](https://img.shields.io/github/v/release/libraz/libmybinlog?label=version)](https://github.com/libraz/libmybinlog/releases)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue)](https://github.com/libraz/libmybinlog/blob/main/LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue?logo=c%2B%2B)](https://en.cppreference.com/w/cpp/17)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS-lightgrey)](https://github.com/libraz/libmybinlog)

**libmybinlog turns MySQL and MariaDB binlog bytes into typed events and row values.** It reads a binlog file or the event stream of a replication connection that you already hold, so it has no network code, no exceptions and no RTTI. It depends on zlib, and optionally on zstd.

📖 **[Documentation](https://github.com/libraz/libmybinlog/blob/main/docs/en/introduction.md)** &nbsp;·&nbsp; **[Getting started](https://github.com/libraz/libmybinlog/blob/main/docs/en/getting-started.md)** &nbsp;·&nbsp; **[Supported servers](https://github.com/libraz/libmybinlog/blob/main/docs/en/compatibility.md)**

> Pre-1.0: the API may still change between minor releases.

## What's inside

- **Streaming decoder** — feed bytes in any split and take typed events one at a time. [The Decoder](https://github.com/libraz/libmybinlog/blob/main/docs/en/decoder.md)
- **Row images** — FULL, MINIMAL and NOBLOB images decoded into structured values, with absent columns kept apart from NULL. [Rows](https://github.com/libraz/libmybinlog/blob/main/docs/en/rows.md) · [Values](https://github.com/libraz/libmybinlog/blob/main/docs/en/values.md)
- **JSON** — MySQL binary JSON rendered as the server's own text, and partial JSON updates applied. [JSON](https://github.com/libraz/libmybinlog/blob/main/docs/en/json.md)
- **Compressed events** — MariaDB zlib events and MySQL zstd transaction payloads expanded. [Compressed events](https://github.com/libraz/libmybinlog/blob/main/docs/en/compression.md)
- **Checksums and GTIDs** — per-file CRC32 handling, MySQL GTID sets with tags, MariaDB GTID positions. [Checksums](https://github.com/libraz/libmybinlog/blob/main/docs/en/checksums.md) · [GTIDs](https://github.com/libraz/libmybinlog/blob/main/docs/en/gtids.md)
- **Safety checks** — event classification for readers that must not lose data, and static server-setting rules. [Event disposition](https://github.com/libraz/libmybinlog/blob/main/docs/en/disposition.md) · [Server settings](https://github.com/libraz/libmybinlog/blob/main/docs/en/server-settings.md)
- **Low-level API** — every per-event parser the decoder uses is public. [Low-level API](https://github.com/libraz/libmybinlog/blob/main/docs/en/low-level-api.md)

## Installation

```cmake
include(FetchContent)
FetchContent_Declare(
  libmybinlog
  GIT_REPOSITORY https://github.com/libraz/libmybinlog.git
  GIT_TAG v0.1.0
)
FetchContent_MakeAvailable(libmybinlog)

target_link_libraries(your_target PRIVATE mybinlog::mybinlog)
```

An installed copy is found with `find_package(mybinlog)` or `pkg-config mybinlog`.

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

[`examples/mybinlog-dump`](https://github.com/libraz/libmybinlog/blob/main/examples/mybinlog-dump/main.cpp) prints every event of a binlog file as JSON Lines. → [The Decoder](https://github.com/libraz/libmybinlog/blob/main/docs/en/decoder.md)

## Build from source

```bash
make build         # configure and compile into build/
make test          # build, then run the tests
make format-check  # clang-format check and clang-tidy
```

Build options (`MYBINLOG_WITH_ZSTD`, `MYBINLOG_BUILD_TESTS`, `MYBINLOG_INSTALL`, `MYBINLOG_BUILD_EXAMPLES`) are covered in [Getting started](https://github.com/libraz/libmybinlog/blob/main/docs/en/getting-started.md).

## Documentation

The guides live in [`docs/en`](https://github.com/libraz/libmybinlog/blob/main/docs/en/introduction.md).

- **Learn** — [Introduction](https://github.com/libraz/libmybinlog/blob/main/docs/en/introduction.md) · [Getting started](https://github.com/libraz/libmybinlog/blob/main/docs/en/getting-started.md) · [FAQ](https://github.com/libraz/libmybinlog/blob/main/docs/en/faq.md)
- **Guides** — [The Decoder](https://github.com/libraz/libmybinlog/blob/main/docs/en/decoder.md) · [Rows](https://github.com/libraz/libmybinlog/blob/main/docs/en/rows.md) · [Values](https://github.com/libraz/libmybinlog/blob/main/docs/en/values.md) · [JSON](https://github.com/libraz/libmybinlog/blob/main/docs/en/json.md) · [Compressed events](https://github.com/libraz/libmybinlog/blob/main/docs/en/compression.md) · [Checksums](https://github.com/libraz/libmybinlog/blob/main/docs/en/checksums.md) · [GTIDs](https://github.com/libraz/libmybinlog/blob/main/docs/en/gtids.md) · [Event disposition](https://github.com/libraz/libmybinlog/blob/main/docs/en/disposition.md) · [Server settings](https://github.com/libraz/libmybinlog/blob/main/docs/en/server-settings.md) · [Low-level API](https://github.com/libraz/libmybinlog/blob/main/docs/en/low-level-api.md)
- **Reference** — [API reference](https://github.com/libraz/libmybinlog/blob/main/docs/en/api-reference.md) · [Errors and validation](https://github.com/libraz/libmybinlog/blob/main/docs/en/errors-and-validation.md) · [Compatibility](https://github.com/libraz/libmybinlog/blob/main/docs/en/compatibility.md) · [Glossary](https://github.com/libraz/libmybinlog/blob/main/docs/en/glossary.md)

## Non-goals

libmybinlog decodes the format and stops there. It is not a replication client (connecting, authenticating and requesting the dump are the caller's job), not a SQL parser (QUERY events give statement text), and not a change-data-capture policy (it classifies events; the caller decides what to do). Windows is not tested. See [Introduction](https://github.com/libraz/libmybinlog/blob/main/docs/en/introduction.md#what-it-is-not) for the details.

## License

[Apache-2.0](LICENSE)
