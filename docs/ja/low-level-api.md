# 低レベル API

`Decoder` は、公開されたイベント単位の関数で組み立てられています。イベントの枠がすでに切り出されているとき（パケットごとに 1 イベントを受け取るレプリケーションクライアント）、1 つの段階だけが必要なとき（チェックポイントから読んだ GTID セットの解析）、行をワーカースレッドでデコードするなどデコーダの選択が合わないときに、直接使います。

## 規約

イベント本体のパーサはすべて同じ形です。

```cpp
Status ParseXxx(const EventHeader& header, const uint8_t* body, size_t size, Xxx* out);
```

- `header` は解析済みの共通ヘッダです。読むパーサ（型コードでレイアウトを選ぶ、あるいはフィールドをヘッダから取る）と、署名をそろえるためだけに受け取るパーサがあります。
- `body` は 19 バイトのヘッダの後ろを指し、`size` はチェックサムのトレーラを除いた本体の長さで、`FrameLayout::body_length()` が返す値です。
- `out` は `kOk` のときに埋まり、それ以外では触れません。`body` が null なら `kTruncated` で失敗します。
- 関数は再入可能で、状態を持ちません。

## イベントの型ごとの関数

| イベント | 関数 | 結果 |
|---|---|---|
| 共通ヘッダ | `ParseEventHeader(data, size, out)` | `EventHeader` |
| 枠とチェックサム | `ChecksumTracker::OnEvent()` | `FrameLayout`。[チェックサム](checksums.md)を参照 |
| FORMAT_DESCRIPTION (15) | `ParseFormatDescriptionEvent()` | `FormatDescriptionEvent` |
| ROTATE (4) | `ParseRotateEvent()` | `RotateEvent` |
| TABLE_MAP (19) | `ParseTableMapEvent(header, body, size, flavor, out)` | `TableMap` |
| 行イベント (23-25, 30-32, 39) | `DecodeRowsEvent(header, body, size, table, options, out)` | `RowsEvent`。[行](rows.md)を参照 |
| QUERY (2) | `ParseQueryEvent()` | `QueryEvent` |
| XID (16) | `ParseXidEvent()` | `XidEvent` |
| GTID (33)、GTID_TAGGED (42) | `ParseGtidEvent()` | `GtidEvent` |
| PREVIOUS_GTIDS (35) | `ParsePreviousGtidsEvent()` | `PreviousGtidsEvent` |
| MariaDB GTID (162) | `ParseMariaDbGtidEvent()` | `MariaDbGtidEvent` |
| MariaDB GTID_LIST (163) | `ParseMariaDbGtidListEvent()` | `MariaDbGtidListEvent` |
| MariaDB ANNOTATE_ROWS (160) | `ParseMariaDbAnnotateRowsEvent()` | `MariaDbAnnotateRowsEvent` |
| HEARTBEAT (27)、HEARTBEAT_V2 (41) | `ParseHeartbeatEvent()` | `HeartbeatEvent` |
| MariaDB の圧縮イベント (165-171) | `DecompressMariaDbEvent()` | `DecompressedEvent`。[圧縮イベント](compression.md)を参照 |
| TRANSACTION_PAYLOAD (40) | `ParseTransactionPayloadEvent()`、`DecompressTransactionPayload()`、`NextPayloadEvent()` | 中のイベント |
| すべて | `ClassifyEvent()` | `EventDisposition`。[イベントの扱い](disposition.md)を参照 |

補助関数として、`EventTypeName()` は型コードのサーバー上の名前を返し、`IsRowsEvent()` は `DecodeRowsEvent()` がそのまま受け付ける型で true を返し、`RealType()` と `HasSignednessBit()` は TABLE_MAP の列を解釈します。

`ParseTableMapEvent()` がサーバーの種類を受け取るのは、2 つのサーバーで文字セットのメタデータが対象とする列の範囲が異なるためです（MariaDB は GEOMETRY を文字列の列として数えます）。`QueryEvent` は、サーバーと同じく、パーサが知らない最初のコードまでのステータス変数を報告します。途中で止まったときは `status_vars_complete` が false です。

## ファイルを手で走査する

