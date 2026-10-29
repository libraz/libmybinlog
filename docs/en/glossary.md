# Glossary

**Artificial event.** An event the source creates for a replication stream rather than reading it from a file, such as the ROTATE that opens a dump. Its header has `kLogEventArtificialFlag` set and `next_position` 0.

**Binlog position.** The byte offset of an event in its binlog file. The first event is at 4, after the magic number. `EventHeader::next_position` is the position of the following event.

**Common header.** The 19 bytes that open every event: timestamp, type code, server id, event length, next position and flags.

**Disposition.** What a reader does with an event type: decode, skip as data-neutral, or stop. See [Event disposition](disposition.md).

**FDE (FORMAT_DESCRIPTION event).** The event after the magic number in every binlog file. It names the server version and declares the checksum algorithm for the rest of the file.

**Flavor.** MySQL or MariaDB. The two share the binlog framework but differ in event numbering, GTIDs, compression and some metadata rules.

**GNO.** The transaction number in a MySQL GTID.

**GTID.** Global transaction identifier: `uuid[:tag]:gno` on MySQL, `domain-server-seq` on MariaDB. See [GTIDs](gtids.md).

**PARTIAL_JSON.** The MySQL setting `binlog_row_value_options=PARTIAL_JSON`, under which a JSON update can be logged as a list of edits. See [JSON](json.md).

**Row image.** The column values of one row as a rows event carries them. An UPDATE has a before image and an after image. `binlog_row_image` (FULL, MINIMAL, NOBLOB) decides which columns an image carries.

**Row metadata.** The optional TABLE_MAP fields controlled by `binlog_row_metadata`: signedness, character sets, column names, ENUM and SET labels.

**Rows event.** WRITE_ROWS, UPDATE_ROWS, DELETE_ROWS or PARTIAL_UPDATE_ROWS: the row images of one table, for one statement or part of one.

**Table id.** The number a TABLE_MAP assigns to a table and its rows events refer to. It is valid only within the file that defines it.

**TABLE_MAP.** The event that describes a table (schema, name, column types and metadata) before the rows events that change it.

**Trailer.** The 4-byte CRC32 at the end of an event when the file's checksum algorithm is CRC32.

**TRANSACTION_PAYLOAD.** The MySQL event that carries a whole transaction, compressed with zstd, when `binlog_transaction_compression=ON`.

**TSID.** Transaction source identifier: a MySQL server UUID plus an optional tag.
