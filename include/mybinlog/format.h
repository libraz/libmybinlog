/**
 * @file format.h
 * @brief Text rendering of decoded values, as the server's text protocol shows them.
 *
 * FormatValue() writes what `SELECT col` returns over the text protocol (and
 * `CAST(col AS CHAR)` for temporals and DECIMAL), so a value printed here can
 * be compared with the server's own output. Where the server's text is not a
 * single canonical string, the choice is documented per kind. These are
 * conveniences for tools and examples; nothing else in the library calls them.
 */

#ifndef MYBINLOG_FORMAT_H_
#define MYBINLOG_FORMAT_H_

#include <string>

#include "mybinlog/export.h"
#include "mybinlog/status.h"
#include "mybinlog/table_map.h"
#include "mybinlog/value.h"

namespace mybinlog {

/**
 * @brief Render a value as text.
 *
 * Per Value::Kind:
 * - kInt, kUInt: decimal digits with a leading '-' when negative.
 * - kDouble: `my_gcvt` form, as the server prints it. DOUBLE uses the shortest
 *   digits that round-trip; FLOAT (Value::type kFloat) keeps 6 significant
 *   digits, so 16777216 prints "16777200". Exponent form is used when the
 *   value is at least 1e15 or below 1e-15 (`1e15`, `1.5e-20`), with no '+'
 *   and no leading zeros; negative zero prints "-0"; NaN and infinities, which no server
 *   stores, print "nan", "inf" and "-inf".
 * - kDecimal: Value::bytes verbatim (exactly `scale` fraction digits).
 * - kDate: `YYYY-MM-DD`; kDateTime: `YYYY-MM-DD hh:mm:ss` plus `.` and `fsp`
 *   fraction digits when fsp > 0; kTime: as kDateTime's clock part with a
 *   leading '-' when negative and at least two hour digits (up to 838). Zero
 *   and partial-zero dates print their zeros (`0000-00-00`, `2024-05-00`).
 * - kTimestamp: the UTC date and time in kDateTime's form, so it equals the
 *   server's text under `time_zone='+00:00'`; wire 0 prints the zero
 *   timestamp `0000-00-00 00:00:00` with `fsp` zero fraction digits.
 * - kYear: four digits; the zero year prints "0000".
 * - kBit, kEnum, kSet: the number, as `CAST(col AS UNSIGNED)` shows it (the
 *   text protocol itself sends BIT as raw bytes and ENUM/SET as labels; use
 *   FormatEnumLabel() and FormatSetLabels() for the labels).
 * - kText, kBytes: Value::bytes verbatim, in the column's character set or as
 *   raw binary; nothing is escaped or hex-encoded.
 * - kJson: the server's `CAST(j AS CHAR)` text (JsonBinaryToText()).
 * - kJsonDiff: a JSON array with one object per diff in log order,
 *   `{"op": "REPLACE", "path": "$.a", "value": <JSON text>}`; op is REPLACE,
 *   INSERT or REMOVE, and REMOVE has no "value" member.
 * - kNull, kAbsent: no text; the call fails with kInvalidMetadata. Callers
 *   test Value::kind first and print their own NULL or "absent" marker.
 *
 * @param value A value from DecodeRowsEvent() (or Decoder).
 * @param[out] out Replaced with the text on kOk; untouched otherwise.
 * @return kOk; kInvalidMetadata for kNull, kAbsent or a kind outside
 *   Value::Kind; for kJson and kJsonDiff, whatever JsonBinaryToText() or
 *   ParseJsonDiffs() returns for malformed bytes. Reentrant.
 */
[[nodiscard]] MYBINLOG_API Status FormatValue(const Value& value, std::string* out);

/**
 * @brief The label of an ENUM value.
 *
 * Labels come from the TABLE_MAP's optional metadata, which servers write
 * only under `binlog_row_metadata=FULL`.
 *
 * @param value A kEnum value.
 * @param column The value's TABLE_MAP column; only read during the call.
 * @param[out] out Replaced on kOk with the label, or the empty string for
 *   ordinal 0 (the value an invalid insert stores); untouched otherwise.
 * @return kOk; kInvalidMetadata when @p value is not kEnum, or its ordinal is
 *   past the column's labels (including a column without labels). Reentrant.
 */
[[nodiscard]] MYBINLOG_API Status FormatEnumLabel(const Value& value, const TableMapColumn& column,
                                                  std::string* out);

/**
 * @brief The members of a SET value, comma-separated in declaration order.
 *
 * This is how the server prints a SET.
 *
 * @param value A kSet value.
 * @param column The value's TABLE_MAP column; only read during the call.
 * @param[out] out Replaced on kOk (empty for the empty set); untouched otherwise.
 * @return kOk; kInvalidMetadata when @p value is not kSet, or a set bit has no
 *   label in @p column. Reentrant.
 */
[[nodiscard]] MYBINLOG_API Status FormatSetLabels(const Value& value, const TableMapColumn& column,
                                                  std::string* out);

}  // namespace mybinlog

#endif  // MYBINLOG_FORMAT_H_
