#include <charconv>
#include <cmath>
#include <utility>

#include "bytes_internal.h"
#include "decimal_internal.h"
#include "json_internal.h"
#include "mybinlog/json.h"

namespace mybinlog {
namespace internal {

namespace {

// MY_GCVT_MAX_FIELD_WIDTH: DBL_DIG + 4 + max(5, MAX_DECPT_FOR_F_FORMAT).
constexpr int kGcvtWidth = 15 + 4 + 15;
// MAX_DECPT_FOR_F_FORMAT (DBL_DIG).
constexpr int kMaxDecptForFixed = 15;

// Json_datetime::PACKED_SIZE: an int64 from TIME_to_longlong_packed.
constexpr size_t kPackedTemporalSize = 8;
// my_packed_time_get_frac_part: microseconds in the low 24 bits.
constexpr int kPackedFractionBits = 24;
constexpr int64_t kPackedFractionMask = (int64_t{1} << kPackedFractionBits) - 1;
constexpr uint64_t kMaxYear = 9999;
constexpr uint64_t kMaxHour = 23;
constexpr uint64_t kMaxTimeHour = 838;
constexpr uint64_t kMaxMinuteOrSecond = 59;
constexpr uint64_t kMaxMicrosecond = 999999;
// MYSQL_TYPE_VAR_STRING opaques print as plain strings (wrapper_to_string).
constexpr uint8_t kOpaqueVarString = 0xFD;
// base64_encode breaks its output every 76 characters.
constexpr size_t kBase64LineLength = 76;

constexpr char kHexDigits[] = "0123456789abcdef";
constexpr char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void AppendDigits(uint64_t value, size_t min_width, std::string* out) {
  char buf[24];
  const auto result = std::to_chars(buf, buf + sizeof(buf), value);
  const size_t length = static_cast<size_t>(result.ptr - buf);
  if (length < min_width) out->append(min_width - length, '0');
  out->append(buf, length);
}

void AppendInt(int64_t value, std::string* out) {
  char buf[24];
  const auto result = std::to_chars(buf, buf + sizeof(buf), value);
  out->append(buf, static_cast<size_t>(result.ptr - buf));
}

void AppendFraction(uint64_t micros, std::string* out) {
  out->push_back('.');
  AppendDigits(micros, 6, out);
}

// TIME_from_longlong_time_packed + my_time_to_str(dec = 6).
Status AppendPackedTime(int64_t packed, std::string* out) {
  if (packed == INT64_MIN) return Status::kMalformedValue;
  const bool negative = packed < 0;
  const uint64_t magnitude = static_cast<uint64_t>(negative ? -packed : packed);
  const uint64_t micros = magnitude & kPackedFractionMask;
  const uint64_t hms = magnitude >> kPackedFractionBits;
  const uint64_t hour = hms >> 12;
  const uint64_t minute = (hms >> 6) & 0x3F;
  const uint64_t second = hms & 0x3F;
  if (hour > kMaxTimeHour || minute > kMaxMinuteOrSecond || second > kMaxMinuteOrSecond ||
      micros > kMaxMicrosecond) {
    return Status::kMalformedValue;
  }
  if (negative) out->push_back('-');
  AppendDigits(hour, 2, out);
  out->push_back(':');
  AppendDigits(minute, 2, out);
  out->push_back(':');
  AppendDigits(second, 2, out);
  AppendFraction(micros, out);
  return Status::kOk;
}

// TIME_from_longlong_datetime_packed + my_date_to_str / my_datetime_to_str(dec = 6).
Status AppendPackedDatetime(int64_t packed, bool date_only, std::string* out) {
  if (packed < 0) return Status::kMalformedValue;
  const uint64_t micros = static_cast<uint64_t>(packed) & kPackedFractionMask;
  const uint64_t ymdhms = static_cast<uint64_t>(packed) >> kPackedFractionBits;
  const uint64_t ymd = ymdhms >> 17;
  const uint64_t ym = ymd >> 5;
  const uint64_t hms = ymdhms & 0x1FFFF;
  const uint64_t year = ym / 13;
  const uint64_t month = ym % 13;
  const uint64_t day = ymd & 0x1F;
  const uint64_t hour = hms >> 12;
  const uint64_t minute = (hms >> 6) & 0x3F;
  const uint64_t second = hms & 0x3F;
  if (year > kMaxYear || hour > kMaxHour || minute > kMaxMinuteOrSecond ||
      second > kMaxMinuteOrSecond || micros > kMaxMicrosecond) {
    return Status::kMalformedValue;
  }
  AppendDigits(year, 4, out);
  out->push_back('-');
  AppendDigits(month, 2, out);
  out->push_back('-');
  AppendDigits(day, 2, out);
  if (date_only) return Status::kOk;
  out->push_back(' ');
  AppendDigits(hour, 2, out);
  out->push_back(':');
  AppendDigits(minute, 2, out);
  out->push_back(':');
  AppendDigits(second, 2, out);
  AppendFraction(micros, out);
  return Status::kOk;
}

// Json_decimal::convert_from_binary + decimal2string: precision, scale, bin2decimal bytes.
Status AppendDecimal(const std::string& bytes, std::string* out) {
  if (bytes.size() < 2) return Status::kMalformedValue;
  const uint32_t meta =
      (static_cast<uint32_t>(static_cast<uint8_t>(bytes[0])) << 8) | static_cast<uint8_t>(bytes[1]);
  size_t width = 0;
  if (DecimalWidth(meta, &width) != Status::kOk || width != bytes.size() - 2) {
    return Status::kMalformedValue;
  }
  std::string text;
  const Status status =
      DecodeDecimal(meta, reinterpret_cast<const uint8_t*>(bytes.data()) + 2, &text);
  if (status != Status::kOk) return Status::kMalformedValue;
  out->append(text);
  return Status::kOk;
}

// "base64:typeNN:" + base64_encode (newline every 76 characters), then quoted.
void AppendBase64Opaque(uint8_t field_type, const std::string& bytes, std::string* out) {
  std::string text = "base64:type";
  AppendDigits(field_type, 0, &text);
  text.push_back(':');
  const auto* data = reinterpret_cast<const uint8_t*>(bytes.data());
  size_t line = 0;
  for (size_t i = 0; i < bytes.size(); i += 3) {
    if (line == kBase64LineLength) {
      text.push_back('\n');
      line = 0;
    }
    const size_t left = bytes.size() - i;
    const uint32_t group = (static_cast<uint32_t>(data[i]) << 16) |
                           (left > 1 ? static_cast<uint32_t>(data[i + 1]) << 8 : 0) |
                           (left > 2 ? data[i + 2] : 0);
    text.push_back(kBase64Alphabet[(group >> 18) & 0x3F]);
    text.push_back(kBase64Alphabet[(group >> 12) & 0x3F]);
    text.push_back(left > 1 ? kBase64Alphabet[(group >> 6) & 0x3F] : '=');
    text.push_back(left > 2 ? kBase64Alphabet[group & 0x3F] : '=');
    line += 4;
  }
  AppendJsonQuoted(text.data(), text.size(), out);
}

Status AppendOpaque(const JsonValue& value, std::string* out) {
  const auto type = static_cast<uint8_t>(value.opaque_type);
  switch (value.opaque_type) {
    case ColumnType::kNewDecimal:
      return AppendDecimal(value.bytes, out);
    case ColumnType::kDate:
    case ColumnType::kTime:
    case ColumnType::kDatetime:
    case ColumnType::kTimestamp: {
      if (value.bytes.size() != kPackedTemporalSize) return Status::kMalformedValue;
      const auto packed =
          static_cast<int64_t>(ReadU64Le(reinterpret_cast<const uint8_t*>(value.bytes.data())));
      std::string text;
      const Status status =
          value.opaque_type == ColumnType::kTime
              ? AppendPackedTime(packed, &text)
              : AppendPackedDatetime(packed, value.opaque_type == ColumnType::kDate, &text);
      if (status != Status::kOk) return status;
      out->push_back('"');
      out->append(text);
      out->push_back('"');
      return Status::kOk;
    }
    default:
      break;
  }
  if (type == kOpaqueVarString) {
    AppendJsonQuoted(value.bytes.data(), value.bytes.size(), out);
  } else {
    AppendBase64Opaque(type, value.bytes, out);
  }
  return Status::kOk;
}

// wrapper_to_string(json_quoted = true, pretty = false): @p depth counts enclosing containers.
Status AppendValue(const JsonValue& value, size_t depth, std::string* out) {
  switch (value.kind) {
    case JsonValue::Kind::kNull:
      out->append("null");
      return Status::kOk;
    case JsonValue::Kind::kBoolean:
      out->append(value.b ? "true" : "false");
      return Status::kOk;
    case JsonValue::Kind::kInt:
      AppendInt(value.i, out);
      return Status::kOk;
    case JsonValue::Kind::kUInt:
      AppendDigits(value.u, 0, out);
      return Status::kOk;
    case JsonValue::Kind::kDouble:
      // JSON has no NaN or infinity, so no server document holds one.
      if (!std::isfinite(value.d)) return Status::kMalformedValue;
      AppendJsonDouble(value.d, out);
      return Status::kOk;
    case JsonValue::Kind::kString:
      AppendJsonQuoted(value.bytes.data(), value.bytes.size(), out);
      return Status::kOk;
    case JsonValue::Kind::kOpaque:
      return AppendOpaque(value, out);
    case JsonValue::Kind::kArray: {
      if (depth + 1 > kJsonMaxDepth) return Status::kLimitExceeded;
      out->push_back('[');
      for (size_t i = 0; i < value.elements.size(); ++i) {
        if (i > 0) out->append(", ");
        const Status status = AppendValue(value.elements[i], depth + 1, out);
        if (status != Status::kOk) return status;
      }
      out->push_back(']');
      return Status::kOk;
    }
    case JsonValue::Kind::kObject: {
      if (depth + 1 > kJsonMaxDepth) return Status::kLimitExceeded;
      out->push_back('{');
      for (size_t i = 0; i < value.members.size(); ++i) {
        if (i > 0) out->append(", ");
        const JsonMember& member = value.members[i];
        AppendJsonQuoted(member.key.data(), member.key.size(), out);
        out->append(": ");
        const Status status = AppendValue(member.value, depth + 1, out);
        if (status != Status::kOk) return status;
      }
      out->push_back('}');
      return Status::kOk;
    }
  }
  return Status::kMalformedValue;
}

}  // namespace

void AppendJsonDouble(double value, std::string* out) {
  // Shortest round-trip digits, as dtoa mode 4 with ndigits >= 17 returns them.
  char buf[40];
  const auto result = std::to_chars(buf, buf + sizeof(buf), value, std::chars_format::scientific);
  const char* p = buf;
  const bool sign = *p == '-';
  if (sign) ++p;
  std::string digits;
  for (; p < result.ptr && *p != 'e'; ++p) {
    if (*p != '.') digits.push_back(*p);
  }
  int exponent = 0;
  bool negative_exponent = false;
  if (p < result.ptr) ++p;  // 'e'
  if (p < result.ptr && (*p == '+' || *p == '-')) negative_exponent = *p++ == '-';
  for (; p < result.ptr; ++p) exponent = exponent * 10 + (*p - '0');
  int decpt = (negative_exponent ? -exponent : exponent) + 1;
  const int len = static_cast<int>(digits.size());

  // my_gcvt's choice between 'f' and 'e' formats, transcribed.
  const int width = kGcvtWidth - (value < 0.0 ? 1 : 0);
  const int exp_len = 1 + (decpt >= 101 || decpt <= -99) + (decpt >= 11 || decpt <= -9);
  const bool have_space = (decpt <= 0 ? len - decpt + 2 : decpt < len ? len + 1 : decpt) <= width;
  const bool force_e = decpt <= 0 && width <= 2 - decpt && width >= 3 + exp_len;
  const bool fixed =
      (have_space ||
       ((decpt <= width && (decpt >= -1 || (decpt == -2 && (len > 1 || !force_e)))) && !force_e)) &&
      (!have_space ||
       (decpt >= -kMaxDecptForFixed + 1 && (decpt <= kMaxDecptForFixed || len > decpt)));

  const size_t start = out->size();
  if (sign) out->push_back('-');
  if (fixed) {
    if (decpt <= 0) {
      out->append("0.");
      out->append(static_cast<size_t>(-decpt), '0');
      out->append(digits);
    } else {
      for (int i = 1; i <= len; ++i) {
        out->push_back(digits[static_cast<size_t>(i - 1)]);
        if (i == decpt && i < len) out->push_back('.');
      }
      if (decpt > len) out->append(static_cast<size_t>(decpt - len), '0');
    }
  } else {
    out->push_back(digits[0]);
    if (len > 1) {
      out->push_back('.');
      out->append(digits, 1, std::string::npos);
    }
    out->push_back('e');
    if (--decpt < 0) {
      out->push_back('-');
      decpt = -decpt;
    }
    AppendDigits(static_cast<uint64_t>(decpt), 0, out);
  }
  if (out->find_first_of(".e", start) == std::string::npos) out->append(".0");
}

void AppendJsonQuoted(const char* data, size_t size, std::string* out) {
  out->push_back('"');
  for (size_t i = 0; i < size; ++i) {
    const auto c = static_cast<unsigned char>(data[i]);
    if (c > 0x1F && c != '"' && c != '\\') {
      out->push_back(static_cast<char>(c));
      continue;
    }
    out->push_back('\\');
    switch (c) {
      case '\b':
        out->push_back('b');
        break;
      case '\t':
        out->push_back('t');
        break;
      case '\n':
        out->push_back('n');
        break;
      case '\f':
        out->push_back('f');
        break;
      case '\r':
        out->push_back('r');
        break;
      case '"':
      case '\\':
        out->push_back(static_cast<char>(c));
        break;
      default:
        out->append("u00");
        out->push_back(kHexDigits[c >> 4]);
        out->push_back(kHexDigits[c & 0x0F]);
        break;
    }
  }
  out->push_back('"');
}

}  // namespace internal

Status JsonToText(const JsonValue& value, std::string* out) {
  std::string text;
  const Status status = internal::AppendValue(value, 0, &text);
  if (status != Status::kOk) return status;
  *out = std::move(text);
  return Status::kOk;
}

}  // namespace mybinlog
