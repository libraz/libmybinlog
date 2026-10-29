# Getting started

## Requirements

- A C++17 compiler. CI builds with GCC and Clang on Linux and with Clang on macOS.
- CMake 3.18 or later.
- zlib, found with `find_package(ZLIB)` unless the including project already defines `ZLIB::ZLIB`.
- zstd, optional, to expand MySQL's compressed transaction payloads. See [Compressed events](compression.md).

## Add it to a CMake project

### FetchContent

Pin a tag or a commit:

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

When libmybinlog is not the top-level project, its tests, examples, install rules and zstd support are off by default. To read MySQL binlogs written with `binlog_transaction_compression=ON`, turn zstd on before `FetchContent_MakeAvailable`:

```cmake
set(MYBINLOG_WITH_ZSTD ON)
```

To build against a local checkout instead of downloading:

```sh
cmake -S . -B build -DFETCHCONTENT_SOURCE_DIR_LIBMYBINLOG=/path/to/libmybinlog
```

### Installed package

Build and install the library on its own:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMYBINLOG_BUILD_TESTS=OFF
cmake --build build
cmake --install build --prefix /usr/local
```

Add `-DBUILD_SHARED_LIBS=ON` for a shared library. A shared build exports only the public symbols (`MYBINLOG_API`) and carries the major and minor version in its soname.

The install provides a CMake package and a pkg-config file. With CMake:

```cmake
find_package(mybinlog CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE mybinlog::mybinlog)
```

A version request in `find_package` matches only installs with the same major and minor number, because a minor release may change the API before 1.0. A static install finds zlib, and zstd when it was built with it, for the consumer's link.

With pkg-config:

```sh
c++ -std=c++17 app.cpp $(pkg-config --cflags --libs mybinlog)
```

For a static install, `mybinlog.pc` adds `-DMYBINLOG_STATIC_DEFINE` to the compile flags, `-lz` to the libraries and `libzstd` to `Requires`, so `--static` is not needed.

## Build options

| Option | Default | Effect |
|---|---|---|
| `MYBINLOG_BUILD_TESTS` | ON at top level, OFF when included | Builds the unit tests and fetches googletest. |
| `MYBINLOG_BUILD_EXAMPLES` | ON at top level, OFF when included | Builds `examples/mybinlog-dump`. |
| `MYBINLOG_INSTALL` | ON at top level, OFF when included | Generates the install rules, `mybinlogConfig.cmake` and `mybinlog.pc`. |
| `MYBINLOG_WITH_ZSTD` | ON at top level, OFF when included | Links zstd, found through its CMake package or through pkg-config (`libzstd`). If zstd is not found, configuration prints a warning and zstd payloads decode as `kUnsupported`. |
| `BUILD_SHARED_LIBS` | OFF | Builds a shared library instead of a static one. |

## A first program

The program below reads a binlog file in 64 KiB chunks and prints every row of every rows event with its column names.

```cpp
#include <mybinlog/mybinlog.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <variant>
#include <vector>

namespace {

void PrintRow(const mybinlog::Row& row, const mybinlog::TableMap& table) {
  for (size_t i = 0; i < row.size(); ++i) {
    const mybinlog::Value& value = row[i];
    const std::string& name = table.columns[i].name;  // empty without FULL row metadata
    std::printf("  %s=", name.empty() ? ("@" + std::to_string(i + 1)).c_str() : name.c_str());
    std::string text;
    if (value.kind == mybinlog::Value::Kind::kNull) {
      std::printf("NULL\n");
    } else if (value.kind == mybinlog::Value::Kind::kAbsent) {
      std::printf("(not in image)\n");
    } else if (mybinlog::FormatValue(value, &text) == mybinlog::Status::kOk) {
      std::printf("%s\n", text.c_str());
    } else {
      std::printf("(malformed)\n");
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <binlog file>\n", argv[0]);
    return 2;
  }
  std::FILE* file = std::fopen(argv[1], "rb");
  if (file == nullptr) {
    std::perror(argv[1]);
    return 2;
  }

  mybinlog::Decoder decoder;
  mybinlog::Event event;
  std::vector<uint8_t> chunk(64 * 1024);
  mybinlog::Status status = mybinlog::Status::kOk;
  for (;;) {
    const size_t n = std::fread(chunk.data(), 1, chunk.size(), file);
    decoder.Feed(chunk.data(), n);
    while ((status = decoder.Next(&event)) == mybinlog::Status::kOk) {
      const auto* rows = std::get_if<mybinlog::TableRowsEvent>(&event.body);
      if (rows == nullptr || !rows->decoded) continue;
      std::printf("%s at %llu: %s.%s\n", mybinlog::EventTypeName(event.header.type_code),
                  static_cast<unsigned long long>(event.offset), rows->table->schema.c_str(),
                  rows->table->table.c_str());
      for (const mybinlog::Row& row : rows->rows.rows) PrintRow(row, *rows->table);
      for (const auto& [before, after] : rows->rows.updates) {
        PrintRow(before, *rows->table);
        std::printf("  ->\n");
        PrintRow(after, *rows->table);
      }
    }
    if (status != mybinlog::Status::kNeedMoreData) break;  // a decode error
    if (n < chunk.size()) {                                // end of file
      status = decoder.Finish();
      break;
    }
  }
  std::fclose(file);
  if (status != mybinlog::Status::kOk) {
    std::fprintf(stderr, "%s\n", decoder.last_error().message.c_str());
    return 1;
  }
  return 0;
}
```

Run against a file from a MySQL server with `binlog_row_metadata=FULL`, it prints lines such as:

```
WRITE_ROWS_EVENT at 1252: fx.t
  id=1
  i8=-128
  dt=2024-02-29 23:59:59.123
  j={"k": [1, 2]}
  n=NULL
```

The loop shows the whole contract: `Next()` returns `kOk` with an event, `kNeedMoreData` when the buffered bytes hold no complete event, or an error that `last_error()` describes. `Finish()` reports a file that ends partway through an event. [The Decoder](decoder.md) covers each of these.

## The mybinlog-dump example

A top-level build also produces `examples/mybinlog-dump`, which prints every event of one or more binlog files as JSON Lines and reads gzip-compressed files directly:

```sh
build/examples/mybinlog-dump/mybinlog-dump [--no-verify] binlog.000001
```

Each line holds the event's position, type name and disposition, plus the parsed fields; row values are keyed by column name and rendered with `FormatValue`. Its source is a complete use of the decoder in under 300 lines.

## Checking the version

`MYBINLOG_VERSION_STRING` (and `MYBINLOG_VERSION_MAJOR`, `_MINOR`, `_PATCH`) in `mybinlog/version.h` give the version of the headers being compiled against; `mybinlog::Version()` returns the version of the library actually linked. The two differ only when a shared library of another version is loaded at run time.
