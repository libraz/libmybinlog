// Binary JSON decoding. Documents are built with a test encoder that lays
// bytes out as json_binary::serialize does (small format first, large on
// overflow, inlined literals and 16-bit ints, 32-bit ints inlined only in the
// large format, keys sorted by length then bytes). Vectors and expected text
// come from MySQL's unittest/gunit/json_binary-t.cc and json_dom-t.cc.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "mybinlog/json.h"

namespace mybinlog {
namespace {

using Bytes = std::vector<uint8_t>;
using Kind = JsonValue::Kind;

// ---- DOM builders --------------------------------------------------------

JsonValue Null() { return JsonValue(); }

JsonValue Bool(bool b) {
  JsonValue v;
  v.kind = Kind::kBoolean;
  v.b = b;
  return v;
}

JsonValue Int(int64_t i) {
  JsonValue v;
  v.kind = Kind::kInt;
  v.i = i;
  return v;
}

JsonValue UInt(uint64_t u) {
  JsonValue v;
  v.kind = Kind::kUInt;
  v.u = u;
  return v;
}

JsonValue Dbl(double d) {
  JsonValue v;
  v.kind = Kind::kDouble;
  v.d = d;
  return v;
}

JsonValue Str(std::string s) {
  JsonValue v;
  v.kind = Kind::kString;
  v.bytes = std::move(s);
  return v;
}

JsonValue Opaque(ColumnType type, std::string bytes) {
  JsonValue v;
  v.kind = Kind::kOpaque;
  v.opaque_type = type;
  v.bytes = std::move(bytes);
  return v;
}

JsonValue Arr(std::vector<JsonValue> elements) {
  JsonValue v;
  v.kind = Kind::kArray;
  v.elements = std::move(elements);
  return v;
}

// Json_object: keys ordered by length, then bytes; a repeated key keeps the last value.
JsonValue Obj(std::vector<std::pair<std::string, JsonValue>> members) {
  JsonValue v;
  v.kind = Kind::kObject;
  for (auto& member : members) {
    const std::string& key = member.first;
    auto same = std::find_if(v.members.begin(), v.members.end(),
                             [&key](const JsonMember& m) { return m.key == key; });
    if (same != v.members.end()) {
      same->value = std::move(member.second);
    } else {
      v.members.push_back(JsonMember{key, std::move(member.second)});
    }
  }
  std::stable_sort(v.members.begin(), v.members.end(),
                   [](const JsonMember& a, const JsonMember& b) {
                     if (a.key.size() != b.key.size()) return a.key.size() < b.key.size();
                     return a.key < b.key;
                   });
  return v;
}

// TIME_to_longlong_*_packed, stored little-endian (Json_datetime::to_packed).
std::string Packed(int64_t packed) {
  std::string out(8, '\0');
  for (int i = 0; i < 8; ++i) out[i] = static_cast<char>(static_cast<uint64_t>(packed) >> (8 * i));
  return out;
}

int64_t PackedDatetime(int year, int month, int day, int hour, int minute, int second, int micros) {
  const int64_t ymd = ((int64_t{year} * 13 + month) << 5) | day;
  const int64_t hms = (int64_t{hour} << 12) | (minute << 6) | second;
  return (((ymd << 17) | hms) << 24) + micros;
}

int64_t PackedTime(bool negative, int hour, int minute, int second, int micros) {
  const int64_t hms = (int64_t{hour} << 12) | (minute << 6) | second;
  const int64_t packed = (hms << 24) + micros;
  return negative ? -packed : packed;
}

JsonValue Datetime(ColumnType type, int64_t packed) { return Opaque(type, Packed(packed)); }

// Json_decimal binary: precision, scale, then bin2decimal bytes.
JsonValue Decimal(uint8_t precision, uint8_t scale, Bytes bin) {
  std::string bytes;
  bytes.push_back(static_cast<char>(precision));
  bytes.push_back(static_cast<char>(scale));
  bytes.append(bin.begin(), bin.end());
  return Opaque(ColumnType::kNewDecimal, bytes);
}

// ---- Encoder (json_binary::serialize) ------------------------------------

enum class Fit { kOk, kTooBig };

bool TooBig(size_t value, bool large) {
  return value > (large ? uint64_t{UINT32_MAX} : uint64_t{UINT16_MAX});
}

void PutOffset(Bytes* dest, size_t pos, size_t value, bool large) {
  for (size_t i = 0; i < (large ? 4u : 2u); ++i)
    (*dest)[pos + i] = static_cast<uint8_t>(value >> (8 * i));
}

void AppendOffset(Bytes* dest, size_t value, bool large) {
  dest->resize(dest->size() + (large ? 4 : 2));
  PutOffset(dest, dest->size() - (large ? 4 : 2), value, large);
}

void AppendVariableLength(Bytes* dest, size_t length) {
  do {
    uint8_t byte = length & 0x7F;
    length >>= 7;
    if (length != 0) byte |= 0x80;
    dest->push_back(byte);
  } while (length != 0);
}

bool Is16(const JsonValue& v) {
  return v.kind == Kind::kInt ? v.i >= INT16_MIN && v.i <= INT16_MAX : v.u <= UINT16_MAX;
}

bool Is32(const JsonValue& v) {
  return v.kind == Kind::kInt ? v.i >= INT32_MIN && v.i <= INT32_MAX : v.u <= UINT32_MAX;
}

bool AttemptInline(const JsonValue& v, Bytes* dest, size_t pos, bool large) {
  uint8_t type = 0;
  uint32_t value = 0;
  switch (v.kind) {
    case Kind::kNull:
      type = 0x04;
      value = 0x00;
      break;
    case Kind::kBoolean:
      type = 0x04;
      value = v.b ? 0x01 : 0x02;
      break;
    case Kind::kInt:
    case Kind::kUInt:
      if (!Is16(v) && !(large && Is32(v))) return false;
      type = v.kind == Kind::kInt ? (Is16(v) ? 0x05 : 0x07) : (Is16(v) ? 0x06 : 0x08);
      value = v.kind == Kind::kInt ? static_cast<uint32_t>(v.i) : static_cast<uint32_t>(v.u);
      break;
    default:
      return false;
  }
  (*dest)[pos] = type;
  PutOffset(dest, pos + 1, large ? value : (value & 0xFFFF), large);
  return true;
}

Fit SerializeValue(const JsonValue& v, size_t type_pos, bool small_parent, Bytes* dest);

Fit SerializeContainer(const JsonValue& v, bool large, Bytes* dest) {
  const bool object = v.kind == Kind::kObject;
  const size_t start = dest->size();
  const size_t count = object ? v.members.size() : v.elements.size();
  if (TooBig(count, large)) return Fit::kTooBig;
  AppendOffset(dest, count, large);
  const size_t size_pos = dest->size();
  AppendOffset(dest, 0, large);
  const size_t key_entry = large ? 6 : 4;
  const size_t value_entry = large ? 5 : 3;
  if (object) {
    size_t key_offset = dest->size() + count * (key_entry + value_entry) - start;
    for (const JsonMember& m : v.members) {
      if (TooBig(key_offset, large)) return Fit::kTooBig;
      AppendOffset(dest, key_offset, large);
      dest->push_back(static_cast<uint8_t>(m.key.size()));
      dest->push_back(static_cast<uint8_t>(m.key.size() >> 8));
      key_offset += m.key.size();
    }
  }
  size_t entry = dest->size();
  dest->resize(dest->size() + count * value_entry, 0);
  if (object) {
    for (const JsonMember& m : v.members) dest->insert(dest->end(), m.key.begin(), m.key.end());
  }
  for (size_t i = 0; i < count; ++i) {
    const JsonValue& child = object ? v.members[i].value : v.elements[i];
    if (!AttemptInline(child, dest, entry, large)) {
      const size_t offset = dest->size() - start;
      if (TooBig(offset, large)) return Fit::kTooBig;
      PutOffset(dest, entry + 1, offset, large);
      if (SerializeValue(child, entry, !large, dest) != Fit::kOk) return Fit::kTooBig;
    }
    entry += value_entry;
  }
  const size_t bytes = dest->size() - start;
  if (TooBig(bytes, large)) return Fit::kTooBig;
  PutOffset(dest, size_pos, bytes, large);
  return Fit::kOk;
}

Fit SerializeValue(const JsonValue& v, size_t type_pos, bool small_parent, Bytes* dest) {
  const size_t start = dest->size();
  switch (v.kind) {
    case Kind::kArray:
    case Kind::kObject: {
      const bool object = v.kind == Kind::kObject;
      (*dest)[type_pos] = object ? 0x00 : 0x02;
      if (SerializeContainer(v, false, dest) == Fit::kOk) return Fit::kOk;
      if (small_parent) return Fit::kTooBig;
      dest->resize(start);
      (*dest)[type_pos] = object ? 0x01 : 0x03;
      return SerializeContainer(v, true, dest);
    }
    case Kind::kString:
      (*dest)[type_pos] = 0x0C;
      AppendVariableLength(dest, v.bytes.size());
      dest->insert(dest->end(), v.bytes.begin(), v.bytes.end());
      return Fit::kOk;
    case Kind::kInt:
    case Kind::kUInt: {
      const bool is_signed = v.kind == Kind::kInt;
      const size_t width = Is16(v) ? 2 : Is32(v) ? 4 : 8;
      (*dest)[type_pos] = static_cast<uint8_t>(width == 2   ? (is_signed ? 0x05 : 0x06)
                                               : width == 4 ? (is_signed ? 0x07 : 0x08)
                                                            : (is_signed ? 0x09 : 0x0A));
      const uint64_t bits = is_signed ? static_cast<uint64_t>(v.i) : v.u;
      for (size_t i = 0; i < width; ++i) dest->push_back(static_cast<uint8_t>(bits >> (8 * i)));
      return Fit::kOk;
    }
    case Kind::kDouble: {
      (*dest)[type_pos] = 0x0B;
      uint64_t bits = 0;
      std::memcpy(&bits, &v.d, sizeof(bits));
      for (size_t i = 0; i < 8; ++i) dest->push_back(static_cast<uint8_t>(bits >> (8 * i)));
      return Fit::kOk;
    }
    case Kind::kNull:
    case Kind::kBoolean:
      (*dest)[type_pos] = 0x04;
      dest->push_back(v.kind == Kind::kNull ? 0x00 : v.b ? 0x01 : 0x02);
      return Fit::kOk;
    case Kind::kOpaque:
      (*dest)[type_pos] = 0x0F;
      dest->push_back(static_cast<uint8_t>(v.opaque_type));
      AppendVariableLength(dest, v.bytes.size());
      dest->insert(dest->end(), v.bytes.begin(), v.bytes.end());
      return Fit::kOk;
  }
  return Fit::kTooBig;
}

Bytes Encode(const JsonValue& v) {
  Bytes out(1, 0);
  EXPECT_EQ(SerializeValue(v, 0, false, &out), Fit::kOk);
  return out;
}

std::string Text(const Bytes& binary) {
  std::string text;
  EXPECT_EQ(JsonBinaryToText(binary.data(), binary.size(), &text), Status::kOk);
  return text;
}

JsonValue Parse(const Bytes& binary) {
  JsonValue value;
  EXPECT_EQ(ParseJsonBinary(binary.data(), binary.size(), &value), Status::kOk);
  return value;
}

Status ParseStatus(const Bytes& binary) {
  JsonValue value;
  return ParseJsonBinary(binary.data(), binary.size(), &value);
}

// ---- Hand-laid bytes pin the encoder to the documented layout -------------

TEST(CodecJsonBinary, HandLaidSmallObject) {
  // {"a": 1}: count, size, key entry (offset 11, length 1), inlined int16, key.
  const Bytes doc = {0x00, 0x01, 0x00, 0x0C, 0x00, 0x0B, 0x00, 0x01, 0x00, 0x05, 0x01, 0x00, 0x61};
  EXPECT_EQ(Encode(Obj({{"a", Int(1)}})), doc);
  EXPECT_EQ(Text(doc), "{\"a\": 1}");
}

TEST(CodecJsonBinary, HandLaidSmallArray) {
  // [1, "x"]: inlined int16, then a string at offset 10.
  const Bytes doc = {0x02, 0x02, 0x00, 0x0C, 0x00, 0x05, 0x01, 0x00, 0x0C, 0x0A, 0x00, 0x01, 0x78};
  EXPECT_EQ(Encode(Arr({Int(1), Str("x")})), doc);
  EXPECT_EQ(Text(doc), "[1, \"x\"]");
}

TEST(CodecJsonBinary, HandLaidScalars) {
  EXPECT_EQ(Text({0x04, 0x00}), "null");
  EXPECT_EQ(Text({0x04, 0x01}), "true");
  EXPECT_EQ(Text({0x04, 0x02}), "false");
  EXPECT_EQ(Text({0x05, 0x00, 0x80}), "-32768");
  EXPECT_EQ(Text({0x06, 0xFF, 0xFF}), "65535");
  EXPECT_EQ(Text({0x07, 0x00, 0x00, 0x00, 0x80}), "-2147483648");
  EXPECT_EQ(Text({0x08, 0xFF, 0xFF, 0xFF, 0xFF}), "4294967295");
  EXPECT_EQ(Text({0x09, 0, 0, 0, 0, 0, 0, 0, 0x80}), "-9223372036854775808");
  EXPECT_EQ(Text({0x0A, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}), "18446744073709551615");
  EXPECT_EQ(Text({0x0B, 0, 0, 0, 0, 0, 0, 0xF8, 0x3F}), "1.5");
  EXPECT_EQ(Text({0x0C, 0x03, 'a', 'b', 'c'}), "\"abc\"");
  EXPECT_EQ(Text({0x0F, 0xFC, 0x04, 'a', 'b', 'c', 'd'}), "\"base64:type252:YWJjZA==\"");
}

TEST(CodecJsonBinary, EmptyDocumentIsNull) {
  // json_binary-t.cc EmptyDocument.
  JsonValue value = Bool(true);
  ASSERT_EQ(ParseJsonBinary(nullptr, 0, &value), Status::kOk);
  EXPECT_EQ(value.kind, Kind::kNull);
  EXPECT_EQ(Text({}), "null");
}

// ---- json_binary-t.cc BasicTest -------------------------------------------

TEST(CodecJsonUpstream, BasicScalars) {
  EXPECT_EQ(Text(Encode(Bool(false))), "false");
  JsonValue v = Parse(Encode(Int(-123)));
  EXPECT_EQ(v.kind, Kind::kInt);
  EXPECT_EQ(v.i, -123);
  EXPECT_EQ(Text(Encode(Int(-123))), "-123");
  v = Parse(Encode(Dbl(3.14)));
  EXPECT_EQ(v.kind, Kind::kDouble);
  EXPECT_EQ(v.d, 3.14);
  v = Parse(Encode(UInt(18446744073709551615ULL)));
  EXPECT_EQ(v.kind, Kind::kUInt);
  EXPECT_EQ(v.u, 18446744073709551615ULL);
  v = Parse(Encode(Str("abc")));
  EXPECT_EQ(v.kind, Kind::kString);
  EXPECT_EQ(v.bytes, "abc");
}

TEST(CodecJsonUpstream, BasicArrays) {
  const Bytes a = Encode(Arr({Int(1), Int(2), Int(3)}));
  EXPECT_EQ(a[0], 0x02);  // small
  const JsonValue v = Parse(a);
  ASSERT_EQ(v.kind, Kind::kArray);
  ASSERT_EQ(v.elements.size(), 3u);
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(v.elements[i].kind, Kind::kInt);
    EXPECT_EQ(v.elements[i].i, i + 1);
  }
  EXPECT_EQ(Text(a), "[1, 2, 3]");

