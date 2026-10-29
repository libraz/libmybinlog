#include "mybinlog/format.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "json_internal.h"
#include "mybinlog/json.h"
#include "mybinlog/json_diff.h"

namespace mybinlog {
namespace {

// Significant digits `my_gcvt` keeps for MY_GCVT_ARG_FLOAT (FLT_DIG).
constexpr int kFloatDigits = 6;
constexpr int64_t kSecondsPerDay = 86400;

void AppendPadded(uint64_t value, size_t width, std::string* out) {
  std::string digits = std::to_string(value);
  if (digits.size() < width) out->append(width - digits.size(), '0');
  out->append(digits);
}

void AppendFraction(uint32_t microsecond, uint8_t fsp, std::string* out) {
  if (fsp == 0) return;
  std::string digits;
  AppendPadded(microsecond, 6, &digits);
  out->push_back('.');
  out->append(digits, 0, fsp > 6 ? 6 : fsp);
}

void AppendDate(uint64_t year, uint64_t month, uint64_t day, std::string* out) {
  AppendPadded(year, 4, out);
  out->push_back('-');
  AppendPadded(month, 2, out);
  out->push_back('-');
  AppendPadded(day, 2, out);
}

void AppendClock(uint64_t hour, uint64_t minute, uint64_t second, uint32_t microsecond, uint8_t fsp,
                 std::string* out) {
  AppendPadded(hour, 2, out);
  out->push_back(':');
  AppendPadded(minute, 2, out);
  out->push_back(':');
  AppendPadded(second, 2, out);
  AppendFraction(microsecond, fsp, out);
}

// Proleptic Gregorian civil date of a day count since 1970-01-01 (H. Hinnant's days_from_civil
// inverse).
void CivilFromDays(int64_t days, int64_t* year, unsigned* month, unsigned* day) {
  days += 719468;
  const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const auto doe = static_cast<unsigned>(days - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  *day = doy - (153 * mp + 2) / 5 + 1;
  *month = mp < 10 ? mp + 3 : mp - 9;
  *year = static_cast<int64_t>(yoe) + era * 400 + (*month <= 2 ? 1 : 0);
}

void AppendTimestamp(const Value& value, std::string* out) {
  if (value.epoch_seconds == 0 && value.microsecond == 0) {
    out->append("0000-00-00 ");
    AppendClock(0, 0, 0, 0, value.fsp, out);
    return;
  }
  int64_t days = value.epoch_seconds / kSecondsPerDay;
  int64_t seconds = value.epoch_seconds % kSecondsPerDay;
  if (seconds < 0) {
    seconds += kSecondsPerDay;
    --days;
  }
  int64_t year = 0;
  unsigned month = 0;
  unsigned day = 0;
  CivilFromDays(days, &year, &month, &day);
  AppendDate(static_cast<uint64_t>(year), month, day, out);
  out->push_back(' ');
  const auto s = static_cast<uint64_t>(seconds);
  AppendClock(s / 3600, s / 60 % 60, s % 60, value.microsecond, value.fsp, out);
}

void AppendDouble(const Value& value, std::string* out) {
  if (std::isnan(value.d)) {
    out->append("nan");
  } else if (std::isinf(value.d)) {
    out->append(value.d < 0 ? "-inf" : "inf");
  } else {
    internal::AppendGcvt(value.d, value.type == ColumnType::kFloat ? kFloatDigits : 0, out);
  }
}

const char* DiffOpText(JsonDiffOperation op) {
  switch (op) {
    case JsonDiffOperation::kReplace:
      return "REPLACE";
    case JsonDiffOperation::kInsert:
      return "INSERT";
    case JsonDiffOperation::kRemove:
      return "REMOVE";
  }
  return "UNKNOWN";
}

Status AppendJsonDiffs(const std::string& bytes, std::string* out) {
  std::vector<JsonDiff> diffs;
  Status status =
      ParseJsonDiffs(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), &diffs);
  if (status != Status::kOk) return status;
  out->push_back('[');
  for (size_t i = 0; i < diffs.size(); ++i) {
    const JsonDiff& diff = diffs[i];
    if (i > 0) out->append(", ");
    out->append("{\"op\": \"");
    out->append(DiffOpText(diff.op));
    out->append("\", \"path\": ");
    internal::AppendJsonQuoted(diff.path.data(), diff.path.size(), out);
    if (diff.op != JsonDiffOperation::kRemove) {
      std::string text;
      status = JsonToText(diff.value, &text);
      if (status != Status::kOk) return status;
      out->append(", \"value\": ");
      out->append(text);
    }
    out->push_back('}');
  }
  out->push_back(']');
  return Status::kOk;
}

// Every case returns, so -Werror=switch catches a new Value::Kind.
Status Render(const Value& value, std::string* text) {
  switch (value.kind) {
    case Value::Kind::kNull:
    case Value::Kind::kAbsent:
      return Status::kInvalidMetadata;
    case Value::Kind::kInt:
      *text = std::to_string(value.i);
      return Status::kOk;
    case Value::Kind::kUInt:
    case Value::Kind::kEnum:
    case Value::Kind::kSet:
    case Value::Kind::kBit:
      *text = std::to_string(value.u);
      return Status::kOk;
    case Value::Kind::kDouble:
      AppendDouble(value, text);
      return Status::kOk;
    case Value::Kind::kDecimal:
    case Value::Kind::kText:
    case Value::Kind::kBytes:
      *text = value.bytes;
      return Status::kOk;
    case Value::Kind::kDate:
      AppendDate(value.t.year, value.t.month, value.t.day, text);
      return Status::kOk;
    case Value::Kind::kDateTime:
      AppendDate(value.t.year, value.t.month, value.t.day, text);
      text->push_back(' ');
      AppendClock(value.t.hour, value.t.minute, value.t.second, value.t.microsecond, value.t.fsp,
                  text);
      return Status::kOk;
    case Value::Kind::kTime:
      if (value.t.negative) text->push_back('-');
      AppendClock(value.t.hour, value.t.minute, value.t.second, value.t.microsecond, value.t.fsp,
                  text);
      return Status::kOk;
    case Value::Kind::kTimestamp:
      AppendTimestamp(value, text);
      return Status::kOk;
    case Value::Kind::kYear:
      AppendPadded(value.u, 4, text);
      return Status::kOk;
    case Value::Kind::kJson:
      return JsonBinaryToText(reinterpret_cast<const uint8_t*>(value.bytes.data()),
                              value.bytes.size(), text);
    case Value::Kind::kJsonDiff:
      return AppendJsonDiffs(value.bytes, text);
  }
  return Status::kInvalidMetadata;
}

}  // namespace

Status FormatValue(const Value& value, std::string* out) {
  std::string text;
  const Status status = Render(value, &text);
  if (status == Status::kOk) *out = std::move(text);
  return status;
}

Status FormatEnumLabel(const Value& value, const TableMapColumn& column, std::string* out) {
  if (value.kind != Value::Kind::kEnum) return Status::kInvalidMetadata;
  if (value.u == 0) {
    out->clear();
    return Status::kOk;
  }
  if (value.u > column.labels.size()) return Status::kInvalidMetadata;
  *out = column.labels[static_cast<size_t>(value.u - 1)];
  return Status::kOk;
}

Status FormatSetLabels(const Value& value, const TableMapColumn& column, std::string* out) {
  if (value.kind != Value::Kind::kSet) return Status::kInvalidMetadata;
  std::string text;
  bool first = true;
  for (size_t bit = 0; bit < 64; ++bit) {
    if ((value.u >> bit & 1U) == 0) continue;
    if (bit >= column.labels.size()) return Status::kInvalidMetadata;
    if (!first) text.push_back(',');
    first = false;
    text.append(column.labels[bit]);
  }
  *out = std::move(text);
  return Status::kOk;
}

}  // namespace mybinlog
