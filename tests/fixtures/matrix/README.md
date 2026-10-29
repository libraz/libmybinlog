# Version x configuration fixture matrix

Real binlogs from every supported server version under several binlog
configurations, each paired with the server's own view of the table contents
after every transaction. A replay test decodes the rows events, applies them by
primary key, and compares the result with `expected.jsonl`. No expected value
comes from a decoder.

## Layout

```
<flavor>-<version>/<config>/
  binlog.NNNNNN.gz  every binlog file holding the workload, gzip-compressed (byte for byte once inflated)
  expected.jsonl.gz server-side table snapshots after each data transaction, gzip-compressed
  meta.json         server version, settings, schema with per-column normalization, transactions
```

The configurations come from `tests/models/capture_matrix_mysql.json` and
`tests/models/capture_matrix_mariadb.json`: a 2-wise covering array over
checksum, row image, row metadata, compression, PARTIAL_JSON (MySQL only) and
version, plus one row per version with CRC32, FULL image, FULL metadata and no
compression, which is the configuration the products require. Each model's
`configs` array names the output directory of every row.

| Flavor | Versions | Configs |
|---|---|---|
| MySQL | 8.4, 9.7 | 11 (8.4: 6, 9.7: 5) |
| MariaDB | 10.11, 11.4, 11.8, 12.3, 13.0 | 18 (10.11: 4, 11.4: 3, 11.8: 4, 12.3: 4, 13.0: 3) |

## Capturing

```sh
./capture_matrix.sh                          # every config of both flavors
./capture_matrix.sh --flavor mariadb --version 11.8
./capture_matrix.sh --config crc32_full_metafull_nozstd_nopjson --force
./capture_matrix.sh --list                   # print the selection only
```

Needs Docker with the `mysql:<version>` and `mariadb:<version>` images, and
`jq` or `python3`. One container runs at a time and is removed afterwards. A
config whose directory already has `meta.json` is skipped unless `--force` is
given; output is written to a temporary directory and moved into place only
after both JSON files validate. One config takes about 10-40 seconds, mostly
server start-up, so the whole matrix takes 5-20 minutes. Each config is
roughly 0.8-1.5 MB.