  const Bytes nested = Encode(Arr({Int(1), Arr({Str("a"), Arr({Dbl(3.14)})})}));
  const JsonValue n = Parse(nested);
  ASSERT_EQ(n.elements.size(), 2u);
  ASSERT_EQ(n.elements[1].elements.size(), 2u);
  EXPECT_EQ(n.elements[1].elements[0].bytes, "a");
  EXPECT_EQ(n.elements[1].elements[1].elements[0].d, 3.14);
  EXPECT_EQ(Text(nested), "[1, [\"a\", [3.14]]]");
}

TEST(CodecJsonUpstream, BasicObjects) {
  const Bytes o = Encode(Obj({{"key", Str("val")}}));
  EXPECT_EQ(o[0], 0x00);  // small
  const JsonValue v = Parse(o);
  ASSERT_EQ(v.members.size(), 1u);
  EXPECT_EQ(v.members[0].key, "key");
  EXPECT_EQ(v.members[0].value.bytes, "val");
  ASSERT_NE(FindJsonMember(v, "key"), nullptr);
  EXPECT_EQ(FindJsonMember(v, "key")->bytes, "val");
  EXPECT_EQ(FindJsonMember(v, "no such key"), nullptr);
  EXPECT_EQ(Text(o), "{\"key\": \"val\"}");

  const Bytes o2 = Encode(Obj({{"a", Str("b")}, {"c", Arr({Str("d")})}}));
  const JsonValue v2 = Parse(o2);
  ASSERT_EQ(v2.members.size(), 2u);
  EXPECT_EQ(v2.members[1].key, "c");
  EXPECT_EQ(FindJsonMember(v2, "c")->elements[0].bytes, "d");
  EXPECT_EQ(Text(o2), "{\"a\": \"b\", \"c\": [\"d\"]}");
}

TEST(CodecJsonUpstream, BasicOpaqueTinyBlob) {
  const std::string blob("\xBE\xBA\xFE\xCA", 4);  // int4store(0xCAFEBABE)
  const JsonValue v = Parse(Encode(Opaque(ColumnType::kTinyBlob, blob)));
  ASSERT_EQ(v.kind, Kind::kOpaque);
  EXPECT_EQ(v.opaque_type, ColumnType::kTinyBlob);
  EXPECT_EQ(v.bytes, blob);
  // json_dom-t.cc: the same opaque formats as base64.
  EXPECT_EQ(Text(Encode(Opaque(ColumnType::kTinyBlob, blob))), "\"base64:type249:vrr+yg==\"");
}

TEST(CodecJsonUpstream, BasicMixedArray) {
  const JsonValue v = Parse(
      Encode(Arr({Bool(true), Bool(false), Null(), Int(0), Str("0"), Str(""), Obj({}), Arr({})})));
  ASSERT_EQ(v.elements.size(), 8u);
  const Kind kinds[] = {Kind::kBoolean, Kind::kBoolean, Kind::kNull,   Kind::kInt,
                        Kind::kString,  Kind::kString,  Kind::kObject, Kind::kArray};
  for (size_t i = 0; i < 8; ++i) EXPECT_EQ(v.elements[i].kind, kinds[i]) << i;
  EXPECT_TRUE(v.elements[0].b);
  EXPECT_FALSE(v.elements[1].b);
  EXPECT_EQ(v.elements[6].members.size(), 0u);
  EXPECT_EQ(v.elements[7].elements.size(), 0u);
  std::string text;
  ASSERT_EQ(JsonToText(v, &text), Status::kOk);
  EXPECT_EQ(text, "[true, false, null, 0, \"0\", \"\", {}, []]");
  EXPECT_EQ(Text(Encode(Obj({}))), "{}");
  EXPECT_EQ(Text(Encode(Arr({}))), "[]");
}

TEST(CodecJsonUpstream, BasicKeyOrderAndDuplicates) {
  const JsonValue doc = Obj({{"key1", Int(1)},
                             {"key2", Int(2)},
                             {"key1", Int(3)},
                             {std::string("key1\0x", 6), Int(4)},
                             {std::string("key1\0y", 6), Int(5)},
                             {"a", Int(6)},
                             {"ab", Int(7)},
                             {"b", Int(8)},
                             {"", Int(9)},
                             {"", Int(10)}});
  const std::string expected_keys[] = {
      "", "a", "b", "ab", "key1", "key2", std::string("key1\0x", 6), std::string("key1\0y", 6)};
  const int64_t expected_values[] = {10, 6, 8, 7, 3, 2, 4, 5};
  const JsonValue v = Parse(Encode(doc));
  ASSERT_EQ(v.members.size(), 8u);
  for (size_t i = 0; i < 8; ++i) {
    EXPECT_EQ(v.members[i].key, expected_keys[i]);
    EXPECT_EQ(v.members[i].value.i, expected_values[i]);
    ASSERT_NE(FindJsonMember(v, expected_keys[i]), nullptr);
    EXPECT_EQ(FindJsonMember(v, expected_keys[i])->i, expected_values[i]);
  }
  EXPECT_EQ(Text(Encode(doc)),
            "{\"\": 10, \"a\": 6, \"b\": 8, \"ab\": 7, \"key1\": 3, \"key2\": 2, "
            "\"key1\\u0000x\": 4, \"key1\\u0000y\": 5}");
}

TEST(CodecJsonUpstream, BasicDecimal) {
  // create_decimal(123.45): precision 5, scale 2.
  const JsonValue d = Decimal(5, 2, {0x80, 0x7B, 0x2D});
  const JsonValue v = Parse(Encode(d));
  EXPECT_EQ(v.kind, Kind::kOpaque);
  EXPECT_EQ(v.opaque_type, ColumnType::kNewDecimal);
  EXPECT_EQ(Text(Encode(d)), "123.45");
}

// ---- json_binary-t.cc DateAndTimeTest -------------------------------------

TEST(CodecJsonUpstream, DateAndTime) {
  const JsonValue array =
      Arr({Datetime(ColumnType::kTime, PackedTime(false, 13, 14, 15, 654321)),
           Datetime(ColumnType::kDate, PackedDatetime(2014, 5, 17, 0, 0, 0, 0)),
           Datetime(ColumnType::kDatetime, PackedDatetime(2015, 1, 15, 15, 16, 17, 123456))});
  const JsonValue v = Parse(Encode(array));
  ASSERT_EQ(v.elements.size(), 3u);
  EXPECT_EQ(v.elements[0].opaque_type, ColumnType::kTime);
  EXPECT_EQ(v.elements[0].bytes.size(), 8u);
  EXPECT_EQ(v.elements[1].opaque_type, ColumnType::kDate);
  EXPECT_EQ(v.elements[2].opaque_type, ColumnType::kDatetime);
  EXPECT_EQ(Text(Encode(array)),
            "[\"13:14:15.654321\", \"2014-05-17\", \"2015-01-15 15:16:17.123456\"]");
}

// ---- json_binary-t.cc LargeDocumentTest -----------------------------------

JsonValue LargeArray() {
  std::vector<JsonValue> elements;
  for (int i = 0; i < 20000; ++i) {
    elements.push_back(Bool(true));
    elements.push_back(Bool(false));
    elements.push_back(Null());
    elements.push_back(Str("a"));
  }
  return Arr(std::move(elements));
}

void ExpectLargeArrayContents(const JsonValue& array) {
  ASSERT_EQ(array.kind, Kind::kArray);
  ASSERT_EQ(array.elements.size(), 80000u);
  for (size_t i = 0; i < array.elements.size(); ++i) {
    const JsonValue& e = array.elements[i];
    switch (i % 4) {
      case 0:
        ASSERT_TRUE(e.kind == Kind::kBoolean && e.b) << i;
        break;
      case 1:
        ASSERT_TRUE(e.kind == Kind::kBoolean && !e.b) << i;
        break;
      case 2:
        ASSERT_EQ(e.kind, Kind::kNull) << i;
        break;
      default:
        ASSERT_TRUE(e.kind == Kind::kString && e.bytes == "a") << i;
        break;
    }
  }
}

std::string LargeArrayText() {
  std::string text = "[";
  for (int i = 0; i < 20000; ++i)
    text += i == 0 ? "true, false, null, \"a\"" : ", true, false, null, \"a\"";
  return text + "]";
}

TEST(CodecJsonUpstream, LargeArray) {
  const Bytes doc = Encode(LargeArray());
  EXPECT_EQ(doc[0], 0x03);  // large
  ExpectLargeArrayContents(Parse(doc));
  EXPECT_EQ(Text(doc), LargeArrayText());

  const Bytes twice = Encode(Arr({LargeArray(), LargeArray()}));
  EXPECT_EQ(twice[0], 0x03);
  const JsonValue v = Parse(twice);
  ASSERT_EQ(v.elements.size(), 2u);
  ExpectLargeArrayContents(v.elements[0]);
  ExpectLargeArrayContents(v.elements[1]);
}

TEST(CodecJsonUpstream, LargeObject) {
  const Bytes doc = Encode(Obj({{"a", LargeArray()}, {"b", Str("c")}}));
  EXPECT_EQ(doc[0], 0x01);  // large
  const JsonValue v = Parse(doc);
  ASSERT_EQ(v.members.size(), 2u);
  EXPECT_EQ(v.members[0].key, "a");
  ExpectLargeArrayContents(v.members[0].value);
  EXPECT_EQ(v.members[1].key, "b");
  EXPECT_EQ(FindJsonMember(v, "b")->bytes, "c");
  EXPECT_EQ(Text(doc), "{\"a\": " + LargeArrayText() + ", \"b\": \"c\"}");
}

TEST(CodecJsonUpstream, LargeDeeplyNested) {
  constexpr size_t kDepth = 50;
  JsonValue array = LargeArray();
  JsonValue object = LargeArray();
  for (size_t i = 0; i < kDepth; ++i) {
    array = Arr({std::move(array)});
    object = Obj({{"key", std::move(object)}});
  }
  JsonValue v = Parse(Encode(array));
  for (size_t i = 0; i < kDepth; ++i) {
    ASSERT_EQ(v.kind, Kind::kArray);
    ASSERT_EQ(v.elements.size(), 1u);
    JsonValue next = std::move(v.elements[0]);
    v = std::move(next);
  }
  ExpectLargeArrayContents(v);

  v = Parse(Encode(object));
  for (size_t i = 0; i < kDepth; ++i) {
    ASSERT_EQ(v.kind, Kind::kObject);
    ASSERT_EQ(v.members.size(), 1u);
    ASSERT_EQ(v.members[0].key, "key");
    JsonValue next = std::move(v.members[0].value);
    v = std::move(next);
  }
  ExpectLargeArrayContents(v);
}

TEST(CodecJsonBinary, Int32InlinedOnlyInLargeFormat) {
  // A large array (pushed over 64KB by a long string) inlines int32/uint32.
  const JsonValue doc = Arr({Str(std::string(64 * 1024, 'a')), Int(INT32_MIN), UInt(UINT32_MAX),
                             Int(INT16_MIN - 1), Int(int64_t{INT32_MAX} + 1)});
  const Bytes bytes = Encode(doc);
  EXPECT_EQ(bytes[0], 0x03);
  // Second value entry: type int32 inlined after the 1 + 8 header bytes and one 5-byte entry.
  EXPECT_EQ(bytes[1 + 8 + 5], 0x07);
  const JsonValue v = Parse(bytes);
  EXPECT_EQ(v.elements[1].i, INT32_MIN);
  EXPECT_EQ(v.elements[2].u, UINT32_MAX);
  EXPECT_EQ(v.elements[3].i, INT16_MIN - 1);
  EXPECT_EQ(v.elements[4].i, int64_t{INT32_MAX} + 1);

  // In a small array the same values are stored out of line.
  const Bytes small = Encode(Arr({Int(INT32_MIN), UInt(UINT32_MAX)}));
  EXPECT_EQ(small[0], 0x02);
  EXPECT_EQ(Text(small), "[-2147483648, 4294967295]");
}

// ---- json_binary-t.cc RawBinaryTest ----------------------------------------

TEST(CodecJsonUpstream, RawBinaryArray) {
  const JsonValue doc =
      Arr({Str("a string"), Int(-123), UInt(42), Dbl(1.5), Null(), Bool(true), Bool(false),
           Opaque(ColumnType::kBlob, "abcd"), Obj({{"key", Bool(true)}}), Arr({Bool(false)})});
  const JsonValue v = Parse(Encode(doc));
  ASSERT_EQ(v.elements.size(), 10u);
  EXPECT_EQ(v.elements[0].bytes, "a string");
  EXPECT_EQ(v.elements[1].i, -123);
  EXPECT_EQ(v.elements[2].kind, Kind::kUInt);
  EXPECT_EQ(v.elements[2].u, 42u);
  EXPECT_EQ(v.elements[3].d, 1.5);
  EXPECT_EQ(v.elements[7].opaque_type, ColumnType::kBlob);
  EXPECT_EQ(v.elements[7].bytes, "abcd");
  EXPECT_TRUE(FindJsonMember(v.elements[8], "key")->b);
  EXPECT_EQ(Text(Encode(doc)),
            "[\"a string\", -123, 42, 1.5, null, true, false, \"base64:type252:YWJjZA==\", "
            "{\"key\": true}, [false]]");
}

// ---- json_binary-t.cc StringLengthTest -------------------------------------

TEST(CodecJsonUpstream, StringLengths) {
  // Length prefixes change width at 2^7, 2^14 and 2^21.
  for (size_t size : {0, 1, 127, 128, 16383, 16384, 2097151, 2097152, 3000000}) {
    SCOPED_TRACE(size);
    const std::string s(size, 'a');
    const JsonValue v = Parse(Encode(Str(s)));
    EXPECT_EQ(v.kind, Kind::kString);
    EXPECT_EQ(v.bytes, s);
  }
}

// ---- json_binary-t.cc SpaceNeededTest: the encoder matches the server layout --

TEST(CodecJsonUpstream, SpaceNeeded) {
  struct Case {
    JsonValue value;
    size_t needed;  // bytes after the type byte; 0 = inlined
  };
  const Case cases[] = {
      {Str(""), 1},
      {Str("a"), 2},
      {Str(std::string(127, 'a')), 128},
      {Str(std::string(128, 'a')), 130},
      {Str(std::string(16383, 'a')), 16385},
      {Str(std::string(16384, 'a')), 16387},
      {Int(INT32_MIN), 4},
      {UInt(UINT32_MAX), 4},
      {Int(INT64_MIN), 8},
      {UInt(UINT64_MAX), 8},
      {Dbl(3.14), 8},
      {Opaque(ColumnType::kBlob, ""), 2},
      {Opaque(ColumnType::kBlob, "a"), 3},
      {Opaque(ColumnType::kBlob, std::string(128, 'a')), 131},
      {Datetime(ColumnType::kTime, PackedTime(false, 13, 14, 15, 654321)), 10},
      {Decimal(3, 1, {0x8C, 0x03}), 6},  // create_decimal(12.3)
      {Arr({}), 4},
      {Arr({Dbl(1.5)}), 15},
      {Obj({}), 4},
      {Obj({{"a", Dbl(1.5)}}), 20},
  };
  for (const Case& c : cases) {
    EXPECT_EQ(Encode(c.value).size(), c.needed + 1) << JsonKindName(c.value.kind);
  }
  // Inlined: an array grows by one 3-byte value entry.
  const size_t base = Encode(Arr({})).size();
  for (const JsonValue& inlined : {Null(), Bool(false), Bool(true), Int(0), Int(-1), Int(INT16_MIN),
                                   Int(INT16_MAX), UInt(0), UInt(UINT16_MAX)}) {
    EXPECT_EQ(Encode(Arr({inlined})).size(), base + 3);
  }
}

// ---- json_binary-t.cc CorruptedBinaryTest ----------------------------------

// The document json_binary-t.cc builds: scalars, an object holding copies of
// the array, and a copy of the array nested inside itself.
JsonValue CorruptionDocument() {
  const std::vector<JsonValue> scalars = {Null(),
                                          Bool(true),
                                          Bool(false),
                                          UInt(0),
                                          UInt(123),
                                          UInt(123000),
                                          UInt(123000000),
                                          Int(0),
                                          Int(123),
                                          Int(123000),
                                          Int(123000000),
                                          Int(-123000000),
                                          Str(""),
                                          Str(std::string(300, 'a')),
                                          Decimal(3, 2, {0x83, 0x0E})};
  auto with = [&scalars](std::vector<JsonValue> tail) {
    std::vector<JsonValue> out = scalars;
    for (JsonValue& v : tail) out.push_back(std::move(v));
    return Arr(std::move(out));
  };
  const JsonValue a1 = with({Obj({})});
  const JsonValue blob = Opaque(ColumnType::kBlob, std::string(32, 'x'));
  const JsonValue inner = Obj({{"a1", a1}, {"s", blob}, {"d", Dbl(3.14)}});
  const JsonValue copy = with({inner});
  const JsonValue outer =
      Obj({{"a1", a1}, {"s", blob}, {"d", Dbl(3.14)}, {"a2", with({inner, copy})}});
  return with({outer, copy});
}

void ExpectEveryPrefixTruncated(const Bytes& doc) {
  ASSERT_EQ(ParseStatus(doc), Status::kOk);
  for (size_t i = 1; i < doc.size(); ++i) {
    const Bytes prefix(doc.begin(), doc.begin() + static_cast<std::ptrdiff_t>(i));
    ASSERT_EQ(ParseStatus(prefix), Status::kTruncated) << "prefix " << i << " of " << doc.size();
  }
}

TEST(CodecJsonUpstream, CorruptedTruncatedAtEveryByte) {
  const JsonValue doc = CorruptionDocument();
  ExpectEveryPrefixTruncated(Encode(doc));
  for (const JsonValue& element : doc.elements) ExpectEveryPrefixTruncated(Encode(element));
}

TEST(CodecJsonUpstream, CorruptedBytesNeverCrash) {
  const Bytes doc = Encode(CorruptionDocument());
  size_t rejected = 0;
  for (size_t i = 0; i < doc.size(); ++i) {
    for (size_t j = 1; j < 3 && i + j < doc.size(); ++j) {
      for (uint8_t fill : {0x00, 0x80, 0xFF}) {
        Bytes copy = doc;
        std::fill(copy.begin() + static_cast<std::ptrdiff_t>(i),
                  copy.begin() + static_cast<std::ptrdiff_t>(i + j), fill);
        JsonValue value;
        const Status status = ParseJsonBinary(copy.data(), copy.size(), &value);
        if (status != Status::kOk) {
          ++rejected;
          continue;
        }
        std::string text;
        (void)JsonToText(value, &text);  // any status; must stay in bounds
      }
    }
  }
  EXPECT_GT(rejected, 0u);
}

// ---- Truncation and malformed offsets at every boundary --------------------

TEST(CodecJsonBinary, TruncatedScalarsAndHeaders) {
  for (const Bytes& doc : std::vector<Bytes>{{0x04},
                                             {0x05, 0x01},
                                             {0x07, 0x01, 0x02, 0x03},
                                             {0x09, 1, 2, 3, 4, 5, 6, 7},
                                             {0x0B, 1, 2, 3, 4, 5, 6, 7},
                                             {0x0C},
                                             {0x0C, 0x80},
                                             {0x0C, 0x03, 'a', 'b'},
                                             {0x0F},
                                             {0x0F, 0xFC},
                                             {0x0F, 0xFC, 0x02, 'a'},
                                             {0x00, 0x01, 0x00, 0x0C},
                                             {0x01, 0x01, 0x00, 0x00, 0x00, 0x0C, 0x00},
                                             {0x02, 0x00, 0x00, 0x04}}) {
    EXPECT_EQ(ParseStatus(doc), Status::kTruncated) << doc.size() << " bytes, type " << int{doc[0]};
  }
}

TEST(CodecJsonBinary, MalformedTypesAndLiterals) {
  for (uint8_t type : {0x0D, 0x0E, 0x10, 0xFF})
    EXPECT_EQ(ParseStatus({type, 0, 0, 0, 0}), Status::kMalformedValue);
  EXPECT_EQ(ParseStatus({0x04, 0x03}), Status::kMalformedValue);
  // Five continuation bytes, and a sixth group past 32 bits.
  EXPECT_EQ(ParseStatus({0x0C, 0x80, 0x80, 0x80, 0x80, 0x80, 0x01}), Status::kMalformedValue);
  EXPECT_EQ(ParseStatus({0x0C, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F}), Status::kMalformedValue);
  // An unknown type nested in an array, and an inlined literal out of range.
  EXPECT_EQ(ParseStatus({0x02, 0x01, 0x00, 0x08, 0x00, 0x0D, 0x07, 0x00, 0x00}),
            Status::kMalformedValue);
  EXPECT_EQ(ParseStatus({0x02, 0x01, 0x00, 0x07, 0x00, 0x04, 0x09, 0x00}), Status::kMalformedValue);
}

// Byte positions inside Encode(Obj({{"a", Str("x")}, {"bb", Arr({Str("y")})}})), after the type
// byte.
struct ObjectLayout {
  static constexpr size_t kCount = 0;
  static constexpr size_t kSize = 2;
  static constexpr size_t kKey0Offset = 4;
  static constexpr size_t kKey0Length = 6;
  static constexpr size_t kKey1Offset = 8;
  static constexpr size_t kValue0Offset = 13;  // the type byte of value entry 0 is at 12
  static constexpr size_t kValue1Offset = 16;
};

Bytes LayoutDocument() { return Encode(Obj({{"a", Str("x")}, {"bb", Arr({Str("y")})}})); }

void Poke16(Bytes* doc, size_t pos, uint16_t value) {
  (*doc)[pos] = static_cast<uint8_t>(value);
  (*doc)[pos + 1] = static_cast<uint8_t>(value >> 8);
}

TEST(CodecJsonBinary, MalformedObjectOffsets) {
  const Bytes doc = LayoutDocument();
  ASSERT_EQ(Text(doc), "{\"a\": \"x\", \"bb\": [\"y\"]}");
  const uint16_t size = static_cast<uint16_t>(doc.size() - 1);
  const uint16_t first_key = 2 + 2 + 2 * 4 + 2 * 3;
  ASSERT_EQ(doc[1 + ObjectLayout::kKey0Offset], first_key);

  struct Poke {
    size_t pos;
    uint16_t value;
    Status expected;
  };
  const Poke pokes[] = {
      // Element count whose entries no longer fit the declared size.
      {ObjectLayout::kCount, 5, Status::kMalformedValue},
      // Declared size larger than the input: the input ended early.
      {ObjectLayout::kSize, static_cast<uint16_t>(size + 1), Status::kTruncated},
      // Key inside the entry tables, at the end, and running past the object.
      {ObjectLayout::kKey0Offset, static_cast<uint16_t>(first_key - 1), Status::kMalformedValue},
      {ObjectLayout::kKey0Offset, size, Status::kMalformedValue},
      {ObjectLayout::kKey0Offset, static_cast<uint16_t>(size + 1), Status::kMalformedValue},
      {ObjectLayout::kKey0Length, static_cast<uint16_t>(size - first_key + 1),
       Status::kMalformedValue},
      {ObjectLayout::kKey1Offset, 0, Status::kMalformedValue},
      // Value offsets inside their own entry, past the object, or at its end.
      {ObjectLayout::kValue0Offset, 0, Status::kMalformedValue},
      {ObjectLayout::kValue0Offset, ObjectLayout::kValue0Offset + 1, Status::kMalformedValue},
      {ObjectLayout::kValue1Offset, static_cast<uint16_t>(size + 1), Status::kMalformedValue},
      {ObjectLayout::kValue1Offset, size, Status::kMalformedValue},
  };
  for (const Poke& p : pokes) {
    Bytes copy = doc;
    Poke16(&copy, 1 + p.pos, p.value);
    EXPECT_EQ(ParseStatus(copy), p.expected) << "pos " << p.pos << " value " << p.value;
  }

  // Boundary values that stay valid: an empty key at the very end of the object.
  Bytes copy = doc;
  Poke16(&copy, 1 + ObjectLayout::kKey0Offset, size);
  Poke16(&copy, 1 + ObjectLayout::kKey0Length, 0);
  EXPECT_EQ(ParseStatus(copy), Status::kOk);
}

TEST(CodecJsonBinary, MalformedNestedLengths) {
  // A nested string whose length runs past its container (not past the input).
  Bytes doc = Encode(Arr({Str("abc"), Int(70000)}));
  Bytes copy = doc;
  const size_t string_at = 1 + 4 + 2 * 3;
  ASSERT_EQ(copy[string_at], 3);
  copy[string_at] = 0x7F;
  copy.resize(copy.size() + 200, 0);
  EXPECT_EQ(ParseStatus(copy), Status::kMalformedValue);

  // A nested container that claims more bytes than its parent holds.
  doc = Encode(Arr({Arr({Int(1)}), Int(2)}));
  copy = doc;
  const size_t inner_size = 1 + 4 + 2 * 3 + 2;
  Poke16(&copy, inner_size, 0x0100);
  copy.resize(copy.size() + 0x200, 0);
  EXPECT_EQ(ParseStatus(copy), Status::kMalformedValue);
}

TEST(CodecJsonBinary, TrailingBytesIgnored) {
  Bytes doc = Encode(Arr({Int(1)}));
  doc.push_back(0xAA);
  EXPECT_EQ(Text(doc), "[1]");
}

TEST(CodecJsonBinary, DepthLimit) {
  JsonValue at_limit = Int(1);
  for (size_t i = 0; i < kJsonMaxDepth; ++i) {
    at_limit = i % 2 == 0 ? Arr({std::move(at_limit)}) : Obj({{"k", std::move(at_limit)}});
  }
  const Bytes ok = Encode(at_limit);
  JsonValue parsed;
  ASSERT_EQ(ParseJsonBinary(ok.data(), ok.size(), &parsed), Status::kOk);
  std::string text;
  EXPECT_EQ(JsonToText(parsed, &text), Status::kOk);

  const Bytes too_deep = Encode(Arr({at_limit}));
  JsonValue untouched = Str("keep");
  EXPECT_EQ(ParseJsonBinary(too_deep.data(), too_deep.size(), &untouched), Status::kLimitExceeded);
  EXPECT_EQ(untouched.bytes, "keep");
  const Bytes too_deep_object = Encode(Obj({{"k", at_limit}}));
  EXPECT_EQ(ParseStatus(too_deep_object), Status::kLimitExceeded);
  // The text printer enforces the same limit on a hand-built DOM.
  std::string kept = "keep";
  EXPECT_EQ(JsonToText(Arr({at_limit}), &kept), Status::kLimitExceeded);
  EXPECT_EQ(kept, "keep");
}

TEST(CodecJsonBinary, OutputUntouchedOnFailure) {
  JsonValue value = Str("keep");
  const Bytes bad = {0x02, 0x01, 0x00, 0x07, 0x00, 0x04, 0x09, 0x00};
  EXPECT_NE(ParseJsonBinary(bad.data(), bad.size(), &value), Status::kOk);
  EXPECT_EQ(value.bytes, "keep");
  std::string text = "keep";
  EXPECT_NE(JsonBinaryToText(bad.data(), bad.size(), &text), Status::kOk);
  EXPECT_EQ(text, "keep");
}

}  // namespace
}  // namespace mybinlog
