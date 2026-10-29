# Low-level API

The `Decoder` is built from public per-event functions. Use them directly when the events are already framed (a replication client that receives one event per packet), when one step is needed on its own (parsing a GTID set read from a checkpoint), or when the decoder's choices do not fit, such as decoding rows on worker threads.

## Conventions

Every event-body parser has the same shape:

```cpp
Status ParseXxx(const EventHeader& header, const uint8_t* body, size_t size, Xxx* out);
```

- `header` is the parsed common header. Some parsers read it (the type code selects a layout, or a field comes from it); the others take it for a uniform signature.
- `body` points at the bytes after the 19-byte header, and `size` is the body length with any checksum trailer excluded, as `FrameLayout::body_length()` reports it.
- `out` is filled on `kOk` and untouched otherwise. A null `body` fails with `kTruncated`.
- The functions are reentrant and keep no state.

## Functions by event type

| Event | Function | Result |
|---|---|---|
| Common header | `ParseEventHeader(data, size, out)` | `EventHeader` |
| Framing and checksum | `ChecksumTracker::OnEvent()` | `FrameLayout`; see [Checksums](checksums.md) |
| FORMAT_DESCRIPTION (15) | `ParseFormatDescriptionEvent()` | `FormatDescriptionEvent` |
| ROTATE (4) | `ParseRotateEvent()` | `RotateEvent` |
| TABLE_MAP (19) | `ParseTableMapEvent(header, body, size, flavor, out)` | `TableMap` |
| Rows events (23-25, 30-32, 39) | `DecodeRowsEvent(header, body, size, table, options, out)` | `RowsEvent`; see [Rows](rows.md) |
| QUERY (2) | `ParseQueryEvent()` | `QueryEvent` |
| XID (16) | `ParseXidEvent()` | `XidEvent` |
| GTID (33), GTID_TAGGED (42) | `ParseGtidEvent()` | `GtidEvent` |
| PREVIOUS_GTIDS (35) | `ParsePreviousGtidsEvent()` | `PreviousGtidsEvent` |
| MariaDB GTID (162) | `ParseMariaDbGtidEvent()` | `MariaDbGtidEvent` |
| MariaDB GTID_LIST (163) | `ParseMariaDbGtidListEvent()` | `MariaDbGtidListEvent` |
| MariaDB ANNOTATE_ROWS (160) | `ParseMariaDbAnnotateRowsEvent()` | `MariaDbAnnotateRowsEvent` |
| HEARTBEAT (27), HEARTBEAT_V2 (41) | `ParseHeartbeatEvent()` | `HeartbeatEvent` |
| MariaDB compressed (165-171) | `DecompressMariaDbEvent()` | `DecompressedEvent`; see [Compressed events](compression.md) |
| TRANSACTION_PAYLOAD (40) | `ParseTransactionPayloadEvent()`, `DecompressTransactionPayload()`, `NextPayloadEvent()` | the inner events |
| Any | `ClassifyEvent()` | `EventDisposition`; see [Event disposition](disposition.md) |

Helpers: `EventTypeName()` gives the server's name for a type code, `IsRowsEvent()` is true for the types `DecodeRowsEvent()` accepts as they are, and `RealType()` and `HasSignednessBit()` interpret a TABLE_MAP column.

`ParseTableMapEvent()` takes the flavor because the two servers index character-set metadata over different column subsets (MariaDB counts GEOMETRY as a character column). `QueryEvent` reports the status variables up to the first code the parser does not know, as the servers do; `status_vars_complete` is false when it stopped early.

## Walking a file by hand

The function below walks one whole binlog file held in memory and decodes its rows events. It is roughly what the decoder does for a file, minus compression, without the streaming buffer.

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

MariaDB writes v1 rows events (23-25) and MySQL v2 (30-32); `IsRowsEvent()` covers both, and `DecodeRowsEvent()` picks the layout from the header. `ParseRowsEventHeader()` reads the 48-bit table id and the flags from the post-header, so the caller can pick the TABLE_MAP before decoding.

## Decoding rows on worker threads

`DecodeRowsEvent()` reads its `TableMap` only during the call and keeps no state, so rows events can be decoded in parallel. Frame and parse events in order on one thread, keep the table map there, and hand each rows event to a worker together with a copy of the event bytes and of its `TableMap`. A `DecodeBudget`, if used, must not be shared between concurrent calls.