Every server runs with `--server-id=1 --log-bin=binlog --binlog-format=ROW`
and the config's settings (MySQL also `--gtid-mode=ON
--enforce-gtid-consistency=ON`; MariaDB also `--binlog-annotate-row-events=ON`,
and `--log-bin-compress-min-len=10` when `log_bin_compress=ON`). The script
reads the settings back from the server and aborts on any mismatch; the
server's values are stored in `meta.json` under `server_settings`. Every
session runs `SET NAMES utf8mb4`, `sql_mode=''` and `time_zone='+00:00'`, and
the client uses `--default-character-set=utf8mb4`. After the workload the script
checks the bytes of known latin1, utf8mb3 and 4-byte utf8mb4 values in
`t_str` and aborts if a client fell back to latin1 and double-encoded them.

The database `mx` is created before the first `FLUSH BINARY LOGS`, so the
captured files start with the `CREATE TABLE` statements. Image tags float, so a
recapture may change server versions, UUIDs and timestamps.

## Workload files

`workload_mysql.sql` and `workload_mariadb.sql` are split into blocks by marker
lines. A block runs as one client session.

| Marker | Meaning |
|---|---|
| `-- @ddl <name>` | starts a block run as-is (no transaction wrapper, no snapshot) |
| `-- @txn <name>` | starts a data block, run inside `START TRANSACTION` / `COMMIT`, followed by a snapshot |
| `-- @min-version X` | run the block only when the config's server version is >= X |
| `-- @max-version X` | run the block only when the version is <= X |
| `-- @gtid-next V` | (`@txn` only) `SET gtid_next='V'` before the transaction and `'AUTOMATIC'` after it |

Names are `[a-z0-9_]+` and unique. Directives must directly follow their
header, before any SQL. Other `-- ` comment lines are passed through, an
unknown `-- @` directive is an error, and no SQL may appear before the first
header. Each `@txn` block is exactly one binlog transaction and must change at
least one row. Skipped blocks are listed in `meta.json` under `skipped`.

Table rules, enforced by the script where it can:

- Every table has a primary key. Replay identifies rows by it, so a table
  without one is rejected.
- Every non-key column is nullable with default NULL, so a column absent from
  a MINIMAL write image replays as NULL.
- No `ALTER`/`DROP` after data is written; the schema in `meta.json` is the
  final one and applies to every transaction.

## expected.jsonl

One line per (transaction, table) whose contents changed in that transaction:

```json
{"txn": "upd_null_to_value", "seq": 16, "table": "t_time", "rows": [{"id": 1, "d": "2020-01-01", ...}, ...]}
```

`seq` numbers the executed blocks (`@ddl` and `@txn`) from 1 in workload
order and matches `transactions[].seq` in `meta.json`. `rows` is the whole
table ordered by primary key, so a table's last line is its state until its
next line. A table not listed for a transaction did not change; a replay can
compare every table after every transaction by carrying the last line forward.
An emptied table appears with `"rows": []`.

`meta.json` `transactions[]` also holds `kind`, `gtid_next` when set, the
tables emitted, and `gtid_after`: `@@global.gtid_executed` (MySQL) or
`@@global.gtid_binlog_pos` (MariaDB) after the block.

## Per-column normalization

Each row is built by the server with `JSON_OBJECT` over one expression per
column, chosen from `information_schema.COLUMNS.DATA_TYPE`. The tag is stored
per column in `meta.json` (`tables[].columns[].norm`, with `data_type`,
`column_type`, `charset`, `compressed` and `pk_seq`). SQL NULL is JSON `null`
for every tag.

| `norm` | Types | Expression | JSON value |
|---|---|---|---|
| `int` | TINYINT .. BIGINT, signed and unsigned | `col` | number; BIGINT UNSIGNED needs a uint64 parse |
| `decimal_text` | DECIMAL | `CAST(col AS CHAR)` | string with exactly `scale` fraction digits; negative zero is stored as zero |
| `float` | FLOAT, DOUBLE | `col` | number. MySQL prints FLOAT after widening to double, so compare as `float(value)`, and keeps `-0.0`. MariaDB prints FLOAT with 6 significant digits and negative zero as `0` |
| `bit_uint` | BIT(n) | `CAST(col AS UNSIGNED)` | number, uint64 |
| `ordinal` | ENUM, SET | `CAST(col AS UNSIGNED)` | ENUM 1-based index (0 for the invalid `''`), SET bitmask as uint64 |
| `year_int` | YEAR | `col+0` | number (0 for 0000) |
| `temporal_text` | DATE, DATETIME(fsp), TIME(fsp) | `CAST(col AS CHAR)` | string with exactly `fsp` fraction digits; zero dates and zero month/day kept |
| `unix_ts_text` | TIMESTAMP(fsp) | `CAST(UNIX_TIMESTAMP(col) AS CHAR)` | seconds since the epoch in UTC with `fsp` fraction digits; the zero timestamp is `"0"` |
| `text` | CHAR, VARCHAR, *TEXT (any charset); MariaDB JSON | `CAST(col AS CHAR)` | UTF-8 string; decode the column's `charset` to UTF-8 before comparing; CHAR drops trailing spaces |
| `hex` | BINARY, VARBINARY, *BLOB, GEOMETRY family, VECTOR | `HEX(col)` | uppercase hex of the stored bytes; BINARY(n) is zero-padded to n; GEOMETRY is the 4-byte SRID plus WKB |
| `json_text` | MySQL JSON | `CAST(col AS CHAR)` | string, the server's canonical JSON text |
| `canonical_text` | MariaDB INET4, INET6, UUID | `CAST(col AS CHAR)` | the type's canonical text form |

MariaDB `COMPRESSED` columns are normalized like their base type; the value is
the decompressed one. MariaDB `JSON` is a `LONGTEXT` alias and has `text`.
`CAST(col AS CHAR)` is used for text rather than `col` because MariaDB's
`JSON_OBJECT` would otherwise embed a JSON-alias column as nested JSON.

## Replay notes

- **Row image.** FULL: every column is present in both images. MINIMAL: the
  before image carries the primary key, the after image only the changed
  columns, and a write image may omit columns the statement did not set (they
  replay as NULL, see the table rules). NOBLOB: like FULL, but BLOB/TEXT/JSON
  columns that are neither key nor changed are absent. An absent column in an
  update keeps the replayed value; locate the row by the primary key from the
  before image and apply the after image, which may change the key
  (`upd_int_pk_change`).
- **Compression.** MySQL `binlog_transaction_compression=ON` wraps each
  transaction in a TRANSACTION_PAYLOAD (zstd); MariaDB `log_bin_compress=ON`
  emits QUERY and rows events as types 165-171 when longer than 10 bytes. The
  decompressed contents are the same events, so replay and expectations do
  not change.
- **PARTIAL_JSON** (MySQL). The `upd_json_*` and `upd_big_json_partial`
  transactions update a JSON column with `JSON_SET`/`JSON_REPLACE`/`JSON_REMOVE`
  on itself. Whether the server logs a PARTIAL_UPDATE_ROWS event with a JSON
  diff depends on the row image and on the update; the diff applies to the
  replayed value of that column, which under MINIMAL is not in the before
  image.
- **Rows-event splitting.** `bulk_insert`, `bulk_update` and `bulk_delete`
  write 2000 / about 1000 / 1300 small rows in one transaction each, which the
  server splits over several rows events at `binlog_row_event_max_size`.
- **GTIDs.** MySQL `tagged_gtid_1`/`tagged_gtid_2` run with
  `gtid_next='AUTOMATIC:mytag'` (GTID_TAGGED events); `after_tagged_gtid`
  returns to untagged.

## Workload coverage

- Integers of every width, signed and unsigned, at min, max, 0 and -1/1.
- DECIMAL (65,30), (10,0), (5,5), (10,2), (18,9), (4,2) at extremes, negative
  zero and tiny values; FLOAT/DOUBLE at 0, -0, min normal, denormal and max;
  BIT(1), BIT(10), BIT(64).
- DATE, DATETIME(0-6), TIMESTAMP(0-6), TIME(0-6), YEAR: zero dates, zero
  month/day, extremes, TIME at ±838:59:59, negative TIME with fractions,
  TIMESTAMP 1970-01-01 00:00:01 and 2038-01-19 03:14:07.999999, YEAR 0000, 1901,
  2155. MariaDB 11.5+ adds TIMESTAMP 2106-02-07 06:28:15.
- CHAR/VARCHAR in latin1, ascii, utf8mb3 and utf8mb4 (4-byte characters),
  CHAR(255) utf8mb4 and CHAR(100) utf8mb3 (byte length above 255), VARCHAR(300)
  (2-byte length), BINARY/VARBINARY, trailing spaces, escapes; all TEXT and
  BLOB widths with empty, at-limit and NULL values, and a 65,600-byte
  MEDIUMTEXT in `t_big`.
- ENUM with 3 and 300 members (2-byte index), SET with 3, 12 and 64 members.
- JSON: scalars, nested documents, unicode escapes, every number kind, empty
  object/array, a document above 64 KB (large storage format); MySQL also
  opaque DATE/TIME/DATETIME/DECIMAL/binary values.
- GEOMETRY, POINT and POLYGON with SRID 4326; VECTOR on MySQL 9.x and
  MariaDB 11.7+.
- MariaDB COMPRESSED BLOB/TEXT/VARCHAR (above and below
  `column_compression_threshold`), INET6, UUID (10.7+), INET4 (10.10+).
- Statement shapes: multi-row INSERT, UPDATE of some columns, of only
  BLOB/TEXT columns and of only non-BLOB columns, NULL to value and back,
  primary-key change, DELETE, a multi-table transaction, and the bulk
  transactions above.
