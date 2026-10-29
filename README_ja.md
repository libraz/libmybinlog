# libmybinlog

[![CI](https://img.shields.io/github/actions/workflow/status/libraz/libmybinlog/ci.yml?branch=main&label=CI)](https://github.com/libraz/libmybinlog/actions)
[![Version](https://img.shields.io/github/v/release/libraz/libmybinlog?label=version)](https://github.com/libraz/libmybinlog/releases)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue)](https://github.com/libraz/libmybinlog/blob/main/LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue?logo=c%2B%2B)](https://en.cppreference.com/w/cpp/17)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS-lightgrey)](https://github.com/libraz/libmybinlog)

**libmybinlog は、MySQL と MariaDB の binlog のバイト列を、型付きのイベントと行の値に変換します。** 手元にある binlog ファイルや、レプリケーション接続のイベントストリームを読むライブラリなので、ネットワークのコードを含まず、例外も RTTI も使いません。依存は zlib だけで、zstd は任意です。

📖 **[ドキュメント](https://github.com/libraz/libmybinlog/blob/main/docs/ja/introduction.md)** &nbsp;·&nbsp; **[はじめての利用](https://github.com/libraz/libmybinlog/blob/main/docs/ja/getting-started.md)** &nbsp;·&nbsp; **[対応サーバー](https://github.com/libraz/libmybinlog/blob/main/docs/ja/compatibility.md)**

> 1.0 未満のため、マイナーリリースの間で API が変わることがあります。

## できること

- **ストリーミングデコーダ** — バイト列を任意の区切りで渡し、型付きのイベントを 1 つずつ取り出します。[Decoder](https://github.com/libraz/libmybinlog/blob/main/docs/ja/decoder.md)
- **行イメージ** — FULL・MINIMAL・NOBLOB の行イメージを構造化した値にデコードし、イメージに含まれない列を NULL と区別します。[行](https://github.com/libraz/libmybinlog/blob/main/docs/ja/rows.md) · [値](https://github.com/libraz/libmybinlog/blob/main/docs/ja/values.md)
- **JSON** — MySQL のバイナリ JSON をサーバーと同じテキストに描画し、部分 JSON 更新を適用します。[JSON](https://github.com/libraz/libmybinlog/blob/main/docs/ja/json.md)
- **圧縮イベント** — MariaDB の zlib イベントと MySQL の zstd トランザクションペイロードを展開します。[圧縮イベント](https://github.com/libraz/libmybinlog/blob/main/docs/ja/compression.md)
- **チェックサムと GTID** — ファイルごとの CRC32 の扱い、タグ付きを含む MySQL の GTID セット、MariaDB の GTID 位置。[チェックサム](https://github.com/libraz/libmybinlog/blob/main/docs/ja/checksums.md) · [GTID](https://github.com/libraz/libmybinlog/blob/main/docs/ja/gtids.md)
- **安全のための判定** — データを失ってはならない読み手のためのイベント分類と、静的なサーバー設定の規則。[イベントの扱い](https://github.com/libraz/libmybinlog/blob/main/docs/ja/disposition.md) · [サーバー設定](https://github.com/libraz/libmybinlog/blob/main/docs/ja/server-settings.md)
- **低レベル API** — デコーダが内部で使うイベントごとのパーサをすべて公開しています。[低レベル API](https://github.com/libraz/libmybinlog/blob/main/docs/ja/low-level-api.md)

## インストール

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

インストール済みのものは `find_package(mybinlog)` または `pkg-config mybinlog` で見つかります。

## クイックスタート

```cpp
#include <mybinlog/mybinlog.h>

#include <cstdio>
#include <variant>

void PrintRowCounts(const uint8_t* data, size_t size) {
  mybinlog::Decoder decoder;
  decoder.Feed(data, size);  // binlog ファイル全体、または任意に区切ったストリームのバイト列
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

[`examples/mybinlog-dump`](https://github.com/libraz/libmybinlog/blob/main/examples/mybinlog-dump/main.cpp) は binlog ファイルの全イベントを JSON Lines で出力します。→ [Decoder](https://github.com/libraz/libmybinlog/blob/main/docs/ja/decoder.md)

## ソースからビルド

```bash
make build         # build/ に構成してコンパイル
make test          # ビルドしてテストを実行
make format-check  # clang-format のチェックと clang-tidy
```

ビルドオプション（`MYBINLOG_WITH_ZSTD`、`MYBINLOG_BUILD_TESTS`、`MYBINLOG_INSTALL`、`MYBINLOG_BUILD_EXAMPLES`）は[はじめての利用](https://github.com/libraz/libmybinlog/blob/main/docs/ja/getting-started.md)を参照してください。

## ドキュメント

ガイドは [`docs/ja`](https://github.com/libraz/libmybinlog/blob/main/docs/ja/introduction.md) にあります。

- **学ぶ** — [はじめに](https://github.com/libraz/libmybinlog/blob/main/docs/ja/introduction.md) · [はじめての利用](https://github.com/libraz/libmybinlog/blob/main/docs/ja/getting-started.md) · [FAQ](https://github.com/libraz/libmybinlog/blob/main/docs/ja/faq.md)
- **ガイド** — [Decoder](https://github.com/libraz/libmybinlog/blob/main/docs/ja/decoder.md) · [行](https://github.com/libraz/libmybinlog/blob/main/docs/ja/rows.md) · [値](https://github.com/libraz/libmybinlog/blob/main/docs/ja/values.md) · [JSON](https://github.com/libraz/libmybinlog/blob/main/docs/ja/json.md) · [圧縮イベント](https://github.com/libraz/libmybinlog/blob/main/docs/ja/compression.md) · [チェックサム](https://github.com/libraz/libmybinlog/blob/main/docs/ja/checksums.md) · [GTID](https://github.com/libraz/libmybinlog/blob/main/docs/ja/gtids.md) · [イベントの扱い](https://github.com/libraz/libmybinlog/blob/main/docs/ja/disposition.md) · [サーバー設定](https://github.com/libraz/libmybinlog/blob/main/docs/ja/server-settings.md) · [低レベル API](https://github.com/libraz/libmybinlog/blob/main/docs/ja/low-level-api.md)
- **リファレンス** — [API リファレンス](https://github.com/libraz/libmybinlog/blob/main/docs/ja/api-reference.md) · [エラーと検証](https://github.com/libraz/libmybinlog/blob/main/docs/ja/errors-and-validation.md) · [対応状況](https://github.com/libraz/libmybinlog/blob/main/docs/ja/compatibility.md) · [用語集](https://github.com/libraz/libmybinlog/blob/main/docs/ja/glossary.md)

## 含まないもの（Non-goals）

libmybinlog が扱うのは形式のデコードまでです。レプリケーションクライアントではなく（接続・認証・ダンプの要求は呼び出し側が行います）、SQL パーサでもなく（QUERY イベントからは文のテキストが得られます）、変更データキャプチャのポリシーでもありません（イベントを分類し、どう扱うかは呼び出し側が決めます）。Windows では検証していません。詳しくは[扱わないこと](https://github.com/libraz/libmybinlog/blob/main/docs/ja/introduction.md#扱わないこと)を参照してください。

## ライセンス

[Apache-2.0](LICENSE)
