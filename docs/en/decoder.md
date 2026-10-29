# The Decoder

`mybinlog::Decoder` (`mybinlog/decoder.h`) turns a byte stream into typed events. It is a pull decoder: the caller pushes bytes with `Feed()` and pulls events with `Next()`, so the caller's own loop keeps control of reading, buffering and stopping.

## Feeding bytes and taking events

```cpp
mybinlog::Decoder decoder;
decoder.Feed(data, size);  // a whole file, or any split of a stream
mybinlog::Event event;
mybinlog::Status status;
while ((status = decoder.Next(&event)) == mybinlog::Status::kOk) {
  // use event
}
// status is kNeedMoreData, or an error
```

`Feed()` copies the bytes, so the caller's buffer can be reused as soon as it returns. The split does not matter: one call with a whole file and one call per byte produce the same events.

`Next()` returns one of three things:

| Result | Meaning |
|---|---|
| `kOk` | `*out` holds the next event. |
| `kNeedMoreData` | The buffered bytes hold no complete event. Feed more, or call `Finish()` if the input has ended. This is not an error, and `*out` is untouched. |
| any other status | The stream failed to decode. `last_error()` describes where and why. |

`buffered_bytes()` reports how many fed bytes `Next()` has not consumed yet.

## End of input

When no more bytes will come, call `Finish()` once `Next()` has returned `kNeedMoreData`. It returns `kOk` if the stream ended on an event boundary and `kTruncated` if the buffer still holds part of an event (or part of the 4-byte file magic). A file that a server is still writing can end partway through an event, so a reader that tails a live file keeps feeding instead of calling `Finish()`.

## Errors are sticky

After `Next()` returns an error, every later call to `Next()` or `Finish()` returns the same status until `Reset()`. A stream that failed to frame cannot be resynchronized safely, because the next event boundary is only known from the length field of the event that failed.

`last_error()` returns a `DecodeError`:

| Member | Content |
|---|---|
| `status` | The failing status; `kOk` when nothing has failed since construction or `Reset()`. |
| `offset` | Stream offset of the event that failed. For an event inside a TRANSACTION_PAYLOAD, the payload event's offset. |
| `message` | Event type, offset and reason, for logs, such as `TABLE_MAP_EVENT at offset 1180: body does not parse (kTruncated)`. |

`Reset()` drops all buffered bytes and state and returns the decoder to its newly constructed condition with the same options. [Errors and validation](errors-and-validation.md) lists what each status means.

## What an Event holds

| Member | Content |
|---|---|
| `header` | The 19-byte common header. For an expanded MariaDB compressed event, the header of the equivalent plain event. |
| `body` | A `std::variant` of the parsed body. Read it with `std::get_if<T>(&event.body)`. |
| `disposition` | `ClassifyEvent()` of the header under the flavor the stream's FDE announced. See [Event disposition](disposition.md). |
| `from_payload` | True for an event that came out of a TRANSACTION_PAYLOAD. |
| `offset` | Stream offset of the event's first byte, which for a file is its binlog position. For an event from a TRANSACTION_PAYLOAD, the offset of the payload event. |

The body alternative follows the header's type code:

| Type code | Alternative |
|---|---|
| FORMAT_DESCRIPTION (15) | `FormatDescriptionEvent` |
| ROTATE (4) | `RotateEvent` |
| TABLE_MAP (19) | `TableMap` |
| WRITE/UPDATE/DELETE_ROWS v1 (23-25) and v2 (30-32), PARTIAL_UPDATE_ROWS (39); MariaDB 166-171 after expansion | `TableRowsEvent` |
| QUERY (2); MariaDB 165 after expansion | `QueryEvent` |
| XID (16) | `XidEvent` |
| GTID (33), GTID_TAGGED (42) | `GtidEvent` |
| PREVIOUS_GTIDS (35) | `PreviousGtidsEvent` |
| MariaDB GTID (162) | `MariaDbGtidEvent` |
| MariaDB GTID_LIST (163) | `MariaDbGtidListEvent` |
| MariaDB ANNOTATE_ROWS (160) | `MariaDbAnnotateRowsEvent` |
| HEARTBEAT (27), HEARTBEAT_V2 (41) | `HeartbeatEvent` |
| anything else | `UnknownEvent` |

