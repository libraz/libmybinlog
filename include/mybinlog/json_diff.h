/**
 * @file json_diff.h
 * @brief Partial JSON updates: decoding a PARTIAL_UPDATE_ROWS diff vector and applying it.
 *
 * With `binlog_row_value_options=PARTIAL_JSON` the server logs a JSON column
 * changed by JSON_SET / JSON_REPLACE / JSON_REMOVE as a list of edits instead
 * of the whole document (`Json_diff_vector`, `sql-common/json_diff.cc`); the
 * row decoder returns those bytes as Value::Kind::kJsonDiff. ParseJsonDiffs()
 * turns them into JsonDiff records, and ApplyJsonDiffs() replays the records
 * on the column's previous value with the server's `apply_json_diff`
 * semantics, giving the value the server stored.
 *
 * Every function is reentrant and keeps no state.
 */

#ifndef MYBINLOG_JSON_DIFF_H_
#define MYBINLOG_JSON_DIFF_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "mybinlog/export.h"
#include "mybinlog/json.h"
#include "mybinlog/status.h"

namespace mybinlog {

/** @brief Edit kind of one diff (`enum_json_diff_operation`; the wire byte is the value). */
enum class JsonDiffOperation : uint8_t {
  kReplace = 0,  ///< Replace the existing value at the path (JSON_REPLACE).
  /**
   * Add a value at the path: a new object member (JSON_INSERT) or an array
   * element placed before the indexed one (JSON_ARRAY_INSERT).
   */
  kInsert = 1,
  kRemove = 2,  ///< Delete the existing value at the path (JSON_REMOVE).
};

/** @brief One edit of a partial JSON update. */
struct JsonDiff {
  JsonDiffOperation op = JsonDiffOperation::kReplace;  ///< Edit kind.
  /**
   * Target as the server prints a JSON path (`Json_path::to_string`): `$`,
   * then `.name` per member (double-quoted with JSON escapes, e.g. `."b c"`,
   * unless the name is an ECMAScript identifier) and `[N]`, `[last]` or
   * `[last-N]` per array cell. ParseJsonDiffs() always writes this canonical
   * form; ApplyJsonDiffs() accepts any MySQL path syntax without wildcards
   * or ranges.
   */
  std::string path;
  JsonValue value;  ///< New value for kReplace and kInsert; the null literal for kRemove.
};

/**
 * @brief Decode one PARTIAL_UPDATE_ROWS JSON column (a `Json_diff_vector`).
 *
 * Expects exactly the bytes of a kJsonDiff Value: a 4-byte little-endian
 * length N, then N bytes holding the diffs back to back. Each diff is the
 * operation byte (0 REPLACE, 1 INSERT, 2 REMOVE), a packed-integer path
 * length and the path text, then, except for REMOVE, a packed-integer value
 * length and that many bytes of binary JSON (zero bytes is the null literal,
 * as in ParseJsonBinary()). N = 0 is an empty vector, which leaves the
 * document unchanged.
 *
 * @param data Diff vector, length prefix included; read only during the
 *   call. May be null when @p size is 0.
 * @param size Byte count; must be 4 + N.
 * @param[out] out Replaced with the diffs in wire order on kOk; untouched otherwise.
 * @return kOk; kTruncated when @p size is below 4 or below 4 + N;
 *   kMalformedValue for bytes after the N declared ones, an operation byte
 *   above 2, a path or value length that runs past N or is a packed-integer
 *   0xFB/0xFF, a path that is not valid MySQL path syntax or holds a
 *   wildcard, range or ellipsis (no server writes those), or a value that is
 *   not valid binary JSON; kLimitExceeded when a value nests deeper than
 *   kJsonMaxDepth. Reentrant.
 */
[[nodiscard]] MYBINLOG_API Status ParseJsonDiffs(const uint8_t* data, size_t size,
                                                 std::vector<JsonDiff>* out);

/**
 * @brief Apply diffs in order to a document, as the server's `apply_json_diffs` does.
 *
 * Every leg but the last must resolve exactly: an array leg needs an array
 * and an index inside it (`[last-N]` counts from the end), a member leg an
 * object holding that key; nothing is auto-wrapped. kReplace needs the full
 * path to resolve and swaps in the new value. kInsert needs the parent to
 * resolve: on an object the member must not exist yet and joins the object at
 * the position the server stores it, keys ordered by byte length and then by
 * bytes, so JsonToText() prints the server's `CAST(j AS CHAR)`; on an array the
 * value goes before element N (`[last-N]` before element size-1-N), or at the
 * end when N is past it. kRemove needs the full path to resolve and deletes
 * that member or element.
 *
 * @param before Document to start from; not modified.
 * @param diffs Edits, applied first to last.
 * @param[out] after Replaced with the edited document on kOk; untouched
 *   otherwise. May alias @p before.
 * @return kOk; kMalformedValue for a path that does not parse, is `$` alone,
 *   holds a wildcard or range, or does not resolve as above, an INSERT of an
 *   existing member, a leg whose kind does not match its container, or an
 *   operation outside the enumerators. Reentrant.
 */
[[nodiscard]] MYBINLOG_API Status ApplyJsonDiffs(const JsonValue& before,
                                                 const std::vector<JsonDiff>& diffs,
                                                 JsonValue* after);

/**
 * @brief ParseJsonBinary() on the previous value, ParseJsonDiffs(),
 *   ApplyJsonDiffs() and JsonToText(): the column's new value as the server prints it.
 * @param before_data The column's previous binary JSON (a kJson Value's bytes);
 *   read only during the call. May be null when @p before_size is 0.
 * @param before_size Byte count; zero is the null literal.
 * @param diff_data The kJsonDiff Value's bytes, as ParseJsonDiffs() expects;
 *   read only during the call. May be null when @p diff_size is 0.
 * @param diff_size Byte count.
 * @param[out] after_text Replaced with the new value's text on kOk; untouched otherwise.
 * @return Any status of the four steps. Reentrant.
 */
[[nodiscard]] MYBINLOG_API Status ApplyJsonDiffsToBinary(const uint8_t* before_data,
                                                         size_t before_size,
                                                         const uint8_t* diff_data, size_t diff_size,
                                                         std::string* after_text);

/**
 * @brief Stable identifier of a diff operation, for logs and messages.
 * @param op Any value, including one outside the enumerators.
 * @return Static string such as "kInsert" ("kUnknown" outside the
 *   enumerators); never null. Reentrant.
 */
[[nodiscard]] MYBINLOG_API const char* JsonDiffOperationName(JsonDiffOperation op) noexcept;

}  // namespace mybinlog

#endif  // MYBINLOG_JSON_DIFF_H_
