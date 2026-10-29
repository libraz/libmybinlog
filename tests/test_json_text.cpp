// JSON text as the server's CAST(j AS CHAR) prints it (sql-common/json_dom.cc
// wrapper_to_string): separators, string escapes, my_gcvt doubles, opaque
// temporals and decimals. Expected strings come from json_dom-t.cc and from
// server output captured in tests/fixtures/matrix.

#include <gtest/gtest.h>

#include <cfloat>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "mybinlog/json.h"
#include "mybinlog/value.h"

namespace mybinlog {
namespace {

using Kind = JsonValue::Kind;

JsonValue Scalar(Kind kind) {
  JsonValue v;
  v.kind = kind;
  return v;
}

JsonValue Int(int64_t i) {
  JsonValue v = Scalar(Kind::kInt);
  v.i = i;
  return v;
}

JsonValue UInt(uint64_t u) {
  JsonValue v = Scalar(Kind::kUInt);
  v.u = u;
  return v;
}

JsonValue Dbl(double d) {
  JsonValue v = Scalar(Kind::kDouble);
  v.d = d;
  return v;
}

JsonValue Str(std::string s) {
  JsonValue v = Scalar(Kind::kString);
  v.bytes = std::move(s);
  return v;
}

JsonValue Bool(bool b) {
  JsonValue v = Scalar(Kind::kBoolean);
  v.b = b;
  return v;
}

JsonValue Opaque(uint8_t type, std::string bytes) {
  JsonValue v = Scalar(Kind::kOpaque);
  v.opaque_type = static_cast<ColumnType>(type);
  v.bytes = std::move(bytes);
  return v;
}

JsonValue Arr(std::vector<JsonValue> elements) {
  JsonValue v = Scalar(Kind::kArray);
  v.elements = std::move(elements);
  return v;
}

JsonValue Obj(std::vector<JsonMember> members) {
  JsonValue v = Scalar(Kind::kObject);
  v.members = std::move(members);
  return v;
}

std::string Text(const JsonValue& v) {
  std::string text;
  EXPECT_EQ(JsonToText(v, &text), Status::kOk);
  return text;
}

Status TextStatus(const JsonValue& v) {
  std::string text = "keep";
  const Status status = JsonToText(v, &text);
  if (status != Status::kOk) EXPECT_EQ(text, "keep");
  return status;
}

std::string Packed(int64_t packed) {
  std::string out(8, '\0');
  for (int i = 0; i < 8; ++i) out[i] = static_cast<char>(static_cast<uint64_t>(packed) >> (8 * i));
  return out;
}

int64_t PackedDatetime(int64_t year, int64_t month, int64_t day, int64_t hour, int64_t minute,
                       int64_t second, int64_t micros) {
  const int64_t ymd = ((year * 13 + month) << 5) | day;
  const int64_t hms = (hour << 12) | (minute << 6) | second;
  return (((ymd << 17) | hms) << 24) + micros;
}

int64_t PackedTime(bool negative, int64_t hour, int64_t minute, int64_t second, int64_t micros) {
  const int64_t packed = (((hour << 12) | (minute << 6) | second) << 24) + micros;
  return negative ? -packed : packed;
}

constexpr uint8_t kDate = 0x0A;
constexpr uint8_t kTime = 0x0B;
constexpr uint8_t kDatetime = 0x0C;
constexpr uint8_t kTimestamp = 0x07;
constexpr uint8_t kNewDecimal = 0xF6;

std::string DecimalBytes(uint8_t precision, uint8_t scale, std::vector<uint8_t> bin) {
  std::string out;
  out.push_back(static_cast<char>(precision));
  out.push_back(static_cast<char>(scale));
  out.append(bin.begin(), bin.end());
  return out;
}

// ---- json_dom-t.cc BasicTest ----------------------------------------------

TEST(CodecJsonText, UpstreamDomScalars) {
  EXPECT_EQ(Text(Str("abc")), "\"abc\"");
  EXPECT_EQ(Text(Bool(true)), "true");
  EXPECT_EQ(Text(Int(-123)), "-123");
  EXPECT_EQ(Text(Int(2147483647)), "2147483647");
  EXPECT_EQ(Text(Int(9223372036854775807LL)), "9223372036854775807");
  EXPECT_EQ(Text(UInt(18446744073709551615ULL)), "18446744073709551615");
  EXPECT_EQ(Text(Dbl(-123.45)), "-123.45");
  EXPECT_EQ(Text(Int(2)), "2");
  EXPECT_EQ(Text(JsonValue()), "null");
}

TEST(CodecJsonText, UpstreamDomEscapes) {
  EXPECT_EQ(Text(Arr({Str("a\"b\\c")})), "[\"a\\\"b\\\\c\"]");
  EXPECT_EQ(Text(Arr({Str("a\b\f\n\r\tb")})), "[\"a\\b\\f\\n\\r\\tb\"]");
  EXPECT_EQ(Text(Arr({Str("丳\x13"
                          "丽\x3")})),
            "[\"丳\\u0013丽\\u0003\"]");
  EXPECT_EQ(Text(Arr({Int(3), Obj({{"abc", Str(std::string("\0inTheText", 10))}})})),
            "[3, {\"abc\": \"\\u0000inTheText\"}]");
}

TEST(CodecJsonText, UpstreamDomContainers) {
  const JsonValue vals = Arr({Str("val1"), Str("val2")});
  EXPECT_EQ(Text(vals), "[\"val1\", \"val2\"]");
  EXPECT_EQ(Text(Obj({{"key1", Str("js6")}, {"key2", vals}})),
            "{\"key1\": \"js6\", \"key2\": [\"val1\", \"val2\"]}");
  const JsonValue o = Obj({{"key1", JsonValue()}, {"key2", vals}});
  EXPECT_EQ(Text(o), "{\"key1\": null, \"key2\": [\"val1\", \"val2\"]}");
  EXPECT_EQ(Text(Arr({Int(123), o})), "[123, {\"key1\": null, \"key2\": [\"val1\", \"val2\"]}]");
  EXPECT_EQ(Text(Arr({Int(123)})), "[123]");
  EXPECT_EQ(
      Text(Arr({Str("val1"), Str("val2"), Opaque(kNewDecimal, DecimalBytes(3, 2, {0x83, 0x0E}))})),
      "[\"val1\", \"val2\", 3.14]");
  EXPECT_EQ(Text(Arr({Int(0), Int(2)})), "[0, 2]");
  EXPECT_EQ(Text(Arr({JsonValue(), Bool(false), Bool(true)})), "[null, false, true]");
  EXPECT_EQ(Text(Obj({{"abc", Int(3)},
                      {"foo", Arr({Int(1), Int(2), Obj({{"foo", Dbl(3.24)}}), JsonValue()})}})),
            "{\"abc\": 3, \"foo\": [1, 2, {\"foo\": 3.24}, null]}");
  EXPECT_EQ(Text(Obj({})), "{}");
  EXPECT_EQ(Text(Arr({})), "[]");
}

TEST(CodecJsonText, UpstreamDomTemporalAndOpaque) {
  EXPECT_EQ(Text(Opaque(kDatetime, Packed(PackedDatetime(2014, 11, 15, 12, 4, 55, 123456)))),
            "\"2014-11-15 12:04:55.123456\"");
  EXPECT_EQ(Text(Opaque(0xF9, std::string("\xBE\xBA\xFE\xCA", 4))), "\"base64:type249:vrr+yg==\"");
}

// json_dom-t.cc EscapeSpecialChars: every byte 0..127.
TEST(CodecJsonText, EscapeEveryAsciiByte) {
  std::string input;
  for (int c = 0; c < 128; ++c) input.push_back(static_cast<char>(c));
  const std::string expected =
      R"x("\u0000\u0001\u0002\u0003\u0004\u0005\u0006\u0007\b\t\n\u000b\f\r\u000e\u000f)x"
      R"x(\u0010\u0011\u0012\u0013\u0014\u0015\u0016\u0017\u0018\u0019\u001a\u001b\u001c)x"
      R"x(\u001d\u001e\u001f !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\])x"
      R"x(^_`abcdefghijklmnopqrstuvwxyz{|}~)x"
      "\x7f\"";
  EXPECT_EQ(Text(Str(input)), expected);
}

TEST(CodecJsonText, NonAsciiPassesThrough) {
  // Matrix row t_json id 11: decoded escapes and raw UTF-8 print identically.
  EXPECT_EQ(Text(Str("\xC3\xA9\xF0\x9F\x98\x80 \n\t\"q\" \\ \xC3\xA9\xF0\x9F\x98\x80")),
            "\"\xC3\xA9\xF0\x9F\x98\x80 \\n\\t\\\"q\\\" \\\\ \xC3\xA9\xF0\x9F\x98\x80\"");
  // Invalid UTF-8 is not the printer's business: bytes >= 0x80 pass as-is.
  EXPECT_EQ(Text(Str("\xFF\x80")), "\"\xFF\x80\"");
  EXPECT_EQ(Text(Obj({{"\n", Int(1)}})), "{\"\\n\": 1}");
}

// ---- Doubles: my_gcvt(MY_GCVT_ARG_DOUBLE, 34) plus ".0" ---------------------

TEST(CodecJsonText, Doubles) {
  const std::pair<double, const char*> cases[] = {
      // Server output from the matrix fixtures and json_dom-t.cc.
      {3.14, "3.14"},
      {1.5e300, "1.5e300"},
      {1e-300, "1e-300"},
      {2.5, "2.5"},
      {1.5, "1.5"},
      {-123.45, "-123.45"},
      {3.24, "3.24"},
      // Integral values keep a ".0"; zero keeps its sign.
      {0.0, "0.0"},
      {-0.0, "-0.0"},
      {1.0, "1.0"},
      {-1.0, "-1.0"},
      {100.0, "100.0"},
      {1e10, "10000000000.0"},
      // Shortest round-trip digits.
      {0.1, "0.1"},
      {-2.25, "-2.25"},
      {1.0 / 3.0, "0.3333333333333333"},
      {0.1 + 0.2, "0.30000000000000004"},
      {0.001, "0.001"},
      {1e-7, "0.0000001"},
      // Fixed notation up to decpt 15 (1e14), exponent from 1e15.
      {1e14, "100000000000000.0"},
      {123456789012345.0, "123456789012345.0"},
      {1e15, "1e15"},
      {1234567890123456.0, "1.234567890123456e15"},
      {12345678901234567890.0, "1.2345678901234567e19"},
      // ... and once a fraction digit remains, fixed even past decpt 15.
      {1234567890123456.8, "1234567890123456.8"},
      // Fixed down to decpt -14 (1e-15), exponent below.
      {1e-15, "0.000000000000001"},
      {-1.5e-15, "-0.0000000000000015"},
      {1e-16, "1e-16"},
      {-2.5e-16, "-2.5e-16"},
      // Exponent widths of one, two and three digits.
      {1e20, "1e20"},
      {1e100, "1e100"},
      {1.2345678901234567e-100, "1.2345678901234567e-100"},
      {DBL_MAX, "1.7976931348623157e308"},
      {-DBL_MAX, "-1.7976931348623157e308"},
      {DBL_MIN, "2.2250738585072014e-308"},
      {std::numeric_limits<double>::denorm_min(), "5e-324"},
  };
  for (const auto& [value, expected] : cases) {
    EXPECT_EQ(Text(Dbl(value)), expected) << "value " << value;
  }
}

TEST(CodecJsonText, NonFiniteDoubleRejected) {
  EXPECT_EQ(TextStatus(Dbl(std::numeric_limits<double>::quiet_NaN())), Status::kMalformedValue);
  EXPECT_EQ(TextStatus(Dbl(std::numeric_limits<double>::infinity())), Status::kMalformedValue);
  EXPECT_EQ(TextStatus(Arr({Dbl(-std::numeric_limits<double>::infinity())})),
            Status::kMalformedValue);
}

// ---- Opaque temporals: my_TIME_to_str(dec = 6), quoted --------------------

TEST(CodecJsonText, OpaqueTemporals) {
  // Matrix rows t_json id 16 and 17.
  EXPECT_EQ(Text(Opaque(kDate, Packed(PackedDatetime(2024, 1, 2, 0, 0, 0, 0)))), "\"2024-01-02\"");
  EXPECT_EQ(Text(Opaque(kTime, Packed(PackedTime(true, 12, 34, 56, 500000)))),
            "\"-12:34:56.500000\"");
  EXPECT_EQ(Text(Opaque(kDatetime, Packed(PackedDatetime(2024, 1, 2, 3, 4, 5, 678000)))),
            "\"2024-01-02 03:04:05.678000\"");
  EXPECT_EQ(Text(Opaque(kDatetime, Packed(PackedDatetime(2038, 1, 19, 3, 14, 7, 0)))),
            "\"2038-01-19 03:14:07.000000\"");
  EXPECT_EQ(Text(Opaque(kTime, Packed(PackedTime(false, 0, 0, 0, 0)))), "\"00:00:00.000000\"");
  // TIMESTAMP prints as a DATETIME.
  EXPECT_EQ(Text(Opaque(kTimestamp, Packed(PackedDatetime(1970, 1, 1, 0, 0, 1, 1)))),
            "\"1970-01-01 00:00:01.000001\"");
  // Edges: zero date, three-digit hours, a negative fraction-only TIME.
  EXPECT_EQ(Text(Opaque(kDate, Packed(0))), "\"0000-00-00\"");
  EXPECT_EQ(Text(Opaque(kDatetime, Packed(0))), "\"0000-00-00 00:00:00.000000\"");
  EXPECT_EQ(Text(Opaque(kDatetime, Packed(PackedDatetime(9999, 12, 31, 23, 59, 59, 999999)))),
            "\"9999-12-31 23:59:59.999999\"");
  EXPECT_EQ(Text(Opaque(kTime, Packed(PackedTime(false, 838, 59, 59, 0)))), "\"838:59:59.000000\"");
  EXPECT_EQ(Text(Opaque(kTime, Packed(PackedTime(true, 100, 0, 0, 0)))), "\"-100:00:00.000000\"");
  EXPECT_EQ(Text(Opaque(kTime, Packed(PackedTime(true, 0, 0, 0, 1)))), "\"-00:00:00.000001\"");
}

TEST(CodecJsonText, OpaqueTemporalsMalformed) {
  EXPECT_EQ(TextStatus(Opaque(kDate, std::string(7, '\0'))), Status::kMalformedValue);
  EXPECT_EQ(TextStatus(Opaque(kTime, std::string(9, '\0'))), Status::kMalformedValue);
  EXPECT_EQ(TextStatus(Opaque(kDatetime, Packed(PackedDatetime(10000, 1, 1, 0, 0, 0, 0)))),
            Status::kMalformedValue);
  EXPECT_EQ(TextStatus(Opaque(kDatetime, Packed(PackedDatetime(2024, 1, 1, 24, 0, 0, 0)))),
            Status::kMalformedValue);
  EXPECT_EQ(TextStatus(Opaque(kDatetime, Packed(PackedDatetime(2024, 1, 1, 0, 60, 0, 0)))),
            Status::kMalformedValue);
  EXPECT_EQ(TextStatus(Opaque(kTimestamp, Packed(PackedDatetime(2024, 1, 1, 0, 0, 60, 0)))),
            Status::kMalformedValue);
  EXPECT_EQ(TextStatus(Opaque(kDatetime, Packed(PackedDatetime(2024, 1, 1, 0, 0, 0, 1000000)))),
            Status::kMalformedValue);
  EXPECT_EQ(TextStatus(Opaque(kDate, Packed(-PackedDatetime(2024, 1, 1, 0, 0, 0, 0)))),
            Status::kMalformedValue);
  EXPECT_EQ(TextStatus(Opaque(kTime, Packed(PackedTime(false, 839, 0, 0, 0)))),
            Status::kMalformedValue);
  EXPECT_EQ(TextStatus(Opaque(kTime, Packed(PackedTime(true, 1, 60, 0, 0)))),
            Status::kMalformedValue);
  EXPECT_EQ(TextStatus(Opaque(kTime, Packed(PackedTime(false, 1, 0, 0, 1000000)))),
            Status::kMalformedValue);
  EXPECT_EQ(TextStatus(Opaque(kTime, Packed(std::numeric_limits<int64_t>::min()))),
            Status::kMalformedValue);
}

// ---- Opaque DECIMAL: decimal2string, unquoted -----------------------------

TEST(CodecJsonText, OpaqueDecimals) {
  EXPECT_EQ(Text(Opaque(kNewDecimal, DecimalBytes(5, 2, {0x80, 0x7B, 0x2D}))), "123.45");
  EXPECT_EQ(Text(Opaque(kNewDecimal, DecimalBytes(5, 2, {0x7F, 0x84, 0xD2}))), "-123.45");
  // Matrix rows t_json id 16 and 17.
  EXPECT_EQ(Text(Opaque(kNewDecimal, DecimalBytes(10, 3, {0x80, 0x00, 0x00, 0x0C, 0x01, 0x59}))),
            "12.345");
  EXPECT_EQ(Text(Opaque(kNewDecimal, DecimalBytes(5, 2, {0x7F, 0xFF, 0xCD}))), "-0.50");
  EXPECT_EQ(Text(Opaque(kNewDecimal, DecimalBytes(5, 2, {0x80, 0x01, 0x32}))), "1.50");
  EXPECT_EQ(Text(Opaque(kNewDecimal, DecimalBytes(1, 0, {0x80}))), "0");
}

TEST(CodecJsonText, OpaqueDecimalsMalformed) {
  EXPECT_EQ(TextStatus(Opaque(kNewDecimal, "")), Status::kMalformedValue);
  EXPECT_EQ(TextStatus(Opaque(kNewDecimal, DecimalBytes(5, 2, {}))), Status::kMalformedValue);
  // Payload one byte short and one byte long for DECIMAL(5,2).
  EXPECT_EQ(TextStatus(Opaque(kNewDecimal, DecimalBytes(5, 2, {0x80, 0x7B}))),
            Status::kMalformedValue);
  EXPECT_EQ(TextStatus(Opaque(kNewDecimal, DecimalBytes(5, 2, {0x80, 0x7B, 0x2D, 0x00}))),
            Status::kMalformedValue);
  // Precision zero, precision 66, and scale above precision.
  EXPECT_EQ(TextStatus(Opaque(kNewDecimal, DecimalBytes(0, 0, {0x80}))), Status::kMalformedValue);
  EXPECT_EQ(TextStatus(Opaque(kNewDecimal, DecimalBytes(66, 0, std::vector<uint8_t>(30, 0x80)))),
            Status::kMalformedValue);
  EXPECT_EQ(TextStatus(Opaque(kNewDecimal, DecimalBytes(2, 3, {0x80, 0x00}))),
            Status::kMalformedValue);
  // A digit group above 99 in a two-digit group.
  EXPECT_EQ(TextStatus(Opaque(kNewDecimal, DecimalBytes(2, 0, {0xE4}))), Status::kMalformedValue);
}

// ---- Other opaque values: "base64:typeNN:..." ------------------------------

TEST(CodecJsonText, OpaqueBase64) {
  // Matrix row t_json id 21: UNHEX('0102FF') is a VARCHAR binary string.
  EXPECT_EQ(Text(Opaque(0x0F, std::string("\x01\x02\xFF", 3))), "\"base64:type15:AQL/\"");
  EXPECT_EQ(Text(Opaque(0xFC, "abcd")), "\"base64:type252:YWJjZA==\"");
  EXPECT_EQ(Text(Opaque(0xFC, "ab")), "\"base64:type252:YWI=\"");
  EXPECT_EQ(Text(Opaque(0xFC, "")), "\"base64:type252:\"");
  EXPECT_EQ(Text(Opaque(0xFF, "a")), "\"base64:type255:YQ==\"");
  // base64_encode breaks lines every 76 characters; the newline is escaped.
  std::string encoded;
  for (int i = 0; i < 19; ++i) encoded += "eHh4";
  EXPECT_EQ(Text(Opaque(0xFC, std::string(57, 'x'))), "\"base64:type252:" + encoded + "\"");
  EXPECT_EQ(Text(Opaque(0xFC, std::string(60, 'x'))), "\"base64:type252:" + encoded + "\\neHh4\"");
}

TEST(CodecJsonText, OpaqueVarStringPrintsAsString) {
  EXPECT_EQ(Text(Opaque(0xFD, "a\"b\n")), "\"a\\\"b\\n\"");
}

TEST(CodecJsonText, DepthLimit) {
  JsonValue v = Int(1);
  for (size_t i = 0; i < kJsonMaxDepth; ++i) v = Arr({std::move(v)});
  EXPECT_EQ(TextStatus(v), Status::kOk);
  EXPECT_EQ(TextStatus(Obj({{"k", v}})), Status::kLimitExceeded);
}

TEST(CodecJsonText, KindNames) {
  EXPECT_STREQ(JsonKindName(Kind::kNull), "kNull");
  EXPECT_STREQ(JsonKindName(Kind::kBoolean), "kBoolean");
  EXPECT_STREQ(JsonKindName(Kind::kInt), "kInt");
  EXPECT_STREQ(JsonKindName(Kind::kUInt), "kUInt");
  EXPECT_STREQ(JsonKindName(Kind::kDouble), "kDouble");
  EXPECT_STREQ(JsonKindName(Kind::kString), "kString");
  EXPECT_STREQ(JsonKindName(Kind::kOpaque), "kOpaque");
  EXPECT_STREQ(JsonKindName(Kind::kArray), "kArray");
  EXPECT_STREQ(JsonKindName(Kind::kObject), "kObject");
  EXPECT_STREQ(JsonKindName(static_cast<Kind>(200)), "kUnknown");
  EXPECT_STREQ(ValueKindName(Value::Kind::kJson), "kJson");
  EXPECT_STREQ(ValueKindName(Value::Kind::kJsonDiff), "kJsonDiff");
  EXPECT_STREQ(ValueKindName(Value::Kind::kAbsent), "kAbsent");
}

TEST(CodecJsonText, FindMemberOnNonObject) {
  EXPECT_EQ(FindJsonMember(Arr({Str("k")}), "k"), nullptr);
  const JsonValue o = Obj({{"k", Int(1)}, {"k", Int(2)}});
  ASSERT_NE(FindJsonMember(o, "k"), nullptr);
  EXPECT_EQ(FindJsonMember(o, "k")->i, 1);
}

}  // namespace
}  // namespace mybinlog