Every event type is returned, including statement-execution context (INTVAR, RAND, USER_VAR), ROWS_QUERY and types the library has no parser for; those arrive as `UnknownEvent` with their raw body, so no event is skipped silently. The decoder does not stop at an event whose disposition is `kFailClosed` either. It returns the event, and the caller decides.

A TRANSACTION_PAYLOAD itself is not returned when expansion is on: the events it carries come one per `Next()` call, in order, each with `from_payload` set.

### Rows events

`TableRowsEvent` holds the rows event and the table it refers to:

| Member | Content |
|---|---|
| `table_id`, `flags` | From the post-header. `flags & kRowsStatementEndFlag` marks the last rows event of a statement. |
| `table` | The `TableMap` the table id refers to, held by the decoder. |
| `decoded` | Whether `rows` holds the decoded images. False when `DecoderOptions::decode_rows` is off or the event has no table. |
| `rows` | The row images; see [Rows](rows.md). |
| `body`, `size` | The raw body after the header, trailer excluded. |

A rows event whose table id has no TABLE_MAP before it fails with `kInvalidMetadata`. The one exception is `kDummyTableId` (all 48 bits set), which MySQL writes on a rows event that names no table; it arrives with `table == nullptr` and `decoded == false`.

The decoder drops its table map at every FORMAT_DESCRIPTION and ROTATE event, and a TABLE_MAP replaces any earlier one with the same id. Servers write a TABLE_MAP before the rows events of every statement, so the map never needs to outlive a file.

## Lifetime of pointers in an Event

Three members point into memory the decoder owns: `UnknownEvent::body`, `TableRowsEvent::table` and `TableRowsEvent::body`. They stay valid until the decoder's next `Next()`, `Feed()` or `Reset()`. Everything else in an `Event` (strings, vectors, the decoded `Value`s) is owned by the event and can be moved or kept.

To keep a table description beyond that, copy the `TableMap`:

```cpp
if (const auto* rows = std::get_if<mybinlog::TableRowsEvent>(&event.body); rows && rows->table) {
  mybinlog::TableMap kept = *rows->table;  // owned copy
}
```

## Options

`DecoderOptions` is passed to the constructor and copied. Every default suits a binlog file from a trusted server.

| Option | Default | Effect |
|---|---|---|
| `default_checksum` | `kCrc32` | Checksum algorithm assumed before the first FDE. Only matters for a replication stream, whose artificial ROTATE comes before any FDE; set it to the algorithm the replica announced to the source. |
| `verify_checksum` | `true` | Compare CRC32 trailers. `false` still frames events by the algorithm but skips the comparison. |
| `max_event_size` | 1 GiB | Ceiling on an event's `event_length`; a longer event fails with `kLimitExceeded`. |
| `decode_rows` | `true` | Decode rows events into `TableRowsEvent::rows`. `false` leaves only the raw body. |
| `rows` | `DecodeOptions()` | Column selection and memory limits for rows decoding; see [Rows](rows.md). |
| `expand_compressed` | `true` | Expand MariaDB compressed events and TRANSACTION_PAYLOAD events. When `false`, both arrive as `UnknownEvent` with disposition `kCompressed`. |
| `max_decompressed_size` | 1 GiB | Ceiling on one expanded MariaDB event body or transaction payload. |

When `rows.budget` is null, each rows event is charged against its own budget sized from the event body. A non-null budget is used as given and accumulates across events; it must outlive the decoder.

## Reading a replication stream

The decoder reads a stream without the 4-byte magic, which is what a replication dump sends once the client strips each packet's header and leading status byte. Feed each event's bytes as they arrive. Three things differ from a file:

- The stream opens with an artificial ROTATE that precedes any FDE. Set `default_checksum` to the algorithm the replica announced (by setting `@source_binlog_checksum` or `@master_binlog_checksum` on the connection).
- HEARTBEAT and HEARTBEAT_V2 events appear on an idle stream. They arrive as `HeartbeatEvent` with disposition `kDataNeutral`.
- `offset` counts bytes fed to this decoder, not positions on the source. Use the header's `next_position`, or a ROTATE's `position`, for source coordinates.

The decoder tells a file from a stream by its first four bytes. A stream without the magic whose first event has the timestamp 1852400382 (2028-09-11) reads the same as the magic and is misread as a file.

## Thread safety

One decoder serves one stream and is used from one thread at a time. Separate decoders are independent and can run on separate threads.
