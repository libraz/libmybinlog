# GTIDs

MySQL and MariaDB both name transactions with global transaction identifiers, in incompatible formats. The library keeps them as separate types: `mybinlog/mysql_gtid.h` and `mybinlog/mariadb_gtid.h`.

| | MySQL | MariaDB |
|---|---|---|
| One GTID | `uuid:gno` or `uuid:tag:gno` | `domain-server-seq` |
| Event before each transaction | GTID (33) or GTID_TAGGED (42) → `GtidEvent` | GTID (162) → `MariaDbGtidEvent` |
| Event at the start of each file | PREVIOUS_GTIDS (35) → `PreviousGtidsEvent` | GTID_LIST (163) → `MariaDbGtidListEvent` |
| Set of executed transactions | `GtidSet`: intervals per source | `MariaDbGtidPosition`: last GTID per domain |

## MySQL GTIDs

A MySQL GTID is a source server UUID, an optional tag and a transaction number (GNO) with `1 <= gno < 2^63 - 1`. The UUID and tag together form the transaction source, `GtidSet::Tsid`. Tags follow the server's rule `[a-z_][a-z0-9_]{0,31}` and are stored lower-cased; `NormalizeGtidTag()` validates and lower-cases one.

`GtidEvent` holds the GTID of the transaction that follows. `ToString()` prints `uuid:gno`, or `uuid:tag:gno` for a tagged GTID. A transaction run with `gtid_next='AUTOMATIC:<tag>'` produces a GTID_TAGGED event, which arrives as the same `GtidEvent` with `tag` set.

`ParseUuid()` and `FormatUuid()` convert between the 16-byte `Uuid` and its 8-4-4-4-12 hex text.

### GtidSet

`GtidSet` is a set of MySQL GTIDs stored as sorted, merged intervals per TSID, the form the server's binary encoding uses. It is the type for resume positions and checkpoints.

```cpp
mybinlog::GtidSet executed;
if (mybinlog::GtidSet::Parse("3e11fa47-71ca-11e1-9e33-c80aa9429562:1-5:mytag:1-2", &executed) ==
    mybinlog::Status::kOk) {
  std::string text = executed.ToString();
  // "3e11fa47-71ca-11e1-9e33-c80aa9429562:1-5,3e11fa47-71ca-11e1-9e33-c80aa9429562:mytag:1-2"
  std::vector<uint8_t> encoded;
  executed.EncodeBinary(&encoded);  // the GTID set COM_BINLOG_DUMP_GTID carries
}
```

| Member | Does |
|---|---|
| `Parse(text, out)` | Reads set text as the server prints it (`uuid:1-3:7:tag:1-4,uuid2:5`). Only `""` is the empty set. A bare `uuid:5` is the single GTID 5; widening it to `1-5` as a resume convention is left to the caller. |
| `ToString()` | Canonical text: one entry per TSID, ranges always as `a-b`, so a single GTID prints `5-5` and never reads as a bare number. |
| `EncodeBinary(out)` | The binary form for `COM_BINLOG_DUMP_GTID`: format 0 when no TSID has a tag, format 1 otherwise. |
| `DecodeBinary(data, size, out)` | Reads the binary form, as in a PREVIOUS_GTIDS body. |
| `Add(uuid, tag, interval)`, `Merge(other)` | Add one interval `[start, end)` to a TSID, or union another set. |
| `sources()` | Every TSID with its sorted, merged `[start, end)` intervals, for rendering the set in another text form. |
| `IsSubsetOf(other)`, `empty()`, `Clear()` | Set queries and reset. |

`Interval` ends are exclusive: `{1, 6}` is `1-5`. A TSID may hold at most `kMaxIntervalsPerTsid` (65536) intervals; beyond that `Parse`, `Add`, `Merge` and `DecodeBinary` fail with `kLimitExceeded`, which bounds memory on untrusted input.

A `GtidSet` may be read from several threads at once but not mutated concurrently.

## MariaDB GTIDs

A MariaDB GTID is a replication domain, the originating server id and a sequence number; sequence numbers are unsigned 64-bit and 0 is valid. `MariaDbGtid::Parse()` reads `domain-server-seq`, and `IsMariaDbGtidFormat()` tests text without keeping the result.

`MariaDbGtidEvent` holds the GTID of the transaction that follows; its `server_id` comes from the event header. `standalone()` reports FL_STANDALONE: a group that ends without COMMIT or XID, as DDL does. `flags` holds the rest of flags2.

A position is a list of GTIDs with at most one meaningful entry per domain (the highest sequence number). It is not an interval set, so it is a different type, `MariaDbGtidPosition`:

```cpp
mybinlog::MariaDbGtidPosition required, available;
if (mybinlog::MariaDbGtidPosition::Parse("0-1-42,1-2-100", &required) == mybinlog::Status::kOk &&
    mybinlog::MariaDbGtidPosition::Parse("0-3-50,1-2-100", &available) == mybinlog::Status::kOk) {
  bool reached = mybinlog::PositionCovers(required, available);  // true
}
```

`PositionCovers()` compares the highest sequence number per domain and ignores server ids, which change legitimately on failover. A position keeps its entries in text (or wire) order, and `ToString()` prints them back in that order.

`MariaDbGtidListEvent::gtids` is the binlog state at the start of a file, and `MariaDbAnnotateRowsEvent::query` the statement behind the rows events that follow it, written when `binlog_annotate_row_events=ON`.
