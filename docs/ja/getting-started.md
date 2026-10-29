# はじめての利用

## 必要なもの

- C++17 コンパイラ。CI は Linux の GCC と Clang、macOS の Clang でビルドしています。
- CMake 3.18 以降。
- zlib。取り込み側のプロジェクトが `ZLIB::ZLIB` をすでに定義していなければ `find_package(ZLIB)` で探します。
- zstd（任意）。MySQL の圧縮されたトランザクションペイロードを展開するのに使います。[圧縮イベント](compression.md)を参照してください。

## CMake プロジェクトへの追加

### FetchContent

タグかコミットに固定します。

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

libmybinlog がトップレベルのプロジェクトでないときは、テスト、サンプル、インストール規則、zstd 対応が既定で無効になります。`binlog_transaction_compression=ON` で書かれた MySQL の binlog を読むには、`FetchContent_MakeAvailable` の前に zstd を有効にします。

```cmake
set(MYBINLOG_WITH_ZSTD ON)
```

ダウンロードせずにローカルのチェックアウトを使うには、次のようにします。

```sh
cmake -S . -B build -DFETCHCONTENT_SOURCE_DIR_LIBMYBINLOG=/path/to/libmybinlog
```

### インストール済みのパッケージ

ライブラリ単体でビルドしてインストールします。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMYBINLOG_BUILD_TESTS=OFF
cmake --build build
cmake --install build --prefix /usr/local
```

共有ライブラリにするには `-DBUILD_SHARED_LIBS=ON` を加えます。共有ビルドは公開シンボル（`MYBINLOG_API`）だけを export し、soname にメジャーとマイナーの版を持ちます。

インストールすると CMake パッケージと pkg-config のファイルが入ります。CMake では次のように使います。

```cmake
find_package(mybinlog CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE mybinlog::mybinlog)
```

1.0 より前はマイナーリリースで API が変わりうるため、`find_package` で版を指定すると、メジャーとマイナーが同じインストールだけに一致します。静的ライブラリのインストールは、利用側のリンクのために zlib を探し、zstd 付きでビルドされていれば zstd も探します。

pkg-config では次のように使います。

```sh
c++ -std=c++17 app.cpp $(pkg-config --cflags --libs mybinlog)
```

静的ライブラリのインストールでは、`mybinlog.pc` がコンパイルフラグに `-DMYBINLOG_STATIC_DEFINE`、ライブラリに `-lz`、`Requires` に `libzstd` を加えるので、`--static` は要りません。

## ビルドオプション

| オプション | 既定値 | 効果 |
|---|---|---|
| `MYBINLOG_BUILD_TESTS` | トップレベルでは ON、取り込まれたときは OFF | ユニットテストをビルドし、googletest を取得します。 |
| `MYBINLOG_BUILD_EXAMPLES` | トップレベルでは ON、取り込まれたときは OFF | `examples/mybinlog-dump` をビルドします。 |
| `MYBINLOG_INSTALL` | トップレベルでは ON、取り込まれたときは OFF | インストール規則、`mybinlogConfig.cmake`、`mybinlog.pc` を生成します。 |
| `MYBINLOG_WITH_ZSTD` | トップレベルでは ON、取り込まれたときは OFF | zstd をリンクします。zstd は CMake パッケージか pkg-config（`libzstd`）で探します。見つからないと configure が警告を出し、zstd のペイロードは `kUnsupported` になります。 |
| `BUILD_SHARED_LIBS` | OFF | 静的ライブラリの代わりに共有ライブラリをビルドします。 |

## 最初のプログラム

次のプログラムは binlog ファイルを 64 KiB ずつ読み、全行イベントの全行を列名付きで表示します。

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

`binlog_row_metadata=FULL` の MySQL サーバーのファイルに対して実行すると、次のような行を表示します。

```
WRITE_ROWS_EVENT at 1252: fx.t
  id=1
  i8=-128
  dt=2024-02-29 23:59:59.123
  j={"k": [1, 2]}
  n=NULL
```

このループに約束事がすべて現れています。`Next()` はイベントとともに `kOk` を返すか、バッファに完全なイベントがないとき `kNeedMoreData` を返すか、`last_error()` が内容を説明するエラーを返します。`Finish()` はイベントの途中で終わったファイルを報告します。それぞれ [Decoder](decoder.md) で説明しています。

## サンプル mybinlog-dump

トップレベルでビルドすると `examples/mybinlog-dump` もできます。1 つ以上の binlog ファイルの全イベントを JSON Lines で出力し、gzip 圧縮されたファイルもそのまま読みます。

```sh
build/examples/mybinlog-dump/mybinlog-dump [--no-verify] binlog.000001
```

各行はイベントの位置、型名、扱い（disposition）と、解析したフィールドを持ちます。行の値は列名をキーにし、`FormatValue` で描画します。300 行に満たないソースで、デコーダの使い方を一通り示しています。

## 版の確認

`mybinlog/version.h` の `MYBINLOG_VERSION_STRING`（と `MYBINLOG_VERSION_MAJOR`、`_MINOR`、`_PATCH`）はコンパイルに使うヘッダの版を、`mybinlog::Version()` は実際にリンクされたライブラリの版を返します。両者が異なるのは、実行時に別の版の共有ライブラリが読み込まれたときだけです。
