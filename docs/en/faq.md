# FAQ

## Can it connect to a server and stream the binlog?

No. The library has no network code. A replication client connects, registers as a replica, sends `COM_BINLOG_DUMP` or `COM_BINLOG_DUMP_GTID`, strips each packet's header and status byte, and feeds the event bytes to a `Decoder`. `GtidSet::EncodeBinary()` produces the GTID set for the dump request. See [The Decoder](decoder.md).

## Why does a column come back as kAbsent?

The server left it out of the row image. Under `binlog_row_image=MINIMAL` or `NOBLOB` an image carries only some columns; the missing ones are unknown, not NULL. See [Rows](rows.md).

## Why are my unsigned integers negative?

The TABLE_MAP carries no SIGNEDNESS metadata, so the decoder cannot tell unsigned columns apart and decodes every integer as signed. Set `binlog_row_metadata` to MINIMAL or FULL. `TableMap::has_signedness` says whether the field was present.

## Why is a VARCHAR column kBytes instead of kText?

The TABLE_MAP carries no character set for the column (row metadata is off), or the column uses the binary collation. With `binlog_row_metadata` MINIMAL or FULL, character columns decode as `kText`.

## Why are column names empty?

Column names are written only under `binlog_row_metadata=FULL`. The same holds for ENUM and SET labels.

## Is character data converted to UTF-8?

No. `Value::bytes` holds the bytes in the column's character set, and `TableMapColumn::charset` gives the collation id for a caller that converts.

## How do I get the TIMESTAMP in local time?

`Value::epoch_seconds` is UTC seconds since the epoch; apply the time zone you need. `FormatValue()` prints the UTC date and time.

## Why does Next() keep returning the same error?

Errors are sticky: after a failure the stream position is unknown, so the decoder refuses to continue until `Reset()`. See [The Decoder](decoder.md).

## Why does a MySQL binlog fail with kUnsupported on a TRANSACTION_PAYLOAD?

The server has `binlog_transaction_compression=ON` and this build has no zstd. Build with `MYBINLOG_WITH_ZSTD=ON` and zstd installed; `HasZstdSupport()` reports which you have. See [Compressed events](compression.md).

## Does it decode statement-based binlogs?

It returns the QUERY events and their statement text, plus INTVAR, RAND and USER_VAR as raw `UnknownEvent`s. It does not execute or interpret statements, so it yields no rows for them.

## Are encrypted binlog files supported?

No. A MariaDB or MySQL server decrypts binlog events before sending them to a replica, so a replication stream is readable, but encrypted files on disk are not.

## What is not decoded?

- Pre-5.0 DECIMAL columns (`kUnsupported`); no current server writes them.
- MariaDB PARTIAL_ROW_DATA events, which arrive as `UnknownEvent` with disposition `kFailClosed`.
- The NDB extra row info in v2 rows events, which is skipped.
- MariaDB INET4, INET6 and UUID values have no text rendering; they come back as their stored bytes.

## Which server versions are supported?

See [Compatibility](compatibility.md), which lists the tested versions and configurations.
