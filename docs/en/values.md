# Values

A decoded column value is a `mybinlog::Value` (`mybinlog/value.h`). It keeps what the wire says, structured and unrendered; turning it into text is a separate, optional step.

## The Value struct

`Value::kind` says which members are meaningful; the others keep their defaults.

| Member | Used by |
|---|---|
| `kind` | Always: the shape of the value. |
| `type` | Always: the column's declared type (`RealType()` of the TABLE_MAP column), so ENUM and SET read `ColumnType::kEnum` and `kSet`. |
| `i` | `kInt` |
| `u` | `kUInt`, `kYear`, `kEnum`, `kSet`, `kBit` |
| `d` | `kDouble` |
| `t` | `kDate`, `kDateTime`, `kTime` |
| `epoch_seconds`, `microsecond`, `fsp` | `kTimestamp` |
| `bytes` | `kDecimal`, `kText`, `kBytes`, `kJson`, `kJsonDiff` |

`kNull` and `kAbsent` carry no data; see [Rows](rows.md) for the difference. `ValueKindName()` returns the enumerator name for logs.

## Column types and value kinds

| SQL type | TABLE_MAP `ColumnType` | `Value::Kind` |
|---|---|---|
| TINYINT, SMALLINT, MEDIUMINT, INT, BIGINT | `kTiny`, `kShort`, `kInt24`, `kLong`, `kLongLong` | `kInt` in `i`; `kUInt` in `u` when SIGNEDNESS marks the column unsigned |
| FLOAT, DOUBLE | `kFloat`, `kDouble` | `kDouble` in `d` (FLOAT widened) |
| DECIMAL | `kNewDecimal` | `kDecimal`: decimal text in `bytes` |
| BIT | `kBit` | `kBit` in `u` |
| YEAR | `kYear` | `kYear` in `u` |
| DATE | `kDate`, `kNewDate` | `kDate` in `t` |
| DATETIME | `kDatetime2`, `kDatetime` | `kDateTime` in `t` |
| TIME | `kTime2`, `kTime` | `kTime` in `t` |
| TIMESTAMP | `kTimestamp2`, `kTimestamp` | `kTimestamp` in `epoch_seconds`, `microsecond`, `fsp` |
| ENUM | `kString` (real type `kEnum`) | `kEnum`: ordinal in `u` |
| SET | `kString` (real type `kSet`) | `kSet`: bitmask in `u` |
| CHAR, BINARY | `kString` | `kText` or `kBytes` in `bytes` |
| VARCHAR, VARBINARY | `kVarchar` | `kText` or `kBytes` in `bytes` |
| TINYTEXT .. LONGTEXT, TINYBLOB .. LONGBLOB | `kBlob` | `kText` or `kBytes` in `bytes` |
| MariaDB COMPRESSED VARCHAR, TEXT, BLOB | `kVarcharCompressed`, `kBlobCompressed` | `kText` or `kBytes`, decompressed |
| JSON (MySQL) | `kJson` | `kJson`: the server's binary JSON in `bytes` |
| JSON column of a PARTIAL_UPDATE_ROWS after image | `kJson` | `kJsonDiff`: the diff vector in `bytes` |
| JSON (MariaDB, an alias of LONGTEXT) | `kBlob` | `kText` |
| GEOMETRY and its subtypes | `kGeometry` | `kBytes`: the 4-byte SRID followed by WKB |
| VECTOR | `kVector` | `kBytes` |
| Typed array (multi-valued index, replication only) | `kTypedArray` | `kBytes` |
| Pre-5.0 DECIMAL | `kDecimal` | not decoded: `kUnsupported` |

Character data is `kText` when the TABLE_MAP gives the column a non-binary collation, and `kBytes` for the binary collation or when the TABLE_MAP carries no character set. The bytes are in the column's character set as stored; nothing is converted to UTF-8. `TableMapColumn::charset` holds the collation id for a caller that converts.

MariaDB's INET4, INET6 and UUID types are logged as binary strings and come back as `kBytes` holding the stored bytes.

## How each kind represents its value

- **Integers.** `kInt` is sign-extended from the column width. Without SIGNEDNESS metadata every integer decodes as signed; see [Rows](rows.md).
- **Floating point.** FLOAT is widened to double, so a FLOAT value compares exactly only after converting back with `static_cast<float>(value.d)`. NaN and infinities pass through.
- **DECIMAL.** Exact decimal text with exactly `scale` fraction digits, such as `"-12.50"`. Negative zero (`"-0.00"`) is kept as written.
- **DATE, DATETIME, TIME.** Calendar and clock fields in `Temporal`. Zero and partial-zero dates (`0000-00-00`, `2024-05-00`) are returned as their fields, never as NULL. `Temporal::negative` is used by TIME only, whose `hour` runs to 838. `fsp` is the declared fractional-second precision and `microsecond` the fraction.
- **TIMESTAMP.** Seconds since the epoch in UTC, plus `microsecond` and `fsp`. No time zone is applied. The zero timestamp stays `epoch_seconds == 0`.
- **YEAR.** 0 for the zero year, otherwise 1901 to 2155.
- **ENUM.** The 1-based ordinal of the label; 0 is the empty value an invalid insert stores.
- **SET.** A bitmask where bit n is the n-th declared member, counting from 0.
- **JSON.** The binary document exactly as stored. `JsonBinaryToText()` prints it; see [JSON](json.md).

## Rendering values as text

`mybinlog/format.h` renders a value the way the server's text protocol shows it, so a printed value can be compared with the output of `SELECT`:

| Kind | `FormatValue()` output |
|---|---|
| `kInt`, `kUInt` | Decimal digits. |
| `kDouble` | The server's `my_gcvt` form: shortest round-trip digits for DOUBLE, 6 significant digits for FLOAT, exponent form for large and small magnitudes (`1e15`, `1.5e-20`). |
| `kDecimal` | `bytes` verbatim. |
| `kDate`, `kDateTime`, `kTime` | `YYYY-MM-DD`, `YYYY-MM-DD hh:mm:ss[.fraction]`, `[-]hh:mm:ss[.fraction]`, with `fsp` fraction digits. Zero dates print their zeros. |
| `kTimestamp` | The UTC date and time in DATETIME form, as the server prints it under `time_zone='+00:00'`. The zero timestamp prints `0000-00-00 00:00:00`. |
| `kYear` | Four digits; the zero year prints `0000`. |
| `kBit`, `kEnum`, `kSet` | The number, as `CAST(col AS UNSIGNED)` shows it. |
| `kText`, `kBytes` | `bytes` verbatim; nothing is escaped or hex-encoded. |
| `kJson` | The server's `CAST(j AS CHAR)` text. |
| `kJsonDiff` | A JSON array with one `{"op", "path", "value"}` object per diff. |
| `kNull`, `kAbsent` | No text: the call fails with `kInvalidMetadata`. Test `kind` first and print your own marker. |

`FormatEnumLabel()` returns an ENUM value's label and `FormatSetLabels()` a SET value's members, comma-separated in declaration order, as the server prints them. Both need the labels the TABLE_MAP carries only under `binlog_row_metadata=FULL`, and fail with `kInvalidMetadata` without them:

```cpp
std::string text;
if (value.kind == mybinlog::Value::Kind::kEnum &&
    mybinlog::FormatEnumLabel(value, column, &text) == mybinlog::Status::kOk) {
  // text is the label, or "" for ordinal 0
}
```

The formatting functions are conveniences for tools, tests and examples. Nothing else in the library calls them.
