# Introduction

libmybinlog is a C++17 library that decodes the MySQL and MariaDB binary log (binlog) wire format. It reads bytes the caller already holds, whether a binlog file or the event stream of a replication connection, and returns typed events: format descriptions, table maps, row images as structured values, GTIDs, queries and transaction boundaries.

The library is built with `-fno-exceptions -fno-rtti`, has no virtual interfaces, and depends on zlib, with zstd as an optional extra.

## What it is not

- **Not a replication client.** There is no network code. Connecting, authenticating, registering as a replica, sending `COM_BINLOG_DUMP` or `COM_BINLOG_DUMP_GTID` and removing the packet framing are the caller's job. The library supplies the pieces such a client needs from the format side: [`GtidSet::EncodeBinary`](gtids.md) produces the GTID set a GTID dump request carries, and the [`Decoder`](decoder.md) reads a dump stream as well as a file.
- **Not a SQL parser.** A QUERY event yields its statement text and session fields; the text is not parsed or classified. A statement-based binlog therefore gives statements, not rows.
- **Not a presentation layer.** Values come back as the wire holds them: TIMESTAMP as epoch seconds, DECIMAL as exact decimal text, ENUM as an ordinal, character data in the column's character set without conversion, JSON as the server's binary format. The functions in `format.h` and `json.h` render the server's own text form when a tool needs it ([Values](values.md)).
- **Not a change-data-capture policy.** [Event disposition](disposition.md) and [server-setting rules](server-settings.md) classify events and settings for a reader that must not lose data. What to do with a classification is left to the caller.

## Two layers

The public API has two layers over the same code.

| Layer | Entry point | Use it when |
|---|---|---|
| High level | `mybinlog::Decoder` | You have bytes (a file, or a stream in any split) and want typed events one at a time. |
| Low level | Per-event free functions: `ParseEventHeader`, `ChecksumTracker`, `ParseTableMapEvent`, `DecodeRowsEvent`, ... | You frame events yourself, need one step on its own, or want control the decoder does not expose. |

The decoder is a pull loop: `Feed()` appends bytes, `Next()` returns the next complete event or `kNeedMoreData`. Inside, it skips the file magic, tracks the checksum algorithm per FORMAT_DESCRIPTION event, detects the server flavor, keeps the table id to TABLE_MAP mapping, expands compressed events and decodes rows events. Each of those steps is a public low-level function, documented in [Low-level API](low-level-api.md).

## The binlog in brief

A binlog is a sequence of events. Every event starts with a 19-byte common header carrying its type code, length and server id. A file opens with a 4-byte magic number and a FORMAT_DESCRIPTION event (FDE), which names the server version and declares whether every later event in the file ends with a CRC32 trailer.

In row-based logging a data change arrives as a TABLE_MAP event, which describes a table and assigns it a numeric table id, followed by one or more rows events that refer to that id. Each rows event holds row images: one per row for INSERT and DELETE, a before/after pair per row for UPDATE. The images are decoded against the column types in the TABLE_MAP, so a rows event cannot be read without the TABLE_MAP before it.

A transaction is framed by a position event (a MySQL GTID event, or a MariaDB GTID event) and ends with an XID event or a COMMIT query. [Glossary](glossary.md) defines the terms used across these pages.

## Conventions

- Every fallible function returns `mybinlog::Status` and writes its result through a trailing out pointer, which it leaves untouched on failure unless its documentation says otherwise. No function throws. See [Errors and validation](errors-and-validation.md).
- Inputs are `(const uint8_t* data, size_t size)` pairs. Event-body parsers take the event header, the body after the header and the body length with any checksum trailer excluded.
- Free functions are reentrant. A `Decoder` or `ChecksumTracker` serves one stream and is used from one thread at a time.
- Everything is in namespace `mybinlog`, and `#include <mybinlog/mybinlog.h>` includes every public header.

Before 1.0, a minor release may change the API.

## Pages

- [Getting started](getting-started.md): install, build options, a first program.
- [The Decoder](decoder.md): feeding bytes, events, errors, pointer lifetimes, replication streams.
- [Rows](rows.md) and [Values](values.md): row images, absent columns, memory limits, the type mapping and text rendering.
- [JSON](json.md), [Compressed events](compression.md), [Checksums](checksums.md), [GTIDs](gtids.md).
- [Event disposition](disposition.md) and [Server settings](server-settings.md): the policy helpers.
- [Low-level API](low-level-api.md), [Errors and validation](errors-and-validation.md), [API reference](api-reference.md).
- [Compatibility](compatibility.md), [Glossary](glossary.md), [FAQ](faq.md).
