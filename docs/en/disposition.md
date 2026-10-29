# Event disposition

A reader that must not lose data has to decide, for every event type, whether to decode it, skip it, or stop. `ClassifyEvent()` (`mybinlog/disposition.h`) makes that decision in one place, and the decoder stores its result in `Event::disposition`.

## Dispositions

| `Disposition` | Meaning |
|---|---|
| `kFraming` | Stream framing, consumed by the reader: FORMAT_DESCRIPTION, ROTATE, STOP. |
| `kTableMap` | TABLE_MAP; decoded into a table description. |
| `kRows` | An uncompressed rows event, PARTIAL_UPDATE_ROWS included; decoded and applied. |
| `kGtid` | A position event: MySQL GTID or GTID_TAGGED, MariaDB GTID or GTID_LIST. |
| `kTransactionBoundary` | XID; commits the open transaction. |
| `kQuery` | QUERY: BEGIN, COMMIT, DDL or statement text. |
| `kCompressed` | A MariaDB compressed event or a TRANSACTION_PAYLOAD; see [Compressed events](compression.md). |
| `kDataNeutral` | Informational. Skipping it cannot lose a row or a position. |
| `kFailClosed` | Cannot be applied and cannot be proven harmless to skip: stop. |

`EventDisposition::reason` is a static string explaining the choice, for logs; `DispositionName()` returns the enumerator name.

The decoder only records the disposition. It returns every event, `kFailClosed` ones included, and leaves stopping to the caller.

## How types are classified

| Disposition | Event types |
|---|---|
| `kDataNeutral` | HEARTBEAT and HEARTBEAT_V2; INTVAR, RAND and USER_VAR (context of the statement that follows, judged at its QUERY); BEGIN_LOAD_QUERY, APPEND_BLOCK and DELETE_FILE; IGNORABLE; ROWS_QUERY; PREVIOUS_GTIDS; TRANSACTION_CONTEXT and VIEW_CHANGE (Group Replication); on MariaDB, ANNOTATE_ROWS, BINLOG_CHECKPOINT and START_ENCRYPTION |
| `kFailClosed` | ANONYMOUS_GTID (no position to record); XA_PREPARE (prepared rows that may never commit); INCIDENT (the source declared lost events); EXECUTE_LOAD_QUERY (statement-based LOAD DATA, whose rows are never materialized); START_EVENT_V3 and other types no supported server writes; MariaDB PARTIAL_ROW_DATA |

Every other known type takes the disposition its row in the first table names.

The flavor matters. A MariaDB-numbered type (160 and up) on a MySQL source fails closed, and so do MySQL GTID types (33, 42) on a MariaDB source, because that server never writes them. The decoder classifies under the flavor the latest FDE announced.

A code outside `EventType` is classified from its header flags: `kDataNeutral` when the source set `LOG_EVENT_IGNORABLE_F` (`kLogEventIgnorableFlag`), `kFailClosed` otherwise. That flag is how a server marks a new event type that an older reader may skip.

These dispositions encode the policy of a row-based change reader: ANONYMOUS_GTID fails closed because such a reader records GTID positions, for instance. A tool with other needs, such as a binlog viewer, can ignore the disposition and look at `header.type_code` instead.

## Using it in a reader

```cpp
switch (event.disposition.disposition) {
  case mybinlog::Disposition::kFailClosed:
    // stop, and report event.disposition.reason with event.offset
    break;
  case mybinlog::Disposition::kDataNeutral:
  case mybinlog::Disposition::kFraming:
    break;  // nothing to apply
  default:
    // handle the parsed body
    break;
}
```

`ClassifyEvent()` has two overloads: one for a known `EventType`, and one for a raw `type_code` plus header `flags`, which also handles codes outside the enumeration. Both are reentrant.
