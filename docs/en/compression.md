# Compressed events

Both servers can compress the binlog, in different ways. The decoder expands both by default, and the events inside come out as if the server had written them uncompressed.

| Server setting | What the server writes | Algorithm |
|---|---|---|
| MariaDB `log_bin_compress=ON` | QUERY and rows events above `log_bin_compress_min_len` as types 165-171 | zlib |
| MySQL `binlog_transaction_compression=ON` | Each transaction wrapped in one TRANSACTION_PAYLOAD event (type 40) | zstd |
| MariaDB column attribute `COMPRESSED` | Individual VARCHAR, TEXT and BLOB values | zlib |

## MariaDB compressed events

A MariaDB compressed event keeps its leading fields in the clear and compresses only the tail: the statement text of QUERY_COMPRESSED (165), or the row images of the compressed rows events (166-171). The decoder rebuilds the plain event, so it arrives as a `QueryEvent` or `TableRowsEvent` whose `header.type_code` is the plain type (QUERY, or WRITE/UPDATE/DELETE_ROWS v1 or v2) and whose disposition is that of the plain type.

## MySQL transaction payloads

A TRANSACTION_PAYLOAD event carries a whole transaction's events back to back, compressed with zstd or, when its compression type is NONE, as written. The decoder expands the payload and returns its events one per `Next()` call, in order, each with `Event::from_payload` set and `Event::offset` set to the payload event's offset. The payload event itself is not returned.

The inner events never carry a checksum trailer, whatever the file's algorithm: the server writes them without one.

## zstd is a build option

zstd support is controlled by the CMake option `MYBINLOG_WITH_ZSTD`, which is on when libmybinlog is built as the top-level project and off when another project includes it. Without zstd a compressed transaction payload fails with `kUnsupported`, and the decoder stops there because errors are sticky.

Check at run time with `HasZstdSupport()`, for example before connecting to a server that has `binlog_transaction_compression=ON`:

```cpp
if (!mybinlog::HasZstdSupport()) {
  // this build cannot expand MySQL compressed transactions
}
```

MariaDB compression needs only zlib, which is always linked.

## Turning expansion off

With `DecoderOptions::expand_compressed = false`, MariaDB compressed events and TRANSACTION_PAYLOAD events arrive as `UnknownEvent` with their raw body and disposition `kCompressed`. A reader that forwards events without looking inside them can use this to skip the decompression cost.

## Compressed columns

A MariaDB column declared `COMPRESSED` holds a compressed value inside an otherwise ordinary rows event. Its TABLE_MAP type is `kVarcharCompressed` or `kBlobCompressed`, and the row decoder returns the decompressed bytes as `kText` or `kBytes`. Expansion of one value is capped by `DecodeOptions::max_decompressed_bytes` (64 MiB by default) and charged to the rows budget; see [Rows](rows.md).

## Limits

| Limit | Default | Applies to |
|---|---|---|
| `DecoderOptions::max_decompressed_size` | 1 GiB | One expanded MariaDB event body, or one expanded transaction payload. |
| `DecodeOptions::max_decompressed_bytes` | 64 MiB | One MariaDB COMPRESSED column value. |

A larger expansion fails with `kLimitExceeded`. When the server recorded a payload's uncompressed size, the expansion must match it exactly, or the event fails with `kMalformedValue`.

## Low-level functions

| Function | Does |
|---|---|
| `IsMariaDbCompressedEvent(type_code)` | True for 165-171. |
| `DecompressMariaDbEvent()` | Rebuilds the plain event: a `DecompressedEvent` with the plain header and body. |
| `ParseTransactionPayloadEvent()` | Reads the payload header fields: compression type, payload size, uncompressed size, and a pointer to the payload. |
| `DecompressTransactionPayload()` | Expands the payload into the inner events, concatenated. |
| `NextPayloadEvent()` | Reads one inner event (`PayloadEventView`) and advances an offset; loop while the offset is below the size. |
