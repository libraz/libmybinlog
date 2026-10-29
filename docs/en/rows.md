# Rows

A rows event carries row images for one table. The decoder decodes them into `RowsEvent` (`mybinlog/row_decoder.h`), and the low-level `DecodeRowsEvent()` does the same for a body the caller framed.

## Row images in an event

| Event | Where the images go |
|---|---|
| WRITE_ROWS (23, 30) | `rows`: one image per inserted row. |
| DELETE_ROWS (25, 32) | `rows`: one image per deleted row. |
| UPDATE_ROWS (24, 31) | `updates`: one `(before, after)` pair per row. |
| PARTIAL_UPDATE_ROWS (39) | `updates`, as UPDATE_ROWS; JSON columns of the after image may hold a partial update. See [JSON](json.md). |

The header's `type_code` says which kind of change an event is; MariaDB's compressed rows events arrive with the plain type code once expanded.

A `Row` is a `std::vector<Value>` with one entry per TABLE_MAP column, in column order, so `row[i]` belongs to `table.columns[i]`. A large statement is split over several rows events; `kRowsStatementEndFlag` in `TableRowsEvent::flags` marks the last one.

For a change in a partitioned MySQL table, `has_partition_id` is set and `partition_id` names the partition the rows are in (after an UPDATE). For UPDATE and PARTIAL_UPDATE, `source_partition_id` is the partition the rows were in before.

## NULL, absent and unselected columns

Three different situations come back without a value, and they must not be confused:

| `Value::kind` | Meaning |
|---|---|
| `kNull` | The row holds SQL NULL in this column. |
| `kAbsent` | The image does not carry this column. Under `binlog_row_image=MINIMAL` or `NOBLOB` the server leaves columns out; the value is unknown, not NULL. |
| `kNull` (unselected) | `DecodeOptions::columns` did not select the column. It reads as `kNull` whether the row held NULL there or the image left it out. |

The row image setting decides what an image carries:

| `binlog_row_image` | Images |
|---|---|
| FULL | Every column in both images. |
| MINIMAL | The before image carries the columns that identify the row (the primary key), the after image only the columns the statement changed. A write image may leave out columns the statement did not set. |
| NOBLOB | Like FULL, except that BLOB, TEXT and JSON columns that are neither part of the key nor changed are left out. |

To replay changes from MINIMAL or NOBLOB images, locate the row by the key columns of the before image, then apply the after image on top of the row you already have: a `kAbsent` column keeps its current value. The before image of a MINIMAL update does not hold the old value of the changed columns, so a reader that needs those values has to keep its own copy of the table state.

## What the TABLE_MAP has to carry

Rows are decoded against the TABLE_MAP before them. Some of what the decoder needs is in the TABLE_MAP's optional metadata, which the server writes according to `binlog_row_metadata`:

| TABLE_MAP field | Written under | Without it |
|---|---|---|
| SIGNEDNESS (`TableMap::has_signedness`) | MINIMAL and FULL | Integers decode as signed, so a BIGINT UNSIGNED above `INT64_MAX` reads as negative. |
| Character sets (`TableMapColumn::has_charset`, `charset`) | MINIMAL and FULL | Character data decodes as `kBytes` instead of `kText`. |
| Column names (`has_column_names`, `name`) | FULL | `TableMapColumn::name` is empty. |
| ENUM and SET labels (`has_enum_labels`, `has_set_labels`, `labels`) | FULL | `FormatEnumLabel()` and `FormatSetLabels()` fail; the ordinal and bitmask still decode. |

MariaDB's `binlog_row_metadata=NO_LOG` writes none of these. The column types and their type metadata are always present, so every value can be decoded and measured whatever the setting.

## Selecting columns and limiting memory

`DecodeOptions` controls what a decode materializes. Pass it as `DecoderOptions::rows`, or directly to `DecodeRowsEvent()`.

| Member | Default | Effect |
|---|---|---|
| `columns` | empty | Columns to decode, by TABLE_MAP ordinal; empty decodes all. A column that is `false` or past the end is measured and skipped: its metadata is still validated, but its value is not materialized or checked. |
| `max_value_bytes` | 0 (none) | Ceiling on one value's payload bytes; above it the event fails with `kLimitExceeded`. |
| `max_decompressed_bytes` | 64 MiB | Ceiling on the expansion of one MariaDB COMPRESSED column value; 0 for none. |
| `budget` | null | A `DecodeBudget` charged for everything the event materializes. |
| `per_value_overhead` | 0 | Bytes charged per `Value` slot of every row image. |
| `per_row_overhead` | 0 | Bytes charged per decoded row, or per UPDATE pair. |

Wire size bounds neither the number of rows nor the expansion of compressed values, and a NULL costs one bit on the wire but a whole `Value` in memory. `DecodeBudget` is the defence against an event that would allocate far more than its size: each row image charges `column count * per_value_overhead` before it is filled, each value charges its payload bytes before they are copied, and each row charges `per_row_overhead` once decoded. When the budget runs out the decode fails with `kLimitExceeded`. `budget.used` reads back what an event cost.

`DecodeBudget::ForEventBody(body_bytes, expansion_ceiling)` builds a budget of at least `expansion_ceiling`, rising with the body size. The decoder uses it for every rows event when `DecoderOptions::rows.budget` is null, with `rows.max_decompressed_bytes` as the floor. To bound memory per event in a way that reflects the caller's allocator, set the overheads:

```cpp
mybinlog::DecoderOptions options;
options.rows.per_value_overhead = sizeof(mybinlog::Value);
options.rows.per_row_overhead = sizeof(mybinlog::Row);
mybinlog::Decoder decoder(options);
```

A budget is mutated by the decode it is passed to, so concurrent decodes must not share one. A budget passed through `DecoderOptions::rows.budget` accumulates across every rows event of that decoder.

## Visiting rows without collecting them

`VisitRowsEvent()` takes the same arguments as `DecodeRowsEvent()` except that a `RowScratch`, a `RowVisitorFn` and a context pointer replace the `RowsEvent`. Every row is decoded into the `RowScratch`; kept across calls, it lets a stream of rows events allocate no `Row` after the first. The visitor receives each row image, and for UPDATE and PARTIAL_UPDATE the after image as a second argument (null otherwise). Both images belong to the scratch, which the decoder overwrites for the next row: the visitor may move payloads out of them (`Value::bytes`) but must not keep pointers into them.

Values, budget charges and statuses are those of `DecodeRowsEvent()`; a row is visited once it and its budget charge have succeeded. A visitor status other than `kOk` stops the decode and is returned unchanged. Rows before a failure have already been visited, so a caller that must publish an event whole buffers what it was given until `VisitRowsEvent()` returns `kOk`. The partition ids of the v2 extra row info are reported only by `DecodeRowsEvent()`.

## Decoding on worker threads

`DecodeRowsEvent()` is reentrant and reads its `TableMap` only during the call. A reader can frame and parse events on one thread and hand rows events, each with a copy of its `TableMap`, to worker threads; see [Low-level API](low-level-api.md).
