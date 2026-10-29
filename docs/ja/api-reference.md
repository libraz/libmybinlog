# API リファレンス

このページは公開ヘッダの地図です。シンボル単位のリファレンスは `include/mybinlog/` の宣言と Doxygen コメントで、各関数が引数、返すステータス、指すものの寿命、スレッド安全性を記述しています。

## インクルードとリンク

- `#include <mybinlog/mybinlog.h>` で全公開ヘッダを読み込めます。各ヘッダを単独で読み込むこともできます。
- CMake のターゲット `mybinlog::mybinlog` をリンクするか、`pkg-config mybinlog` を使います。[はじめての利用](getting-started.md)を参照してください。
- すべて名前空間 `mybinlog` にあります。公開の関数とクラスには `MYBINLOG_API`（生成される `mybinlog/export.h`）が付き、共有ビルドではこれだけが export されます。静的ビルドは、CMake のターゲットか `mybinlog.pc` を通じて利用側に `MYBINLOG_STATIC_DEFINE` を定義します。
- `mybinlog/version.h` は configure のときに生成され、版のマクロと `Version()` を持ちます。

## ヘッダ

| ヘッダ | 内容 | ガイド |
|---|---|---|
| `decoder.h` | `Decoder`、`DecoderOptions`、`DecodeError` | [Decoder](decoder.md) |
| `event.h` | `Event`、`EventBody`、`TableRowsEvent`、`UnknownEvent` | [Decoder](decoder.md) |
| `status.h` | `Status`、`StatusName()` | [エラーと検証](errors-and-validation.md) |
| `event_header.h` | `EventHeader`、`ParseEventHeader()`、`kEventHeaderSize`、`kBinlogMagic` | [低レベル API](low-level-api.md) |
| `event_types.h` | `EventType`、`ColumnType`、ヘッダのフラグ、`EventTypeName()`、`IsRowsEvent()` | [低レベル API](low-level-api.md) |
| `checksum.h` | `ChecksumAlgorithm`、`ChecksumTracker`、`FrameLayout`、`ReadFdeChecksumAlgorithm()` | [チェックサム](checksums.md) |
| `crc32.h` | `ComputeCrc32()`、`UpdateCrc32()` | [チェックサム](checksums.md) |
| `format_description.h` | `FormatDescriptionEvent`、`ParseFormatDescriptionEvent()` | [チェックサム](checksums.md) |
| `table_map.h` | `TableMap`、`TableMapColumn`、`ParseTableMapEvent()`、`ParseTableMapEventWithDiagnostics()`、`TableMapDiagnostics`、`RealType()` | [行](rows.md) |
| `row_decoder.h` | `RowsEvent`、`Row`、`DecodeOptions`、`DecodeBudget`、`DecodeRowsEvent()`、`RowScratch`、`RowVisitorFn`、`VisitRowsEvent()`、`RowsEventHeader`、`ParseRowsEventHeader()`、`kRowsStatementEndFlag`、`kDummyTableId` | [行](rows.md) |
| `value.h` | `Value`、`Temporal`、`ValueKindName()` | [値](values.md) |
| `format.h` | `FormatValue()`、`FormatEnumLabel()`、`FormatSetLabels()` | [値](values.md) |
| `json.h` | `JsonValue`、`ParseJsonBinary()`、`JsonToText()`、`JsonBinaryToText()`、`FindJsonMember()` | [JSON](json.md) |
| `json_diff.h` | `JsonDiff`、`ParseJsonDiffs()`、`ApplyJsonDiffs()`、`ApplyJsonDiffsToBinary()` | [JSON](json.md) |
| `compression.h` | `HasZstdSupport()`、`DecompressMariaDbEvent()`、`ParseTransactionPayloadEvent()`、`DecompressTransactionPayload()`、`NextPayloadEvent()` | [圧縮イベント](compression.md) |
| `mysql_gtid.h` | `GtidSet`、`GtidEvent`、`PreviousGtidsEvent`、`ParseGtidEvent()`、UUID の補助関数 | [GTID](gtids.md) |
| `mariadb_gtid.h` | `MariaDbGtid`、`MariaDbGtidPosition`、`PositionCovers()`、MariaDB の GTID・GTID_LIST・ANNOTATE_ROWS イベント | [GTID](gtids.md) |
| `query_event.h` | `QueryEvent`、`ParseQueryEvent()`、`ParseQueryEventWithPolicy()`、`StatusVarPolicy` | [低レベル API](low-level-api.md) |
| `rotate.h`、`xid.h`、`heartbeat.h` | ROTATE、XID、ハートビートの本体とパーサ | [低レベル API](low-level-api.md) |
| `disposition.h` | `Disposition`、`ClassifyEvent()` | [イベントの扱い](disposition.md) |
| `server_settings.h` | `RuleId`、`kServerSettingRules`、`EvaluateServerSettings()` | [サーバー設定](server-settings.md) |
| `server_flavor.h` | `ServerFlavor`、`DetectFlavorFromVersion()`、`ClassifyFlavorProbe()` | [サーバー設定](server-settings.md) |
| `version.h` | `MYBINLOG_VERSION_*`、`Version()` | [はじめての利用](getting-started.md) |

## 層の構成

ヘッダは 3 つのグループに分かれ、それぞれ上のグループにだけ依存します。

1. 形式の基本要素: イベントヘッダ、型コード、チェックサム、CRC32、ステータス。
2. イベント本体と値: イベントの型ごとのパーサ、行デコーダ、値、JSON、圧縮、GTID。これが低レベル API です。
3. グループ 2 を組み合わせてストリームの読み手にするデコーダと、デコードの判断を自分では行わないポリシーの補助（扱い、サーバー設定、種類）。

## ログ用の名前

`StatusName()`、`EventTypeName()`、`ValueKindName()`、`JsonKindName()`、`JsonDiffOperationName()`、`DispositionName()`、`RuleIdName()`、`ServerFlavorName()` は静的な文字列を返し、null を返すことはありません。列挙子の範囲外の値には `"kUnknown"` を返します。