次の関数は、メモリ上にある binlog ファイル 1 つ全体を走査し、行イベントをデコードします。ファイルに対してデコーダが行う処理から、圧縮とストリーミング用のバッファを除いたものにほぼ相当します。

```cpp
#include <mybinlog/mybinlog.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <utility>
#include <vector>

mybinlog::Status Walk(const std::vector<uint8_t>& file) {
  using mybinlog::Status;
  if (file.size() < mybinlog::kBinlogMagic.size() ||
      std::memcmp(file.data(), mybinlog::kBinlogMagic.data(), mybinlog::kBinlogMagic.size()) != 0) {
    return Status::kMalformedValue;
  }
  mybinlog::ChecksumTracker tracker;
  mybinlog::ServerFlavor flavor = mybinlog::ServerFlavor::kMySQL;
  std::unordered_map<uint64_t, mybinlog::TableMap> tables;

  size_t pos = mybinlog::kBinlogMagicOffset;
  while (pos < file.size()) {
    const uint8_t* frame = file.data() + pos;
    mybinlog::EventHeader header;
    Status status = mybinlog::ParseEventHeader(frame, file.size() - pos, &header);
    if (status != Status::kOk) return status;
    if (header.event_length < mybinlog::kEventHeaderSize) return Status::kMalformedValue;
    if (header.event_length > file.size() - pos) return Status::kTruncated;

    mybinlog::FrameLayout layout;
    status = tracker.OnEvent(header, frame, header.event_length, &layout);
    if (status != Status::kOk) return status;
    const uint8_t* body = frame + mybinlog::kEventHeaderSize;
    const size_t size = layout.body_length();

    switch (static_cast<mybinlog::EventType>(header.type_code)) {
      case mybinlog::EventType::kFormatDescriptionEvent: {
        mybinlog::FormatDescriptionEvent fde;
        status = mybinlog::ParseFormatDescriptionEvent(header, body, size, &fde);
        if (status != Status::kOk) return status;
        flavor = mybinlog::DetectFlavorFromVersion(fde.server_version);
        tables.clear();
        break;
      }
      case mybinlog::EventType::kTableMapEvent: {
        mybinlog::TableMap map;
        status = mybinlog::ParseTableMapEvent(header, body, size, flavor, &map);
        if (status != Status::kOk) return status;
        const uint64_t id = map.table_id;
        tables[id] = std::move(map);
        break;
      }
      default:
        if (mybinlog::IsRowsEvent(header.type_code)) {
          mybinlog::RowsEventHeader fixed;
          status = mybinlog::ParseRowsEventHeader(header, body, size, &fixed);
          if (status != Status::kOk) return status;
          const auto table = tables.find(fixed.table_id);
          if (table == tables.end()) return Status::kInvalidMetadata;
          mybinlog::RowsEvent rows;
          status = mybinlog::DecodeRowsEvent(header, body, size, table->second,
                                             mybinlog::DecodeOptions(), &rows);
          if (status != Status::kOk) return status;
          std::printf("%s.%s: %zu rows, %zu updates\n", table->second.schema.c_str(),
                      table->second.table.c_str(), rows.rows.size(), rows.updates.size());
        }
        break;
    }
    pos += header.event_length;
  }
  return Status::kOk;
}
```

MariaDB は v1 の行イベント (23-25)、MySQL は v2 (30-32) を書きます。`IsRowsEvent()` は両方を含み、`DecodeRowsEvent()` はヘッダからレイアウトを選びます。`ParseRowsEventHeader()` はポストヘッダから 48 ビットのテーブル ID とフラグを読むので、呼び出し側はデコードの前に TABLE_MAP を選べます。

## ワーカースレッドでの行のデコード

`DecodeRowsEvent()` は `TableMap` を呼び出しの間だけ読み、状態を持たないので、行イベントを並列にデコードできます。1 つのスレッドでイベントを順に切り出して解析し、テーブルの対応もそこで保持し、各行イベントをイベントのバイト列と `TableMap` のコピーとともにワーカーへ渡します。`DecodeBudget` を使う場合、並行する呼び出しで共有してはいけません。
