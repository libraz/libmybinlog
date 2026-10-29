// Partial JSON diffs: Json_diff_vector bytes laid out as
// Json_diff_vector::write_binary does (uint32 length, then per diff the
// operation byte, packed path length, path, and for REPLACE/INSERT a packed
// value length and binary JSON), and their application with the semantics of
// apply_json_diff (`sql-common/json_diff.cc`).

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "mybinlog/json.h"
#include "mybinlog/json_diff.h"

namespace mybinlog {
namespace {

using Bytes = std::vector<uint8_t>;
using Kind = JsonValue::Kind;
using Op = JsonDiffOperation;

// ---- DOM builders --------------------------------------------------------

JsonValue Int(int64_t i) {
  JsonValue v;
  v.kind = Kind::kInt;
  v.i = i;
  return v;
}

JsonValue Str(const std::string& s) {
  JsonValue v;
  v.kind = Kind::kString;
  v.bytes = s;
  return v;
}

JsonValue Arr(std::vector<JsonValue> elements) {
  JsonValue v;
  v.kind = Kind::kArray;
  v.elements = std::move(elements);
  return v;
}

JsonValue Obj(std::vector<std::pair<std::string, JsonValue>> members) {
  JsonValue v;
  v.kind = Kind::kObject;
  for (auto& m : members) v.members.push_back(JsonMember{m.first, std::move(m.second)});
  return v;
}

std::string Text(const JsonValue& v) {
  std::string out;
  EXPECT_EQ(JsonToText(v, &out), Status::kOk);
  return out;
}

// ---- Binary JSON encoder (small format; int16 and literals inlined) -------

void PutU16(Bytes* out, size_t v) {
  out->push_back(static_cast<uint8_t>(v));
  out->push_back(static_cast<uint8_t>(v >> 8));
}

void PutU16At(Bytes* out, size_t at, size_t v) {
  (*out)[at] = static_cast<uint8_t>(v);
  (*out)[at + 1] = static_cast<uint8_t>(v >> 8);
}

uint8_t TypeOf(const JsonValue& v) {
  switch (v.kind) {
    case Kind::kNull:
    case Kind::kBoolean:
      return 0x04;
    case Kind::kInt:
      return v.i >= INT16_MIN && v.i <= INT16_MAX ? 0x05 : 0x09;
    case Kind::kString:
      return 0x0C;
    case Kind::kArray:
      return 0x02;
    case Kind::kObject:
      return 0x00;
    default:
      ADD_FAILURE() << "encoder does not handle " << JsonKindName(v.kind);
      return 0x04;
  }
}

Bytes Body(const JsonValue& v);

Bytes ContainerBody(const JsonValue& v) {
  const bool object = v.kind == Kind::kObject;
  const size_t count = object ? v.members.size() : v.elements.size();
  Bytes out;
  PutU16(&out, count);
  PutU16(&out, 0);  // size, patched below
  const size_t key_entries = out.size();
  if (object) out.resize(out.size() + 4 * count);
  const size_t value_entries = out.size();
  out.resize(out.size() + 3 * count);
  if (object) {
    for (size_t i = 0; i < count; ++i) {
      const std::string& key = v.members[i].key;
      PutU16At(&out, key_entries + 4 * i, out.size());
      PutU16At(&out, key_entries + 4 * i + 2, key.size());
      out.insert(out.end(), key.begin(), key.end());
    }
  }
  for (size_t i = 0; i < count; ++i) {
    const JsonValue& e = object ? v.members[i].value : v.elements[i];
    const uint8_t type = TypeOf(e);
    out[value_entries + 3 * i] = type;
    if (type == 0x04 || type == 0x05) {
      const Bytes inl = Body(e);
      out[value_entries + 3 * i + 1] = inl[0];
      out[value_entries + 3 * i + 2] = inl.size() > 1 ? inl[1] : 0;
    } else {
      PutU16At(&out, value_entries + 3 * i + 1, out.size());
      const Bytes body = Body(e);
      out.insert(out.end(), body.begin(), body.end());
    }
  }
  PutU16At(&out, 2, out.size());
  return out;
}

Bytes Body(const JsonValue& v) {
  switch (v.kind) {
    case Kind::kNull:
      return {0x00};
    case Kind::kBoolean:
      return {static_cast<uint8_t>(v.b ? 0x01 : 0x02)};
    case Kind::kInt: {
      Bytes out;
      const size_t width = TypeOf(v) == 0x05 ? 2 : 8;
      for (size_t i = 0; i < width; ++i) {
        out.push_back(static_cast<uint8_t>(static_cast<uint64_t>(v.i) >> (8 * i)));
      }
      return out;
    }
    case Kind::kString: {
      Bytes out;
      size_t n = v.bytes.size();
      do {
        out.push_back(static_cast<uint8_t>((n & 0x7F) | (n >= 0x80 ? 0x80 : 0)));
        n >>= 7;
      } while (n != 0);
      out.insert(out.end(), v.bytes.begin(), v.bytes.end());
      return out;
    }
    default:
      return ContainerBody(v);
  }
}

Bytes Doc(const JsonValue& v) {
  Bytes out{TypeOf(v)};
  const Bytes body = Body(v);
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

// ---- Diff vector encoder --------------------------------------------------

void PutPacked(Bytes* out, uint64_t n) {
  if (n < 251) {
    out->push_back(static_cast<uint8_t>(n));
  } else if (n < 0x10000) {
    out->push_back(0xFC);
    PutU16(out, n);
  } else {
    out->push_back(0xFD);
    out->push_back(static_cast<uint8_t>(n));
    out->push_back(static_cast<uint8_t>(n >> 8));
    out->push_back(static_cast<uint8_t>(n >> 16));
  }
}

Bytes RawDiff(uint8_t op, const std::string& path, const Bytes* value) {
  Bytes out{op};
  PutPacked(&out, path.size());
  out.insert(out.end(), path.begin(), path.end());
  if (value != nullptr) {
    PutPacked(&out, value->size());
    out.insert(out.end(), value->begin(), value->end());
  }
  return out;
}

Bytes Replace(const std::string& path, const JsonValue& v) {
  const Bytes doc = Doc(v);
  return RawDiff(0, path, &doc);
}

Bytes Insert(const std::string& path, const JsonValue& v) {
  const Bytes doc = Doc(v);
  return RawDiff(1, path, &doc);
}

Bytes Remove(const std::string& path) { return RawDiff(2, path, nullptr); }

Bytes Vector(const std::vector<Bytes>& diffs) {
  Bytes body;
  for (const Bytes& d : diffs) body.insert(body.end(), d.begin(), d.end());
  Bytes out;
  const size_t n = body.size();
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(n >> (8 * i)));
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

Status Parse(const Bytes& bytes, std::vector<JsonDiff>* out) {
  return ParseJsonDiffs(bytes.data(), bytes.size(), out);
}

// Path of a single REMOVE diff after a ParseJsonDiffs round trip.
Status CanonicalPath(const std::string& path, std::string* out) {
  std::vector<JsonDiff> diffs;
  const Status status = Parse(Vector({Remove(path)}), &diffs);
  if (status == Status::kOk) *out = diffs.at(0).path;
  return status;
}

JsonDiff MakeDiff(Op op, const std::string& path, JsonValue value = JsonValue()) {
  JsonDiff d;
  d.op = op;
  d.path = path;
  d.value = std::move(value);
  return d;
}

// Apply to @p before and return the text, or "<status>" on failure.
std::string ApplyText(const JsonValue& before, const std::vector<JsonDiff>& diffs) {
  JsonValue after;
  const Status status = ApplyJsonDiffs(before, diffs, &after);
  if (status != Status::kOk) return std::string("<") + StatusName(status) + ">";
  return Text(after);
}

JsonValue Sample() {
  // {"a": 1, "b": [10, 20, 30], "cc": {"d": "x"}}
  return Obj(
      {{"a", Int(1)}, {"b", Arr({Int(10), Int(20), Int(30)})}, {"cc", Obj({{"d", Str("x")}})}});
}

// ---- ParseJsonDiffs -------------------------------------------------------

TEST(CodecJsonDiffParse, EmptyVectorIsNoDiffs) {
  std::vector<JsonDiff> diffs(1);
  ASSERT_EQ(Parse(Vector({}), &diffs), Status::kOk);
  EXPECT_TRUE(diffs.empty());
}

TEST(CodecJsonDiffParse, EachOperation) {
  std::vector<JsonDiff> diffs;
  ASSERT_EQ(
      Parse(Vector({Replace("$.a", Int(42)), Insert("$.b[1]", Str("x")), Remove("$.c")}), &diffs),
      Status::kOk);
  ASSERT_EQ(diffs.size(), 3u);
  EXPECT_EQ(diffs[0].op, Op::kReplace);
  EXPECT_EQ(diffs[0].path, "$.a");
  EXPECT_EQ(Text(diffs[0].value), "42");
  EXPECT_EQ(diffs[1].op, Op::kInsert);
  EXPECT_EQ(diffs[1].path, "$.b[1]");
  EXPECT_EQ(Text(diffs[1].value), "\"x\"");
  EXPECT_EQ(diffs[2].op, Op::kRemove);
  EXPECT_EQ(diffs[2].path, "$.c");
  EXPECT_EQ(diffs[2].value.kind, Kind::kNull);
}

TEST(CodecJsonDiffParse, ContainerValue) {
  std::vector<JsonDiff> diffs;
  ASSERT_EQ(Parse(Vector({Insert("$.new", Obj({{"x", Int(1)}, {"yy", Arr({Int(2), Str("z")})}}))}),
                  &diffs),
            Status::kOk);
  ASSERT_EQ(diffs.size(), 1u);
  EXPECT_EQ(Text(diffs[0].value), "{\"x\": 1, \"yy\": [2, \"z\"]}");
}

TEST(CodecJsonDiffParse, ZeroLengthValueIsNullLiteral) {
  const Bytes empty;
  std::vector<JsonDiff> diffs;
  ASSERT_EQ(Parse(Vector({RawDiff(0, "$.a", &empty)}), &diffs), Status::kOk);
  ASSERT_EQ(diffs.size(), 1u);
  EXPECT_EQ(Text(diffs[0].value), "null");
}

TEST(CodecJsonDiffParse, MultiByteLengths) {
  // A 300-byte key needs the 0xFC path length; a 70000-byte string the 0xFD value length.
  const std::string key(300, 'k');
  const std::string big(70000, 's');
  std::vector<JsonDiff> diffs;
  ASSERT_EQ(Parse(Vector({Replace("$." + key, Str(big))}), &diffs), Status::kOk);
  ASSERT_EQ(diffs.size(), 1u);
  EXPECT_EQ(diffs[0].path, "$." + key);
  EXPECT_EQ(diffs[0].value.bytes, big);
}

TEST(CodecJsonDiffParse, OutputUntouchedOnFailure) {
  std::vector<JsonDiff> diffs(2);
  EXPECT_EQ(Parse(Vector({Remove("$.a"), Remove("$[")}), &diffs), Status::kMalformedValue);
  EXPECT_EQ(diffs.size(), 2u);
}

TEST(CodecJsonDiffParse, NullDataWithZeroSizeIsTruncated) {
  std::vector<JsonDiff> diffs;
  EXPECT_EQ(ParseJsonDiffs(nullptr, 0, &diffs), Status::kTruncated);
}

TEST(CodecJsonDiffParse, EveryShortInputIsTruncated) {
  const Bytes full = Vector({Replace("$.a", Str("hello")), Insert("$[0]", Int(7)), Remove("$.b")});
  for (size_t size = 0; size < full.size(); ++size) {
    SCOPED_TRACE(size);
    std::vector<JsonDiff> diffs;
    EXPECT_EQ(ParseJsonDiffs(full.data(), size, &diffs), Status::kTruncated);
  }
}

TEST(CodecJsonDiffParse, TrailingBytesAreMalformed) {
  Bytes bytes = Vector({Remove("$.a")});
  bytes.push_back(0);
  std::vector<JsonDiff> diffs;
  EXPECT_EQ(Parse(bytes, &diffs), Status::kMalformedValue);
}

// The declared length cut at every byte: only diff boundaries parse.
TEST(CodecJsonDiffParse, EveryInnerCutIsMalformedOrABoundary) {
  const std::vector<Bytes> parts = {Replace("$.a", Str("hello")), Insert("$[0]", Int(7)),
                                    Remove("$.b"), Replace("$.\"x y\"", Arr({Int(1)}))};
  Bytes body;
  std::vector<size_t> boundaries = {0};
  for (const Bytes& p : parts) {
    body.insert(body.end(), p.begin(), p.end());
    boundaries.push_back(body.size());
  }
  for (size_t cut = 0; cut <= body.size(); ++cut) {
    SCOPED_TRACE(cut);
    Bytes bytes = Vector({Bytes(body.begin(), body.begin() + static_cast<std::ptrdiff_t>(cut))});
    std::vector<JsonDiff> diffs;
    const Status status = Parse(bytes, &diffs);
    size_t complete = 0;
    bool at_boundary = false;
    for (size_t i = 0; i < boundaries.size(); ++i) {
      if (boundaries[i] == cut) {
        at_boundary = true;
        complete = i;
      }
    }
    if (at_boundary) {
      ASSERT_EQ(status, Status::kOk);
      EXPECT_EQ(diffs.size(), complete);
    } else {
      EXPECT_EQ(status, Status::kMalformedValue);
    }
  }
}

TEST(CodecJsonDiffParse, BadOperation) {
  const Bytes value = Doc(Int(1));
  for (uint8_t op : {uint8_t{3}, uint8_t{127}, uint8_t{255}}) {
    std::vector<JsonDiff> diffs;
    EXPECT_EQ(Parse(Vector({RawDiff(op, "$.a", &value)}), &diffs), Status::kMalformedValue)
        << int{op};
  }
}

TEST(CodecJsonDiffParse, BadLengths) {
  std::vector<JsonDiff> diffs;
  // Path length is the NULL marker / undefined prefix.
  EXPECT_EQ(Parse(Vector({Bytes{2, 0xFB}}), &diffs), Status::kMalformedValue);
  EXPECT_EQ(Parse(Vector({Bytes{2, 0xFF, 1, 0, 0, 0, 0, 0, 0, 0}}), &diffs),
            Status::kMalformedValue);
  // Path length past the vector, and a packed length cut inside its own bytes.
  EXPECT_EQ(Parse(Vector({Bytes{2, 5, '$', '.', 'a'}}), &diffs), Status::kMalformedValue);
  EXPECT_EQ(Parse(Vector({Bytes{2, 0xFC, 3}}), &diffs), Status::kMalformedValue);
  // Value length past the vector.
  EXPECT_EQ(Parse(Vector({Bytes{0, 3, '$', '.', 'a', 9, 0x04, 0x01}}), &diffs),
            Status::kMalformedValue);
  // Missing value length.
  EXPECT_EQ(Parse(Vector({Bytes{1, 3, '$', '.', 'a'}}), &diffs), Status::kMalformedValue);
}

TEST(CodecJsonDiffParse, BadValue) {
  std::vector<JsonDiff> diffs;
  const Bytes unknown_type = {0x0D, 0x00};
  EXPECT_EQ(Parse(Vector({RawDiff(0, "$.a", &unknown_type)}), &diffs), Status::kMalformedValue);
  // A string whose own length runs past the declared value length.
  const Bytes short_string = {0x0C, 0x05, 'a', 'b'};
  EXPECT_EQ(Parse(Vector({RawDiff(0, "$.a", &short_string)}), &diffs), Status::kMalformedValue);
  const Bytes bad_literal = {0x04, 0x07};
  EXPECT_EQ(Parse(Vector({RawDiff(1, "$.a", &bad_literal)}), &diffs), Status::kMalformedValue);
}

TEST(CodecJsonDiffParse, TooDeepValue) {
  JsonValue v = Int(1);
  for (size_t i = 0; i <= kJsonMaxDepth; ++i) v = Arr({v});
  std::vector<JsonDiff> diffs;
  EXPECT_EQ(Parse(Vector({Replace("$.a", v)}), &diffs), Status::kLimitExceeded);
}

// ---- Path syntax ------------------------------------------------------------

TEST(CodecJsonDiffPath, Canonical) {
  const std::vector<std::pair<std::string, std::string>> cases = {
      {"$.a", "$.a"},
      {"$", "$"},
      {"  $ . a [ 1 ] . \"b c\" [ last - 2 ] ", "$.a[1].\"b c\"[last-2]"},
      {"$[last]", "$[last]"},
      {"$[last-0]", "$[last]"},
      {"$[007]", "$[7]"},
      {"$[4294967295]", "$[4294967295]"},
      {"$.\"abc\"", "$.abc"},
      {"$.a\\u0062", "$.ab"},
      {"$.\"\"", "$.\"\""},
      {"$.\"1a\"", "$.\"1a\""},
      {"$.$_x9", "$.$_x9"},
      {"$.\"a-b\"", "$.\"a-b\""},
      {"$.\"a\\\"b\\\\c\\n\\u0001\"", "$.\"a\\\"b\\\\c\\n\\u0001\""},
      {"$.\"\\ud83d\\ude00\"", "$.\"\xF0\x9F\x98\x80\""},
      {"$.\"\\/\"", "$.\"/\""},
  };
  for (const auto& [in, want] : cases) {
    std::string got;
    ASSERT_EQ(CanonicalPath(in, &got), Status::kOk) << in;
    EXPECT_EQ(got, want) << in;
  }
}

// is_ecmascript_identifier looks up only each codepoint's low byte in the
// utf8mb4 ctype table, so whether a non-ASCII key prints quoted depends on
// that byte: U+3042 (0x42 'B') and U+00E9 (0xE9) count as letters, U+672C
// (0x2C ',') and U+4E00 (0x00) do not.
TEST(CodecJsonDiffPath, UnicodeKeys) {
  const std::vector<std::pair<std::string, std::string>> cases = {
      {"$.\"\xE3\x81\x82\"", "$.\xE3\x81\x82"},                              // あ
      {"$.\xC3\xA9t\xC3\xA9", "$.\xC3\xA9t\xC3\xA9"},                        // été
      {"$.\"\xE6\x97\xA5\xE6\x9C\xAC\"", "$.\"\xE6\x97\xA5\xE6\x9C\xAC\""},  // 日本
      {"$.\"\xE4\xB8\x80\"", "$.\"\xE4\xB8\x80\""},                          // 一
      {"$.a\xCC\x81", "$.a\xCC\x81"},                                        // a + combining acute
      {"$.\"\xCC\x81"
       "a\"",
       "$.\"\xCC\x81"
       "a\""},                                 // combining mark first
      {"$.a\xE2\x80\xBF", "$.a\xE2\x80\xBF"},  // undertie
  };
  for (const auto& [in, want] : cases) {
    std::string got;
    ASSERT_EQ(CanonicalPath(in, &got), Status::kOk) << in;
    EXPECT_EQ(got, want) << in;
  }
}

TEST(CodecJsonDiffPath, Rejected) {
  const std::vector<std::string> cases = {
      "",
      " ",
      "a",
      "$a",
      "$.",
      "$. ",
      "$[",
      "$[1",
      "$[]",
      "$[x]",
      "$[-1]",
      "$[last-]",
      "$[4294967296]",
      "$[*]",
      "$.*",
      "$**.a",
      "$.a**",
      "$[1 to 2]",
      "$[last to 0]",
      "$.\"unterminated",
      "$.\"a\\",
      "$.a\"b",
      "$.1a",
      "$.a-b",
      "$.\"\\x\"",
      "$.\"\\u00\"",
      "$.\"\\ud800\"",
      "$.\"\\udc00\"",
      "$.\"\\ud800\\u0041\"",
      "$.\"a\tb\"",
      "$.\xFF",
      "$.a\xC3",
  };
  for (const std::string& path : cases) {
    std::string got;
    EXPECT_EQ(CanonicalPath(path, &got), Status::kMalformedValue) << "path: " << path;
  }
}

// ---- ApplyJsonDiffs -------------------------------------------------------

TEST(CodecJsonDiffApply, Replace) {
  const JsonValue before = Sample();
  EXPECT_EQ(ApplyText(before, {MakeDiff(Op::kReplace, "$.a", Str("one"))}),
            "{\"a\": \"one\", \"b\": [10, 20, 30], \"cc\": {\"d\": \"x\"}}");
  EXPECT_EQ(ApplyText(before, {MakeDiff(Op::kReplace, "$.b[1]", Int(21))}),
            "{\"a\": 1, \"b\": [10, 21, 30], \"cc\": {\"d\": \"x\"}}");
  EXPECT_EQ(ApplyText(before, {MakeDiff(Op::kReplace, "$.b[last]", Int(31))}),
            "{\"a\": 1, \"b\": [10, 20, 31], \"cc\": {\"d\": \"x\"}}");
  EXPECT_EQ(ApplyText(before, {MakeDiff(Op::kReplace, "$.b[last-2]", Int(11))}),
            "{\"a\": 1, \"b\": [11, 20, 30], \"cc\": {\"d\": \"x\"}}");
  EXPECT_EQ(ApplyText(before, {MakeDiff(Op::kReplace, "$.cc.d", Arr({Int(1)}))}),
            "{\"a\": 1, \"b\": [10, 20, 30], \"cc\": {\"d\": [1]}}");
}

TEST(CodecJsonDiffApply, ReplaceRequiresExistingTarget) {
  const JsonValue before = Sample();
  for (const char* path :
       {"$.zz", "$.b[3]", "$.b[last-3]", "$.cc.e", "$.a.x", "$.a[0]", "$.b.x", "$[0]", "$.cc[0]"}) {
    EXPECT_EQ(ApplyText(before, {MakeDiff(Op::kReplace, path, Int(0))}), "<kMalformedValue>")
        << path;
  }
}

TEST(CodecJsonDiffApply, InsertMemberKeepsServerKeyOrder) {
  const JsonValue before = Obj({{"a", Int(1)}, {"bb", Int(2)}, {"ccc", Int(3)}});
  EXPECT_EQ(ApplyText(before, {MakeDiff(Op::kInsert, "$.b", Int(0))}),
            "{\"a\": 1, \"b\": 0, \"bb\": 2, \"ccc\": 3}");
  EXPECT_EQ(ApplyText(before, {MakeDiff(Op::kInsert, "$.zz", Int(0))}),
            "{\"a\": 1, \"bb\": 2, \"zz\": 0, \"ccc\": 3}");
  EXPECT_EQ(ApplyText(before, {MakeDiff(Op::kInsert, "$.aa", Int(0))}),
            "{\"a\": 1, \"aa\": 0, \"bb\": 2, \"ccc\": 3}");
  EXPECT_EQ(ApplyText(before, {MakeDiff(Op::kInsert, "$.\"\"", Int(0))}),
            "{\"\": 0, \"a\": 1, \"bb\": 2, \"ccc\": 3}");
  EXPECT_EQ(ApplyText(before, {MakeDiff(Op::kInsert, "$.dddd", Int(0))}),
            "{\"a\": 1, \"bb\": 2, \"ccc\": 3, \"dddd\": 0}");
  EXPECT_EQ(ApplyText(Obj({}), {MakeDiff(Op::kInsert, "$.k", Int(0))}), "{\"k\": 0}");
}

TEST(CodecJsonDiffApply, InsertMemberComparesUnsignedBytes) {
  // "é" is 2 bytes (C3 A9) and sorts after every 2-byte ASCII key.
  const JsonValue before = Obj({{"zz", Int(1)}});
  EXPECT_EQ(ApplyText(before, {MakeDiff(Op::kInsert, "$.\xC3\xA9", Int(2))}),
            "{\"zz\": 1, \"\xC3\xA9\": 2}");
  EXPECT_EQ(ApplyText(before, {MakeDiff(Op::kInsert, "$.Z", Int(2))}), "{\"Z\": 2, \"zz\": 1}");
}

TEST(CodecJsonDiffApply, InsertArrayElement) {
  const JsonValue before = Arr({Int(1), Int(2), Int(3)});
  EXPECT_EQ(ApplyText(before, {MakeDiff(Op::kInsert, "$[0]", Int(0))}), "[0, 1, 2, 3]");
  EXPECT_EQ(ApplyText(before, {MakeDiff(Op::kInsert, "$[2]", Int(0))}), "[1, 2, 0, 3]");
  EXPECT_EQ(ApplyText(before, {MakeDiff(Op::kInsert, "$[3]", Int(0))}), "[1, 2, 3, 0]");
  EXPECT_EQ(ApplyText(before, {MakeDiff(Op::kInsert, "$[99]", Int(0))}), "[1, 2, 3, 0]");
  // [last] names the last element, so the value goes before it.
  EXPECT_EQ(ApplyText(before, {MakeDiff(Op::kInsert, "$[last]", Int(0))}), "[1, 2, 0, 3]");
  EXPECT_EQ(ApplyText(before, {MakeDiff(Op::kInsert, "$[last-2]", Int(0))}), "[0, 1, 2, 3]");
  EXPECT_EQ(ApplyText(before, {MakeDiff(Op::kInsert, "$[last-9]", Int(0))}), "[0, 1, 2, 3]");
  EXPECT_EQ(ApplyText(Arr({}), {MakeDiff(Op::kInsert, "$[last]", Int(0))}), "[0]");
  EXPECT_EQ(ApplyText(Sample(), {MakeDiff(Op::kInsert, "$.b[1]", Str("n"))}),
            "{\"a\": 1, \"b\": [10, \"n\", 20, 30], \"cc\": {\"d\": \"x\"}}");
}

TEST(CodecJsonDiffApply, InsertRejected) {
  const JsonValue before = Sample();
  for (const char* path : {"$.a", "$.zz.y", "$.b.x", "$.cc[0]", "$.a[0]", "$.a.x", "$.b[5][0]"}) {
    EXPECT_EQ(ApplyText(before, {MakeDiff(Op::kInsert, path, Int(0))}), "<kMalformedValue>")
        << path;
  }
}

TEST(CodecJsonDiffApply, Remove) {
  const JsonValue before = Sample();
  EXPECT_EQ(ApplyText(before, {MakeDiff(Op::kRemove, "$.a")}),
            "{\"b\": [10, 20, 30], \"cc\": {\"d\": \"x\"}}");
  EXPECT_EQ(ApplyText(before, {MakeDiff(Op::kRemove, "$.b[0]")}),
            "{\"a\": 1, \"b\": [20, 30], \"cc\": {\"d\": \"x\"}}");
  EXPECT_EQ(ApplyText(before, {MakeDiff(Op::kRemove, "$.b[last]")}),
            "{\"a\": 1, \"b\": [10, 20], \"cc\": {\"d\": \"x\"}}");
  EXPECT_EQ(ApplyText(before, {MakeDiff(Op::kRemove, "$.cc.d")}),
            "{\"a\": 1, \"b\": [10, 20, 30], \"cc\": {}}");
}

TEST(CodecJsonDiffApply, RemoveRejected) {
  const JsonValue before = Sample();
  for (const char* path :
       {"$.zz", "$.b[3]", "$.b[last-3]", "$.b.x", "$.cc[0]", "$.a[0]", "$.a.x", "$.zz.y"}) {
    EXPECT_EQ(ApplyText(before, {MakeDiff(Op::kRemove, path)}), "<kMalformedValue>") << path;
  }
}

TEST(CodecJsonDiffApply, RootPathRejected) {
  for (Op op : {Op::kReplace, Op::kInsert, Op::kRemove}) {
    EXPECT_EQ(ApplyText(Sample(), {MakeDiff(op, "$", Int(0))}), "<kMalformedValue>")
        << JsonDiffOperationName(op);
  }
}

TEST(CodecJsonDiffApply, ScalarRootResolvesNothing) {
  // No auto-wrapping: $[0] does not name a scalar document.
  for (Op op : {Op::kReplace, Op::kInsert, Op::kRemove}) {
    EXPECT_EQ(ApplyText(Int(5), {MakeDiff(op, "$[0]", Int(0))}), "<kMalformedValue>");
    EXPECT_EQ(ApplyText(JsonValue(), {MakeDiff(op, "$.a", Int(0))}), "<kMalformedValue>");
  }
}

TEST(CodecJsonDiffApply, BadPathOrOperation) {
  EXPECT_EQ(ApplyText(Sample(), {MakeDiff(Op::kReplace, "$.b[*]", Int(0))}), "<kMalformedValue>");
  EXPECT_EQ(ApplyText(Sample(), {MakeDiff(Op::kReplace, "a", Int(0))}), "<kMalformedValue>");
  EXPECT_EQ(ApplyText(Sample(), {MakeDiff(static_cast<Op>(3), "$.a", Int(0))}),
            "<kMalformedValue>");
}

TEST(CodecJsonDiffApply, NonCanonicalPathAccepted) {
  EXPECT_EQ(ApplyText(Sample(), {MakeDiff(Op::kReplace, " $ . \"cc\" . d ", Int(0))}),
            "{\"a\": 1, \"b\": [10, 20, 30], \"cc\": {\"d\": 0}}");
}

TEST(CodecJsonDiffApply, QuotedAndUnicodeKeys) {
  const JsonValue before =
      Obj({{"b c", Int(1)}, {"\xE6\x97\xA5\xE6\x9C\xAC", Obj({{"a\"b", Int(2)}})}});
  EXPECT_EQ(ApplyText(before, {MakeDiff(Op::kReplace, "$.\"b c\"", Int(9)),
                               MakeDiff(Op::kReplace, "$.\"\xE6\x97\xA5\xE6\x9C\xAC\".\"a\\\"b\"",
                                        Int(8))}),
            "{\"b c\": 9, \"\xE6\x97\xA5\xE6\x9C\xAC\": {\"a\\\"b\": 8}}");
}

TEST(CodecJsonDiffApply, DiffsSeeEarlierEdits) {
  EXPECT_EQ(
      ApplyText(Sample(),
                {MakeDiff(Op::kInsert, "$.n", Obj({})), MakeDiff(Op::kInsert, "$.n.x", Arr({})),
                 MakeDiff(Op::kInsert, "$.n.x[0]", Int(1)),
                 MakeDiff(Op::kReplace, "$.n.x[0]", Int(2)), MakeDiff(Op::kRemove, "$.a")}),
      "{\"b\": [10, 20, 30], \"n\": {\"x\": [2]}, \"cc\": {\"d\": \"x\"}}");
}

TEST(CodecJsonDiffApply, EmptyDiffsCopy) { EXPECT_EQ(ApplyText(Sample(), {}), Text(Sample())); }

TEST(CodecJsonDiffApply, OutputUntouchedOnFailure) {
  JsonValue after = Int(77);
  EXPECT_EQ(ApplyJsonDiffs(Sample(), {MakeDiff(Op::kRemove, "$.a"), MakeDiff(Op::kRemove, "$.a")},
                           &after),
            Status::kMalformedValue);
  EXPECT_EQ(Text(after), "77");
}

TEST(CodecJsonDiffApply, AfterMayAliasBefore) {
  JsonValue doc = Sample();
  ASSERT_EQ(ApplyJsonDiffs(doc, {MakeDiff(Op::kReplace, "$.b", Int(0))}, &doc), Status::kOk);
  EXPECT_EQ(Text(doc), "{\"a\": 1, \"b\": 0, \"cc\": {\"d\": \"x\"}}");
}

// ---- ApplyJsonDiffsToBinary ----------------------------------------------

TEST(CodecJsonDiffBinary, EndToEnd) {
  const Bytes before = Doc(Sample());
  const Bytes diffs = Vector({Replace("$.b[last]", Str("thirty")), Insert("$.bb", Int(-40000)),
                              Remove("$.cc.d"), Insert("$.b[0]", Int(5))});
  std::string text;
  ASSERT_EQ(ApplyJsonDiffsToBinary(before.data(), before.size(), diffs.data(), diffs.size(), &text),
            Status::kOk);
  EXPECT_EQ(text, "{\"a\": 1, \"b\": [5, 10, 20, \"thirty\"], \"bb\": -40000, \"cc\": {}}");
}

TEST(CodecJsonDiffBinary, EmptyVectorKeepsDocument) {
  const Bytes before = Doc(Sample());
  const Bytes diffs = Vector({});
  std::string text;
  ASSERT_EQ(ApplyJsonDiffsToBinary(before.data(), before.size(), diffs.data(), diffs.size(), &text),
            Status::kOk);
  EXPECT_EQ(text, Text(Sample()));
}

TEST(CodecJsonDiffBinary, Failures) {
  const Bytes before = Doc(Sample());
  const Bytes diffs = Vector({Remove("$.a")});
  std::string text = "kept";
  // Empty previous value is the null literal, which no path resolves in.
  EXPECT_EQ(ApplyJsonDiffsToBinary(nullptr, 0, diffs.data(), diffs.size(), &text),
            Status::kMalformedValue);
  EXPECT_EQ(ApplyJsonDiffsToBinary(before.data(), before.size(), nullptr, 0, &text),
            Status::kTruncated);
  EXPECT_EQ(ApplyJsonDiffsToBinary(before.data(), 3, diffs.data(), diffs.size(), &text),
            Status::kTruncated);
  const Bytes missing = Vector({Remove("$.zz")});
  EXPECT_EQ(
      ApplyJsonDiffsToBinary(before.data(), before.size(), missing.data(), missing.size(), &text),
      Status::kMalformedValue);
  EXPECT_EQ(text, "kept");
}

TEST(CodecJsonDiffNames, Operation) {
  EXPECT_STREQ(JsonDiffOperationName(Op::kReplace), "kReplace");
  EXPECT_STREQ(JsonDiffOperationName(Op::kInsert), "kInsert");
  EXPECT_STREQ(JsonDiffOperationName(Op::kRemove), "kRemove");
  EXPECT_STREQ(JsonDiffOperationName(static_cast<Op>(9)), "kUnknown");
}

}  // namespace
}  // namespace mybinlog
