# Errors and validation

No libmybinlog function throws. Every fallible function returns a `mybinlog::Status` (`mybinlog/status.h`) and delivers its result through a trailing out pointer, which it leaves untouched on failure unless its documentation says otherwise. Functions that return a `Status` are `[[nodiscard]]`.

## Status values

| `Status` | Meaning |
|---|---|
| `kOk` | Success. |
| `kTruncated` | The input ended before the structure was complete. |
| `kInvalidMetadata` | Column or table metadata is inconsistent or out of range: a column count that does not match the TABLE_MAP, a length-prefix width no server writes, a rows event with no TABLE_MAP before it. |
| `kMalformedValue` | A value's bytes are not a valid encoding for its type: a DATE with month 13, a corrupt compressed stream, an `event_length` shorter than the header. |
| `kUnsupported` | Well-formed input the library does not decode: an unknown column type or compression method, a zstd payload in a build without zstd. |
| `kChecksumMismatch` | A CRC32 trailer does not match, or a checksummed frame is too short to carry one. |
| `kLimitExceeded` | A configured size or budget limit was exceeded. |
| `kNeedMoreData` | Returned only by `Decoder::Next()`: no complete event is buffered yet. Not an error. |

`StatusName()` returns the enumerator name (`"kTruncated"`) for logs. Each function's Doxygen comment lists which statuses it returns and for what.

## Decoder errors

The decoder adds position and context: `Decoder::last_error()` returns the status, the stream offset of the failing event and a message naming the event type and the reason. Errors are sticky until `Reset()`. See [The Decoder](decoder.md).

## Validation

The library treats its input as untrusted. Every length is compared with the bytes that remain before it is used, and metadata and values that no server writes are rejected rather than guessed at: a TABLE_MAP column type of unknown width fails the event, because a guess would misalign every later column. The checks reject, for example, temporal fields out of range, DECIMAL precision outside 1..65, ENUM sizes outside 1..2 bytes, BIT wider than 64, and JSON offsets outside their container.

Memory use is bounded by explicit limits, each failing with `kLimitExceeded`:

| Limit | Default | Bounds |
|---|---|---|
| `DecoderOptions::max_event_size` | 1 GiB | One event's length. |
| `DecoderOptions::max_decompressed_size` | 1 GiB | One expanded MariaDB event or transaction payload. |
| `DecodeOptions::max_value_bytes` | none | One decoded value. |
| `DecodeOptions::max_decompressed_bytes` | 64 MiB | One MariaDB COMPRESSED column value. |
| `DecodeBudget` | per event, sized from the body | Everything one rows event materializes; see [Rows](rows.md). |
| `kMaxTableColumns` | 4096 | Columns in a TABLE_MAP. |
| `kJsonMaxDepth` | 100 | JSON nesting. |
| `GtidSet::kMaxIntervalsPerTsid` | 65536 | Intervals per GTID source. |

## Thread safety

Free functions are reentrant. `Decoder`, `ChecksumTracker` and `DecodeBudget` are mutable state for one stream or one decode and must not be used from two threads at once. `GtidSet` and `MariaDbGtidPosition` may be read concurrently but not mutated concurrently. `kServerSettingRules` is immutable.
