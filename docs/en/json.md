# JSON

MySQL stores a JSON column in its own binary format, and row events carry that binary document. MariaDB stores JSON as LONGTEXT, so a MariaDB JSON column arrives as `kText` holding the document text and needs none of this page.

## From binary JSON to text

A MySQL JSON column decodes as `Value::Kind::kJson` with the binary document in `Value::bytes`. `JsonBinaryToText()` prints it byte for byte as the server's `CAST(j AS CHAR)` would:

```cpp
std::string text;
const auto* data = reinterpret_cast<const uint8_t*>(value.bytes.data());
if (mybinlog::JsonBinaryToText(data, value.bytes.size(), &text) == mybinlog::Status::kOk) {
  // text is e.g. {"a": 1, "b": [true, null]}
}
```

The server's text has fixed rules, and the output follows them: `", "` and `": "` as separators, object members in stored order (keys sorted by length, then by bytes), doubles in their shortest round-trip form with `.0` appended to integral values, DATE, TIME, DATETIME and TIMESTAMP values as quoted strings, DECIMAL unquoted, and any other opaque value as `"base64:typeNN:..."`. Strings escape `"`, `\` and control bytes; other bytes, non-ASCII included, pass through.

An empty value (zero bytes) is the JSON null literal: the server stores it for a NULL forced into a NOT NULL column. `FormatValue()` on a `kJson` value calls `JsonBinaryToText()`.

## The document tree

`ParseJsonBinary()` decodes a document into a `JsonValue` tree, for callers that read fields instead of printing the whole document:

```cpp
mybinlog::JsonValue doc;
if (mybinlog::ParseJsonBinary(reinterpret_cast<const uint8_t*>(value.bytes.data()),
                              value.bytes.size(), &doc) == mybinlog::Status::kOk) {
  if (const mybinlog::JsonValue* name = mybinlog::FindJsonMember(doc, "name");
      name != nullptr && name->kind == mybinlog::JsonValue::Kind::kString) {
    // name->bytes holds the utf8mb4 string
  }
}
```

| `JsonValue::Kind` | Data |
|---|---|
| `kNull` | none |
| `kBoolean` | `b` |
| `kInt`, `kUInt` | `i`, `u` (binary int16/32/64 and uint16/32/64) |
| `kDouble` | `d` |
| `kString` | `bytes`, utf8mb4 |
| `kOpaque` | `opaque_type` and `bytes`: any other MySQL value. DATE, TIME, DATETIME and TIMESTAMP carry an 8-byte packed temporal; DECIMAL carries precision, scale and the binary decimal. |
| `kArray` | `elements` |
| `kObject` | `members`, in stored order |

A `JsonValue` owns all of its bytes, and copying it copies the subtree. `JsonToText()` prints a tree as `JsonBinaryToText()` would. Nesting deeper than `kJsonMaxDepth` (100, the server's limit) fails with `kLimitExceeded`, and every offset and length in the binary document is bounds-checked.

## Partial updates

With `binlog_row_value_options=PARTIAL_JSON`, MySQL can log an UPDATE that changes a JSON column through `JSON_SET`, `JSON_REPLACE` or `JSON_REMOVE` as a list of edits instead of the whole new document. Such an update arrives as a PARTIAL_UPDATE_ROWS event (type 39). In its after image, a JSON column that carries edits decodes as `Value::Kind::kJsonDiff`; every other column, and every row the server logged in full, decodes as in UPDATE_ROWS.

Whether the server logs a diff depends on the row image and on the update itself, so a reader must handle both kinds in the same column:

```cpp
// previous: the column's last known kJson value; after: the after-image value.
mybinlog::Status NewJsonText(const mybinlog::Value& previous, const mybinlog::Value& after,
                             std::string* text) {
  const auto* before_bytes = reinterpret_cast<const uint8_t*>(previous.bytes.data());
  const auto* after_bytes = reinterpret_cast<const uint8_t*>(after.bytes.data());
  if (after.kind == mybinlog::Value::Kind::kJson) {
    return mybinlog::JsonBinaryToText(after_bytes, after.bytes.size(), text);
  }
  return mybinlog::ApplyJsonDiffsToBinary(before_bytes, previous.bytes.size(), after_bytes,
                                          after.bytes.size(), text);
}
```

A diff applies to the column's previous value. Under a FULL row image that value is in the before image; under MINIMAL the before image does not carry the JSON column, and the reader has to supply the value it last saw for that row.

The steps are also available one by one:

| Function | Does |
|---|---|
| `ParseJsonDiffs()` | Decodes a `kJsonDiff` value into `JsonDiff` records (operation, path, value), in log order. |
| `ApplyJsonDiffs()` | Applies the records to a `JsonValue` with the server's semantics, giving the document the server stored. Object members inserted by a diff take the position the server stores them in, so `JsonToText()` prints the server's text. |
| `ApplyJsonDiffsToBinary()` | `ParseJsonBinary()` on the previous value, then the two above, then `JsonToText()`. |

A `JsonDiff::path` is in the form the server prints a JSON path (`$.a`, `$."b c"`, `$[0]`, `$[last]`); `ApplyJsonDiffs()` accepts any MySQL path without wildcards or ranges. `FormatValue()` on a `kJsonDiff` value prints the edits as a JSON array of `{"op": "REPLACE", "path": "$.a", "value": ...}` objects, with no `value` member for REMOVE.
