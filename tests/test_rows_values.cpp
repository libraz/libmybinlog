// Non-temporal column values decoded through DecodeRowsEvent(): each case is
// a one-row, one-column WRITE_ROWS v2 event holding the value bytes.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "mybinlog/event_header.h"
#include "mybinlog/event_types.h"
#include "mybinlog/row_decoder.h"
#include "mybinlog/status.h"
#include "mybinlog/table_map.h"
#include "mybinlog/value.h"

namespace mybinlog {
namespace {

using Bytes = std::vector<uint8_t>;

constexpr uint32_t kTextCollation = 45;  // utf8mb4_general_ci
constexpr uint32_t kBinaryCollation = 63;

struct Writer {
  Bytes bytes;
  Writer& U8(uint64_t v) { return Le(v, 1); }
  Writer& Le(uint64_t v, int n) {
    for (int i = 0; i < n; ++i) bytes.push_back(static_cast<uint8_t>(v >> (8 * i)));
    return *this;
  }
  Writer& Be(uint64_t v, int n) {
    for (int i = n - 1; i >= 0; --i) bytes.push_back(static_cast<uint8_t>(v >> (8 * i)));
    return *this;
  }
  Writer& Str(const std::string& s) {
    bytes.insert(bytes.end(), s.begin(), s.end());
    return *this;
  }
  Writer& Raw(const Bytes& b) {
    bytes.insert(bytes.end(), b.begin(), b.end());
    return *this;
  }
};

TableMapColumn Column(ColumnType type, uint32_t meta = 0, bool is_unsigned = false) {
  TableMapColumn column;
  column.type = type;
  column.meta = meta;
  column.is_unsigned = is_unsigned;
  return column;
}

TableMapColumn WithCharset(TableMapColumn column, uint32_t collation) {
  column.has_charset = true;
  column.charset = collation;
  return column;
}

// Decodes @p value as the only column of a one-row WRITE_ROWS v2 event.
Status DecodeOne(const TableMapColumn& column, const Bytes& value, Value* out,
                 const DecodeOptions& options = DecodeOptions()) {
  TableMap table;
  table.table_id = 1;
  table.columns = {column};
  Writer w;
  w.Le(1, 6).Le(0, 2).Le(2, 2).U8(1).U8(0x01).U8(0x00).Raw(value);
  const Bytes body(w.bytes.begin(), w.bytes.end());  // snug, so an overread hits the redzone
  EventHeader header;
  header.type_code = static_cast<uint8_t>(EventType::kWriteRowsEvent);
  RowsEvent data;
  const Status status = DecodeRowsEvent(header, body.data(), body.size(), table, options, &data);
  if (status != Status::kOk) return status;
  if (data.rows.size() != 1 || data.rows[0].size() != 1) {
    ADD_FAILURE() << "the value did not consume exactly its own bytes";
    return Status::kMalformedValue;
  }
  *out = data.rows[0][0];
  return Status::kOk;
}

// --- Integers ---

TEST(CodecRowsValues, Integers) {
  struct Case {
    const char* label;
    ColumnType type;
    bool is_unsigned;
    Bytes wire;
    Value::Kind kind;
    int64_t i;
    uint64_t u;
  };
  const Case cases[] = {
      {"tiny 127", ColumnType::kTiny, false, {127}, Value::Kind::kInt, 127, 0},
      {"tiny -1", ColumnType::kTiny, false, {0xFF}, Value::Kind::kInt, -1, 0},
      {"tiny -128", ColumnType::kTiny, false, {0x80}, Value::Kind::kInt, -128, 0},
      {"tiny unsigned 255", ColumnType::kTiny, true, {0xFF}, Value::Kind::kUInt, 0, 255},
      {"tiny unsigned 0", ColumnType::kTiny, true, {0}, Value::Kind::kUInt, 0, 0},
      {"short -32768", ColumnType::kShort, false, {0x00, 0x80}, Value::Kind::kInt, -32768, 0},
      {"short unsigned 65535",
       ColumnType::kShort,
       true,
       {0xFF, 0xFF},
       Value::Kind::kUInt,
       0,
       65535},
      {"int24 -1", ColumnType::kInt24, false, {0xFF, 0xFF, 0xFF}, Value::Kind::kInt, -1, 0},
      {"int24 100", ColumnType::kInt24, false, {100, 0, 0}, Value::Kind::kInt, 100, 0},
      {"int24 unsigned max",
       ColumnType::kInt24,
       true,
       {0xFF, 0xFF, 0xFF},
       Value::Kind::kUInt,
       0,
       0xFFFFFF},
      {"long -100", ColumnType::kLong, false, Writer().Le(static_cast<uint32_t>(-100), 4).bytes,
       Value::Kind::kInt, -100, 0},
      {"long unsigned 4e9", ColumnType::kLong, true, Writer().Le(4000000000u, 4).bytes,
       Value::Kind::kUInt, 0, 4000000000u},
      {"longlong -1000", ColumnType::kLongLong, false,
       Writer().Le(static_cast<uint64_t>(-1000LL), 8).bytes, Value::Kind::kInt, -1000, 0},
      {"longlong min", ColumnType::kLongLong, false, Writer().Le(uint64_t{1} << 63, 8).bytes,
       Value::Kind::kInt, std::numeric_limits<int64_t>::min(), 0},
      {"longlong unsigned 9e18", ColumnType::kLongLong, true,
       Writer().Le(9000000000000000000ULL, 8).bytes, Value::Kind::kUInt, 0, 9000000000000000000ULL},
      {"longlong unsigned INT64_MAX", ColumnType::kLongLong, true,
       Writer().Le(static_cast<uint64_t>(INT64_MAX), 8).bytes, Value::Kind::kUInt, 0,
       static_cast<uint64_t>(INT64_MAX)},
      {"longlong unsigned INT64_MAX+1", ColumnType::kLongLong, true,
       Writer().Le(static_cast<uint64_t>(INT64_MAX) + 1, 8).bytes, Value::Kind::kUInt, 0,
       static_cast<uint64_t>(INT64_MAX) + 1},
      {"longlong unsigned max", ColumnType::kLongLong, true, Writer().Le(UINT64_MAX, 8).bytes,
       Value::Kind::kUInt, 0, UINT64_MAX},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(c.label);
    Value value;
    ASSERT_EQ(DecodeOne(Column(c.type, 0, c.is_unsigned), c.wire, &value), Status::kOk);
    EXPECT_EQ(value.kind, c.kind);
    EXPECT_EQ(value.type, c.type);
    if (c.kind == Value::Kind::kInt) {
      EXPECT_EQ(value.i, c.i);
    } else {
      EXPECT_EQ(value.u, c.u);
    }
  }
}

TEST(CodecRowsValues, FixedWidthValuesShortOfTheirWidthAreTruncated) {
  struct Case {
    ColumnType type;
    uint32_t meta;
    size_t available;
  };
  const Case cases[] = {
      {ColumnType::kTiny, 0, 0},      {ColumnType::kShort, 0, 1},    {ColumnType::kLong, 0, 3},
      {ColumnType::kLongLong, 0, 7},  {ColumnType::kInt24, 0, 2},    {ColumnType::kFloat, 4, 3},
      {ColumnType::kDouble, 8, 7},    {ColumnType::kYear, 0, 0},     {ColumnType::kDate, 0, 2},
      {ColumnType::kTimestamp, 0, 3}, {ColumnType::kBit, 0x0100, 0},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(static_cast<int>(c.type));
    Value value;
    EXPECT_EQ(DecodeOne(Column(c.type, c.meta), Bytes(c.available, 0), &value), Status::kTruncated);
  }
}

// --- FLOAT / DOUBLE: IEEE little-endian, whatever the host order ---

Bytes FloatBytes(float f) {
  uint32_t bits = 0;
  std::memcpy(&bits, &f, sizeof(bits));
  return Writer().Le(bits, 4).bytes;
}

Bytes DoubleBytes(double d) {
  uint64_t bits = 0;
  std::memcpy(&bits, &d, sizeof(bits));
  return Writer().Le(bits, 8).bytes;
}

TEST(CodecRowsValues, FloatAndDouble) {
  const float floats[] = {3.14f, 0.0f, -1.5f};
  for (float f : floats) {
    Value value;
    ASSERT_EQ(DecodeOne(Column(ColumnType::kFloat, 4), FloatBytes(f), &value), Status::kOk);
    EXPECT_EQ(value.kind, Value::Kind::kDouble);
    EXPECT_EQ(value.d, static_cast<double>(f));
  }
  const double doubles[] = {2.718281828, 0.0, -999.999, 1e300};
  for (double d : doubles) {
    Value value;
    ASSERT_EQ(DecodeOne(Column(ColumnType::kDouble, 8), DoubleBytes(d), &value), Status::kOk);
    EXPECT_EQ(value.kind, Value::Kind::kDouble);
    EXPECT_EQ(value.d, d);
  }
  // The wire byte order is fixed: 1.0 is 00 00 80 3F.
  Value one;
  ASSERT_EQ(DecodeOne(Column(ColumnType::kFloat, 4), {0x00, 0x00, 0x80, 0x3F}, &one), Status::kOk);
  EXPECT_EQ(one.d, 1.0);
}

TEST(CodecRowsValues, NanAndInfinityPassThrough) {
  Value value;
  ASSERT_EQ(DecodeOne(Column(ColumnType::kDouble, 8),
                      DoubleBytes(std::numeric_limits<double>::quiet_NaN()), &value),
            Status::kOk);
  EXPECT_EQ(value.kind, Value::Kind::kDouble);
  EXPECT_TRUE(std::isnan(value.d));
  ASSERT_EQ(DecodeOne(Column(ColumnType::kDouble, 8),
                      DoubleBytes(-std::numeric_limits<double>::infinity()), &value),
            Status::kOk);
  EXPECT_TRUE(std::isinf(value.d));
  EXPECT_LT(value.d, 0);
  ASSERT_EQ(DecodeOne(Column(ColumnType::kFloat, 4),
                      FloatBytes(std::numeric_limits<float>::infinity()), &value),
            Status::kOk);
  EXPECT_TRUE(std::isinf(value.d));
}

// --- DECIMAL ---

// decimal2bin for a decimal string within (precision, scale): nine-digit groups
// big-endian, the top bit inverted, every byte complemented when negative.
Bytes EncodeDecimal(const std::string& text, int precision, int scale) {
  static constexpr int kDigitBytes[] = {0, 1, 1, 2, 2, 3, 3, 4, 4, 4};
  const bool negative = !text.empty() && text[0] == '-';
  const std::string body = negative ? text.substr(1) : text;
  const size_t dot = body.find('.');
  std::string int_digits = body.substr(0, dot);
  std::string frac_digits = dot == std::string::npos ? "" : body.substr(dot + 1);
  const int intg = precision - scale;
  while (!int_digits.empty() && int_digits[0] == '0') int_digits.erase(0, 1);
  int_digits.insert(0, static_cast<size_t>(intg) - int_digits.size(), '0');
  frac_digits.append(static_cast<size_t>(scale) - frac_digits.size(), '0');

  Writer w;
  const auto put = [&w](const std::string& digits) {
    if (digits.empty()) return;
    const int bytes = kDigitBytes[digits.size()];
    w.Be(std::stoull(digits), digits.size() == 9 ? 4 : bytes);
  };
  put(int_digits.substr(0, static_cast<size_t>(intg % 9)));
  for (int i = intg % 9; i < intg; i += 9) put(int_digits.substr(static_cast<size_t>(i), 9));
  for (int i = 0; i + 9 <= scale; i += 9) put(frac_digits.substr(static_cast<size_t>(i), 9));
  put(frac_digits.substr(static_cast<size_t>(scale / 9 * 9)));

  Bytes out = w.bytes;
  out[0] ^= 0x80;
  if (negative) {
    for (uint8_t& b : out) b ^= 0xFF;
  }
  return out;
}

uint32_t DecimalMeta(uint32_t precision, uint32_t scale) { return precision << 8 | scale; }

TEST(CodecRowsValues, DecimalText) {
  struct Case {
    std::string text;
    int precision;
    int scale;
  };
  const Case cases[] = {
      {"12345678.90", 10, 2},
      {"-123.45", 5, 2},
      {"12345", 5, 0},
      {"-1.50", 5, 2},
      {"1234.56", 15, 2},
      {"1234", 10, 0},
      {"0.00", 15, 2},
      {"-0.00", 10, 2},  // a legal wire value, kept as written
      {"-12345678901234567890.0123456789", 30, 10},
      {"0.0000000000", 30, 10},
      {"99999999.99", 10, 2},
      {"9.99", 3, 2},
      {"-99999999999999999999999999999999999999999999999999999999999999999", 65, 0},
      {"0.12345678901234567890123456789012345678", 38, 38},
      {"1.12345678901234567890123456789012345678", 39, 38},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(c.text);
    const Bytes wire = EncodeDecimal(c.text, c.precision, c.scale);
    Value value;
    ASSERT_EQ(
        DecodeOne(Column(ColumnType::kNewDecimal, DecimalMeta(static_cast<uint32_t>(c.precision),
                                                              static_cast<uint32_t>(c.scale))),
                  wire, &value),
        Status::kOk);
    EXPECT_EQ(value.kind, Value::Kind::kDecimal);
    EXPECT_EQ(value.bytes, c.text);
  }
  // Hand-encoded vectors, independent of EncodeDecimal.
  Value value;
  ASSERT_EQ(DecodeOne(Column(ColumnType::kNewDecimal, 0x0502), {0x7F, 0xFE, 0xCD}, &value),
            Status::kOk);
  EXPECT_EQ(value.bytes, "-1.50");
  ASSERT_EQ(
      DecodeOne(Column(ColumnType::kNewDecimal, 0x0A02), {0x7F, 0xFF, 0xFF, 0xFF, 0xFF}, &value),
      Status::kOk);
  EXPECT_EQ(value.bytes, "-0.00");
  ASSERT_EQ(
      DecodeOne(Column(ColumnType::kNewDecimal, 0x0A02), {0x80, 0x00, 0x00, 0x00, 0x00}, &value),
      Status::kOk);
  EXPECT_EQ(value.bytes, "0.00");
}

TEST(CodecRowsValues, DecimalMetadataBounds) {
  // precision 1..65 and scale <= min(precision, 38); 0, 66 and 39 are refused.
  const uint32_t invalid[] = {DecimalMeta(0, 0), DecimalMeta(66, 0), DecimalMeta(40, 39),
                              DecimalMeta(5, 6), DecimalMeta(255, 0)};
  for (uint32_t meta : invalid) {
    SCOPED_TRACE(meta);
    Value value;
    EXPECT_EQ(DecodeOne(Column(ColumnType::kNewDecimal, meta), Bytes(64, 0x80), &value),
              Status::kInvalidMetadata);
  }
  Value value;
  EXPECT_EQ(DecodeOne(Column(ColumnType::kNewDecimal, DecimalMeta(65, 30)),
                      EncodeDecimal("1", 65, 30), &value),
            Status::kOk);
  EXPECT_EQ(DecodeOne(Column(ColumnType::kNewDecimal, DecimalMeta(38, 38)),
                      EncodeDecimal("0", 38, 38), &value),
            Status::kOk);
  EXPECT_EQ(DecodeOne(Column(ColumnType::kNewDecimal, DecimalMeta(1, 0)), EncodeDecimal("7", 1, 0),
                      &value),
            Status::kOk);
  EXPECT_EQ(value.bytes, "7");
}

TEST(CodecRowsValues, DecimalDigitGroupPastItsDigitCountIsMalformed) {
  struct Case {
    const char* label;
    Bytes wire;
    uint32_t meta;
  };
  const Case cases[] = {
      {"full group 10^9", {0xBB, 0x9A, 0xCA, 0x00}, DecimalMeta(9, 0)},
      {"second group all ones",
       {0x80, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF},
       DecimalMeta(18, 0)},
      {"integer remainder 10", {0x8A, 0x00}, DecimalMeta(3, 2)},
      {"fraction remainder 100", {0x80, 0x64}, DecimalMeta(3, 2)},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(c.label);
    Value value;
    EXPECT_EQ(DecodeOne(Column(ColumnType::kNewDecimal, c.meta), c.wire, &value),
              Status::kMalformedValue);
  }
  Value value;
  ASSERT_EQ(DecodeOne(Column(ColumnType::kNewDecimal, DecimalMeta(3, 2)), {0x89, 0x63}, &value),
            Status::kOk);
  EXPECT_EQ(value.bytes, "9.99");
}

TEST(CodecRowsValues, TruncatedDecimal) {
  Value value;
  EXPECT_EQ(DecodeOne(Column(ColumnType::kNewDecimal, 0x0A02), {0x80, 0x00}, &value),
            Status::kTruncated);
}

// --- Character columns: text or bytes by the TABLE_MAP collation ---

TEST(CodecRowsValues, VarcharPrefixWidthFollowsTheDeclaredLength) {
  Value value;
  ASSERT_EQ(DecodeOne(WithCharset(Column(ColumnType::kVarchar, 100), kTextCollation),
                      Writer().U8(5).Str("hello").bytes, &value),
            Status::kOk);
  EXPECT_EQ(value.kind, Value::Kind::kText);
  EXPECT_EQ(value.bytes, "hello");

  ASSERT_EQ(DecodeOne(WithCharset(Column(ColumnType::kVarchar, 500), kTextCollation),
                      Writer().Le(3, 2).Str("abc").bytes, &value),
            Status::kOk);
  EXPECT_EQ(value.bytes, "abc");

  ASSERT_EQ(DecodeOne(WithCharset(Column(ColumnType::kVarchar, 255), kTextCollation),
                      Writer().U8(0).bytes, &value),
            Status::kOk);
  EXPECT_EQ(value.kind, Value::Kind::kText);
  EXPECT_TRUE(value.bytes.empty());
}

TEST(CodecRowsValues, CharAndVarStringUseTheFoldedLength) {
  struct Case {
    const char* label;
    ColumnType type;
    uint32_t meta;
    Bytes wire;
    std::string expected;
  };
  const Case cases[] = {
      // CHAR(10): byte 0 is STRING unmodified, length 10, one-byte prefix.
      {"CHAR(10)", ColumnType::kString, 0xFE0A, Writer().U8(5).Str("hello").bytes, "hello"},
      // 0xFE3F folds to a maximum of 63: one-byte prefix.
      {"CHAR max 63", ColumnType::kString, 0xFE3F, Writer().U8(5).Str("ABCDE").bytes, "ABCDE"},
      // CHAR(100) utf8mb4 = 400 bytes: byte 0 = 0xFE ^ 0x10, so a two-byte prefix.
      {"CHAR 400 bytes", ColumnType::kString, 0xEE90, Writer().Le(7, 2).Str("testing").bytes,
       "testing"},
      // VAR_STRING metadata is written by the same function as CHAR.
      {"VAR_STRING", ColumnType::kVarString, 0xFE64, Writer().U8(3).Str("abc").bytes, "abc"},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(c.label);
    Value value;
    ASSERT_EQ(DecodeOne(WithCharset(Column(c.type, c.meta), kTextCollation), c.wire, &value),
              Status::kOk);
    EXPECT_EQ(value.kind, Value::Kind::kText);
    EXPECT_EQ(value.type, c.type);
    EXPECT_EQ(value.bytes, c.expected);
  }
}

TEST(CodecRowsValues, CharsetSelectsTextOrBytes) {
  const Bytes varchar = {2, 0xFF, 0xFE};
  Value value;
  ASSERT_EQ(
      DecodeOne(WithCharset(Column(ColumnType::kVarchar, 100), kBinaryCollation), varchar, &value),
      Status::kOk);
  EXPECT_EQ(value.kind, Value::Kind::kBytes);
  EXPECT_EQ(value.bytes, std::string("\xFF\xFE", 2));

  ASSERT_EQ(DecodeOne(Column(ColumnType::kVarchar, 100), varchar, &value), Status::kOk);
  EXPECT_EQ(value.kind, Value::Kind::kBytes);  // no collation: bytes

  ASSERT_EQ(
      DecodeOne(WithCharset(Column(ColumnType::kBlob, 1), kTextCollation), {2, 'o', 'k'}, &value),
      Status::kOk);
  EXPECT_EQ(value.kind, Value::Kind::kText);
  EXPECT_EQ(value.bytes, "ok");
}

TEST(CodecRowsValues, BlobFamilyLengthPrefixes) {
  struct Case {
    ColumnType type;
    uint32_t width;
    std::string payload;
  };
  const Case cases[] = {
      {ColumnType::kBlob, 1, "abc"},      {ColumnType::kBlob, 2, "hello"},
      {ColumnType::kBlob, 3, "abc"},      {ColumnType::kBlob, 4, "test"},
      {ColumnType::kTinyBlob, 1, "ab"},   {ColumnType::kMediumBlob, 3, "abc"},
      {ColumnType::kLongBlob, 4, "abcd"},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(testing::Message() << static_cast<int>(c.type) << "/" << c.width);
    Value value;
    ASSERT_EQ(
        DecodeOne(Column(c.type, c.width),
                  Writer().Le(c.payload.size(), static_cast<int>(c.width)).Str(c.payload).bytes,
                  &value),
        Status::kOk);
    EXPECT_EQ(value.kind, Value::Kind::kBytes);
    EXPECT_EQ(value.bytes, c.payload);
  }
}

TEST(CodecRowsValues, LengthPrefixWidthOutsideOneToFourIsInvalidMetadata) {
  const ColumnType types[] = {ColumnType::kBlob,       ColumnType::kTinyBlob,
                              ColumnType::kMediumBlob, ColumnType::kLongBlob,
                              ColumnType::kJson,       ColumnType::kGeometry,
                              ColumnType::kVector,     ColumnType::kBlobCompressed};
  for (ColumnType type : types) {
    for (uint32_t width : {0u, 5u}) {
      SCOPED_TRACE(testing::Message() << static_cast<int>(type) << " width " << width);
      Value value;
      EXPECT_EQ(DecodeOne(Column(type, width), {2, 0, 0, 0, 0, 'a', 'b'}, &value),
                Status::kInvalidMetadata);
    }
  }
}

TEST(CodecRowsValues, LengthPrefixPastTheEndIsTruncated) {
  Value value;
  EXPECT_EQ(DecodeOne(Column(ColumnType::kBlob, 4), {0x05, 0x00}, &value), Status::kTruncated);
  // VECTOR declaring three float32 elements with one present.
  EXPECT_EQ(
      DecodeOne(Column(ColumnType::kVector, 4), Writer().Le(12, 4).Le(0x3F800000, 4).bytes, &value),
      Status::kTruncated);
  // A four-byte length near UINT32_MAX must not wrap.
  EXPECT_EQ(DecodeOne(Column(ColumnType::kLongBlob, 4), Writer().Le(0xFFFFFFFF, 4).Str("x").bytes,
                      &value),
            Status::kTruncated);
}

// --- Opaque payloads: JSON (binary, not rendered), GEOMETRY, VECTOR, TYPED_ARRAY ---

TEST(CodecRowsValues, OpaquePayloadsAreRawBytes) {
  for (uint32_t width = 1; width <= 4; ++width) {
    for (ColumnType type : {ColumnType::kJson, ColumnType::kGeometry}) {
      SCOPED_TRACE(testing::Message() << static_cast<int>(type) << "/" << width);
      Value value;
      ASSERT_EQ(DecodeOne(Column(type, width),
                          Writer().Le(3, static_cast<int>(width)).Raw({1, 2, 3}).bytes, &value),
                Status::kOk);
      EXPECT_EQ(value.kind, Value::Kind::kBytes);
      EXPECT_EQ(value.bytes, std::string("\x01\x02\x03", 3));
    }
  }

  // A JSON column holds MySQL binary JSON even when TABLE_MAP gives it a charset.
  Value json;
  ASSERT_EQ(DecodeOne(WithCharset(Column(ColumnType::kJson, 4), kTextCollation),
                      Writer().Le(2, 4).Str("{}").bytes, &json),
            Status::kOk);
  EXPECT_EQ(json.kind, Value::Kind::kBytes);
  EXPECT_EQ(json.type, ColumnType::kJson);

  const float elements[] = {1.0f, -2.5f, 3.25f};
  Bytes raw(sizeof(elements));
  std::memcpy(raw.data(), elements, sizeof(elements));
  for (bool binary_charset : {false, true}) {
    TableMapColumn vector = Column(ColumnType::kVector, 4);
    if (binary_charset) vector = WithCharset(vector, kBinaryCollation);
    Value value;
    ASSERT_EQ(DecodeOne(vector, Writer().Le(raw.size(), 4).Raw(raw).bytes, &value), Status::kOk);
    EXPECT_EQ(value.kind, Value::Kind::kBytes);
    EXPECT_EQ(value.type, ColumnType::kVector);
    EXPECT_EQ(value.bytes, std::string(raw.begin(), raw.end()));
  }

  // TYPED_ARRAY: the metadata describes the element; the value is JSON-framed.
  TableMapColumn typed = Column(ColumnType::kTypedArray, 0);
  typed.element_type = ColumnType::kLongLong;
  const Bytes binary_json = {0x02, 0x01, 0x00, 0x05, 0x2A};
  Value value;
  ASSERT_EQ(DecodeOne(typed, Writer().Le(binary_json.size(), 4).Raw(binary_json).bytes, &value),
            Status::kOk);
  EXPECT_EQ(value.kind, Value::Kind::kBytes);
  EXPECT_EQ(value.type, ColumnType::kTypedArray);
  EXPECT_EQ(value.bytes, std::string(binary_json.begin(), binary_json.end()));
}

TEST(CodecRowsValues, LargePayloadsKeepTheirExactBytes) {
  constexpr size_t kPayload = 4 * 1024 * 1024;
  Writer w;
  w.Le(kPayload, 4);
  for (size_t i = 0; i < kPayload; ++i) w.bytes.push_back(static_cast<uint8_t>((i * 31u) & 0xFF));
  for (ColumnType type : {ColumnType::kBlob, ColumnType::kJson, ColumnType::kGeometry}) {
    Value value;
    ASSERT_EQ(DecodeOne(Column(type, 4), w.bytes, &value), Status::kOk);
    ASSERT_EQ(value.bytes.size(), kPayload);
    EXPECT_EQ(std::memcmp(value.bytes.data(), w.bytes.data() + 4, kPayload), 0);
  }
}

// --- ENUM / SET / BIT ---

TEST(CodecRowsValues, EnumAndSetAreOrdinalAndBitmask) {
  struct Case {
    const char* label;
    ColumnType type;
    uint32_t meta;
    Bytes wire;
    Value::Kind kind;
    ColumnType real;
    uint64_t u;
  };
  const Case cases[] = {
      {"ENUM 1 byte", ColumnType::kString, 0xF701, {3}, Value::Kind::kEnum, ColumnType::kEnum, 3},
      {"ENUM 0 (empty value)",
       ColumnType::kString,
       0xF701,
       {0},
       Value::Kind::kEnum,
       ColumnType::kEnum,
       0},
      {"ENUM 2 bytes", ColumnType::kString, 0xF702, Writer().Le(300, 2).bytes, Value::Kind::kEnum,
       ColumnType::kEnum, 300},
      {"SET 2 bytes", ColumnType::kString, 0xF802, Writer().Le(5, 2).bytes, Value::Kind::kSet,
       ColumnType::kSet, 5},
      {"SET 8 bytes bit 63", ColumnType::kString, 0xF808, Writer().Le(uint64_t{1} << 63, 8).bytes,
       Value::Kind::kSet, ColumnType::kSet, uint64_t{1} << 63},
      // A bit beyond the label count is returned as is; labels are the caller's.
      {"SET 1 byte all bits",
       ColumnType::kString,
       0xF801,
       {0xFF},
       Value::Kind::kSet,
       ColumnType::kSet,
       0xFF},
      // The ENUM/SET type codes themselves carry the same metadata.
      {"ENUM type code", ColumnType::kEnum, 0xF701, {3}, Value::Kind::kEnum, ColumnType::kEnum, 3},
      {"SET type code", ColumnType::kSet, 0xF804, Writer().Le(0x12345678, 4).bytes,
       Value::Kind::kSet, ColumnType::kSet, 0x12345678},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(c.label);
    Value value;
    ASSERT_EQ(DecodeOne(Column(c.type, c.meta), c.wire, &value), Status::kOk);
    EXPECT_EQ(value.kind, c.kind);
    EXPECT_EQ(value.type, c.real);
    EXPECT_EQ(value.u, c.u);
  }
}

TEST(CodecRowsValues, EnumAndSetSizesOutsideTheirRangeAreInvalidMetadata) {
  const uint32_t metas[] = {0xF700, 0xF703, 0xF800, 0xF809};
  for (uint32_t meta : metas) {
    SCOPED_TRACE(meta);
    Value value;
    EXPECT_EQ(DecodeOne(Column(ColumnType::kString, meta), Bytes(9, 0), &value),
              Status::kInvalidMetadata);
  }
}

TEST(CodecRowsValues, BitIsBigEndian) {
  struct Case {
    uint32_t meta;  // bytes << 8 | bits
    Bytes wire;
    uint64_t expected;
  };
  const Case cases[] = {
      {0x0001, {1}, 1},
      {0x0200, {0x01, 0x02}, 0x0102},
      {0x0100, {0xAB}, 0xAB},
      {0x0102, {0x02, 0xAA}, 682},  // BIT(10) b'1010101010'
      {0x0800, {0x80, 0, 0, 0, 0, 0, 0, 0}, uint64_t{1} << 63},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(c.meta);
    Value value;
    ASSERT_EQ(DecodeOne(Column(ColumnType::kBit, c.meta), c.wire, &value), Status::kOk);
    EXPECT_EQ(value.kind, Value::Kind::kBit);
    EXPECT_EQ(value.u, c.expected);
  }
}

TEST(CodecRowsValues, BitWiderThanSixtyFourBitsIsInvalidMetadata) {
  // Reading these would keep 8 bytes and consume more, shifting every later column.
  Bytes wire(256);
  for (size_t i = 0; i < wire.size(); ++i) wire[i] = static_cast<uint8_t>(i + 1);
  for (uint32_t meta : {0x0900u, 0x0801u, 0xFF07u, 0x0000u}) {
    SCOPED_TRACE(meta);
    Value value;
    EXPECT_EQ(DecodeOne(Column(ColumnType::kBit, meta), wire, &value), Status::kInvalidMetadata);
  }
}

// --- Types the codec does not decode ---

TEST(CodecRowsValues, UndecodableTypesAreUnsupported) {
  Value value;
  EXPECT_EQ(DecodeOne(Column(ColumnType::kDecimal), {0xAA}, &value), Status::kUnsupported);
  EXPECT_EQ(DecodeOne(Column(static_cast<ColumnType>(0x06)), {0xAA}, &value),
            Status::kUnsupported);  // MYSQL_TYPE_NULL
  EXPECT_EQ(DecodeOne(Column(static_cast<ColumnType>(0x15)), {0xAA}, &value), Status::kUnsupported);
}

TEST(CodecRowsValues, MaxValueBytesIsAPerValueCeiling) {
  DecodeOptions options;
  options.max_value_bytes = 4;
  Value value;
  EXPECT_EQ(
      DecodeOne(Column(ColumnType::kBlob, 1), Writer().U8(4).Str("abcd").bytes, &value, options),
      Status::kOk);
  EXPECT_EQ(
      DecodeOne(Column(ColumnType::kBlob, 1), Writer().U8(5).Str("abcde").bytes, &value, options),
      Status::kLimitExceeded);
  EXPECT_EQ(DecodeOne(Column(ColumnType::kNewDecimal, DecimalMeta(10, 2)),
                      EncodeDecimal("123.45", 10, 2), &value, options),
            Status::kLimitExceeded);
}

}  // namespace
}  // namespace mybinlog
