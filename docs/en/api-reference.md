# API reference

This page maps the public headers. The declarations and their Doxygen comments in `include/mybinlog/` are the symbol-level reference: each function documents its parameters, the statuses it returns, the lifetime of anything it points into, and its thread safety.

## Including and linking

- `#include <mybinlog/mybinlog.h>` includes every public header. Each header can also be included on its own.
- Link the CMake target `mybinlog::mybinlog`, or use `pkg-config mybinlog`. See [Getting started](getting-started.md).
- Everything is in namespace `mybinlog`. Public functions and classes are marked `MYBINLOG_API` (from the generated `mybinlog/export.h`); in a shared build only these are exported. A static build defines `MYBINLOG_STATIC_DEFINE` for its consumers through the CMake target or `mybinlog.pc`.
- `mybinlog/version.h` is generated at configure time and holds the version macros and `Version()`.

## Headers

| Header | Holds | Guide |
|---|---|---|
| `decoder.h` | `Decoder`, `DecoderOptions`, `DecodeError` | [The Decoder](decoder.md) |
| `event.h` | `Event`, `EventBody`, `TableRowsEvent`, `UnknownEvent` | [The Decoder](decoder.md) |
| `status.h` | `Status`, `StatusName()` | [Errors and validation](errors-and-validation.md) |
| `event_header.h` | `EventHeader`, `ParseEventHeader()`, `kEventHeaderSize`, `kBinlogMagic` | [Low-level API](low-level-api.md) |
| `event_types.h` | `EventType`, `ColumnType`, header flags, `EventTypeName()`, `IsRowsEvent()` | [Low-level API](low-level-api.md) |
| `checksum.h` | `ChecksumAlgorithm`, `ChecksumTracker`, `FrameLayout`, `ReadFdeChecksumAlgorithm()` | [Checksums](checksums.md) |
| `crc32.h` | `ComputeCrc32()`, `UpdateCrc32()` | [Checksums](checksums.md) |
| `format_description.h` | `FormatDescriptionEvent`, `ParseFormatDescriptionEvent()` | [Checksums](checksums.md) |
| `table_map.h` | `TableMap`, `TableMapColumn`, `ParseTableMapEvent()`, `ParseTableMapEventWithDiagnostics()`, `TableMapDiagnostics`, `RealType()` | [Rows](rows.md) |
| `row_decoder.h` | `RowsEvent`, `Row`, `DecodeOptions`, `DecodeBudget`, `DecodeRowsEvent()`, `RowScratch`, `RowVisitorFn`, `VisitRowsEvent()`, `RowsEventHeader`, `ParseRowsEventHeader()`, `kRowsStatementEndFlag`, `kDummyTableId` | [Rows](rows.md) |
| `value.h` | `Value`, `Temporal`, `ValueKindName()` | [Values](values.md) |
| `format.h` | `FormatValue()`, `FormatEnumLabel()`, `FormatSetLabels()` | [Values](values.md) |
| `json.h` | `JsonValue`, `ParseJsonBinary()`, `JsonToText()`, `JsonBinaryToText()`, `FindJsonMember()` | [JSON](json.md) |
| `json_diff.h` | `JsonDiff`, `ParseJsonDiffs()`, `ApplyJsonDiffs()`, `ApplyJsonDiffsToBinary()` | [JSON](json.md) |
| `compression.h` | `HasZstdSupport()`, `DecompressMariaDbEvent()`, `ParseTransactionPayloadEvent()`, `DecompressTransactionPayload()`, `NextPayloadEvent()` | [Compressed events](compression.md) |
| `mysql_gtid.h` | `GtidSet`, `GtidEvent`, `PreviousGtidsEvent`, `ParseGtidEvent()`, UUID helpers | [GTIDs](gtids.md) |
| `mariadb_gtid.h` | `MariaDbGtid`, `MariaDbGtidPosition`, `PositionCovers()`, MariaDB GTID, GTID_LIST and ANNOTATE_ROWS events | [GTIDs](gtids.md) |
| `query_event.h` | `QueryEvent`, `ParseQueryEvent()`, `ParseQueryEventWithPolicy()`, `StatusVarPolicy` | [Low-level API](low-level-api.md) |
| `rotate.h`, `xid.h`, `heartbeat.h` | ROTATE, XID and heartbeat bodies and parsers | [Low-level API](low-level-api.md) |
| `disposition.h` | `Disposition`, `ClassifyEvent()` | [Event disposition](disposition.md) |
| `server_settings.h` | `RuleId`, `kServerSettingRules`, `EvaluateServerSettings()` | [Server settings](server-settings.md) |
| `server_flavor.h` | `ServerFlavor`, `DetectFlavorFromVersion()`, `ClassifyFlavorProbe()` | [Server settings](server-settings.md) |
| `version.h` | `MYBINLOG_VERSION_*`, `Version()` | [Getting started](getting-started.md) |

## Layering

The headers fall into three groups, and each depends only on the groups above it:

1. Format primitives: the event header, type codes, checksums, CRC32, statuses.
2. Event bodies and values: one parser per event type, the row decoder, values, JSON, compression, GTIDs. These are the low-level API.
3. The decoder, which composes group 2 into a streaming reader, and the policy helpers (disposition, server settings, flavor), which make no decoding decisions of their own.

## Names for logs

`StatusName()`, `EventTypeName()`, `ValueKindName()`, `JsonKindName()`, `JsonDiffOperationName()`, `DispositionName()`, `RuleIdName()` and `ServerFlavorName()` return static strings and never return null; a value outside the enumerators gives `"kUnknown"`.
