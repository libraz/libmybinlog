# Compatibility

The library targets MySQL 8.4 and 9.x and MariaDB 10.11 and later. The tables below list the server versions and binlog configurations it is tested against: for each one, the repository holds binlog files captured from a real server running that configuration (`tests/fixtures/matrix`).

The tables are generated from the `meta.json` file of each capture. Do not edit them by hand; regenerate them with:

```sh
python3 tools/gen_compat_table.py
```

`python3 tools/gen_compat_table.py --check` writes nothing and exits non-zero when a page is out of date.

## Server versions

Each row is one server version and the values its captures cover. "Configurations" counts the captures.

<!-- compat:summary:begin -->
| Flavor | Version | Server version | Configurations | Checksum | Row image | Row metadata | Compression | PARTIAL_JSON |
|---|---|---|---|---|---|---|---|---|
| MySQL | 8.4 | `8.4.13` | 6 | NONE, CRC32 | FULL, MINIMAL, NOBLOB | MINIMAL, FULL | OFF, zstd | OFF, ON |
| MySQL | 9.7 | `9.7.4` | 5 | NONE, CRC32 | FULL, MINIMAL, NOBLOB | MINIMAL, FULL | OFF, zstd | OFF, ON |
| MariaDB | 10.11 | `10.11.19-MariaDB-ubu2204-log` | 4 | NONE, CRC32 | FULL, MINIMAL, NOBLOB | MINIMAL, FULL | OFF, zlib | - |
| MariaDB | 11.4 | `11.4.13-MariaDB-ubu2404-log` | 3 | NONE, CRC32 | FULL, MINIMAL, NOBLOB | MINIMAL, FULL | OFF, zlib | - |
| MariaDB | 11.8 | `11.8.9-MariaDB-ubu2404-log` | 4 | NONE, CRC32 | FULL, MINIMAL, NOBLOB | MINIMAL, FULL | OFF, zlib | - |
| MariaDB | 12.3 | `12.3.3-MariaDB-ubu2404-log` | 4 | NONE, CRC32 | FULL, MINIMAL, NOBLOB | MINIMAL, FULL | OFF, zlib | - |
| MariaDB | 13.0 | `13.0.2-MariaDB-ubu2604-log` | 3 | NONE, CRC32 | FULL, MINIMAL, NOBLOB | MINIMAL, FULL | OFF, zlib | - |
<!-- compat:summary:end -->

## Configurations

One row per capture. Compression is `zstd` for MySQL `binlog_transaction_compression=ON` and `zlib` for MariaDB `log_bin_compress=ON`. PARTIAL_JSON is `binlog_row_value_options=PARTIAL_JSON`, which only MySQL has.

<!-- compat:configs:begin -->
| Flavor | Version | Checksum | Row image | Row metadata | Compression | PARTIAL_JSON |
|---|---|---|---|---|---|---|
| MySQL | 8.4 | NONE | FULL | MINIMAL | OFF | OFF |
| MySQL | 8.4 | NONE | FULL | FULL | OFF | ON |
| MySQL | 8.4 | NONE | MINIMAL | MINIMAL | zstd | OFF |
| MySQL | 8.4 | NONE | MINIMAL | FULL | zstd | ON |
| MySQL | 8.4 | NONE | NOBLOB | MINIMAL | OFF | ON |
| MySQL | 8.4 | CRC32 | FULL | FULL | OFF | OFF |
| MySQL | 9.7 | NONE | FULL | MINIMAL | zstd | OFF |
| MySQL | 9.7 | NONE | NOBLOB | FULL | zstd | OFF |
| MySQL | 9.7 | CRC32 | FULL | FULL | OFF | OFF |
| MySQL | 9.7 | CRC32 | MINIMAL | MINIMAL | OFF | ON |
| MySQL | 9.7 | CRC32 | NOBLOB | MINIMAL | zstd | ON |
| MariaDB | 10.11 | NONE | FULL | MINIMAL | OFF | - |
| MariaDB | 10.11 | CRC32 | FULL | FULL | OFF | - |
| MariaDB | 10.11 | CRC32 | MINIMAL | FULL | OFF | - |
| MariaDB | 10.11 | CRC32 | NOBLOB | FULL | zlib | - |
| MariaDB | 11.4 | NONE | MINIMAL | FULL | zlib | - |
| MariaDB | 11.4 | NONE | NOBLOB | MINIMAL | OFF | - |
| MariaDB | 11.4 | CRC32 | FULL | FULL | OFF | - |
| MariaDB | 11.8 | NONE | MINIMAL | FULL | zlib | - |
| MariaDB | 11.8 | CRC32 | FULL | MINIMAL | OFF | - |
| MariaDB | 11.8 | CRC32 | FULL | FULL | OFF | - |
| MariaDB | 11.8 | CRC32 | NOBLOB | FULL | OFF | - |
| MariaDB | 12.3 | NONE | FULL | MINIMAL | zlib | - |
| MariaDB | 12.3 | CRC32 | FULL | FULL | OFF | - |
| MariaDB | 12.3 | CRC32 | MINIMAL | MINIMAL | OFF | - |
| MariaDB | 12.3 | CRC32 | NOBLOB | FULL | zlib | - |
| MariaDB | 13.0 | NONE | MINIMAL | MINIMAL | OFF | - |
| MariaDB | 13.0 | NONE | NOBLOB | MINIMAL | zlib | - |
| MariaDB | 13.0 | CRC32 | FULL | FULL | OFF | - |
<!-- compat:configs:end -->

The configurations are a pairwise covering set over checksum, row image, row metadata, compression, PARTIAL_JSON and version, plus one capture per version with CRC32, FULL row image, FULL row metadata and no compression. Every pair of values appears in some capture; not every combination does.

## What the tests check

Every capture runs the same workload: integers of every width at their limits, DECIMAL, FLOAT and DOUBLE edge values, every temporal type with zero dates and all fractional precisions, character data in several character sets, every TEXT and BLOB width, ENUM and SET, JSON (including documents above 64 KB), spatial types, VECTOR where the server has it, MariaDB COMPRESSED columns, and statements from single-row changes to bulk transactions split over many rows events. On MySQL it includes tagged GTIDs.

The test suite decodes every rows event in every capture, expanding compressed events first, and checks that MINIMAL and NOBLOB images produce absent columns and FULL images never do, and that PARTIAL_JSON configurations produce JSON diffs. For the uncompressed MySQL configurations it compares JSON column text, with partial updates applied, against the server's own `CAST(j AS CHAR)` after every transaction. Separate fixtures from MySQL 8.4, MySQL 9.7 and MariaDB 11.8 check each decoded value against the SQL that wrote it.

## Outside the tested set

- Other server versions are not tested. The wire format of the supported events is stable across the versions above, but a version absent from the tables has no captured evidence.
- Statement-based and mixed logging produce QUERY events with statement text and no row images. The library parses QUERY events but does not interpret statements.
- Encrypted binlog files are not decrypted. A server decrypts them before sending them to a replica, so a replication stream is readable; the files on disk are not.
- Servers older than MySQL 5.6.1 or MariaDB 5.3.0 write no checksums; checksum handling recognizes them, but nothing else is tested against them.
