// GtidSet text grammar, binary encoding and set operations.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "mybinlog/mysql_gtid.h"

namespace mybinlog {
namespace {

constexpr char kSid[] = "00000000-0000-0000-0000-000000000001";
constexpr char kSid2[] = "00000000-0000-0000-0000-000000000002";

uint64_t ReadInt64Le(const std::vector<uint8_t>& data, size_t offset) {
  uint64_t value = 0;
  for (int i = 7; i >= 0; --i) value = (value << 8) | data[offset + i];
  return value;
}

std::vector<uint8_t> ExtractUuid(const std::vector<uint8_t>& data, size_t offset) {
  return std::vector<uint8_t>(data.begin() + offset, data.begin() + offset + 16);
}

// Parse + EncodeBinary, the path COM_BINLOG_DUMP_GTID takes.
Status Encode(const std::string& text, std::vector<uint8_t>* out) {
  GtidSet set;
  const Status status = GtidSet::Parse(text, &set);
  if (status == Status::kOk) set.EncodeBinary(out);
  return status;
}

// --- Encoding ---

TEST(CodecGtidSet, EmptySetEncodesZeroTsids) {
  std::vector<uint8_t> out;
  ASSERT_EQ(Encode("", &out), Status::kOk);
  ASSERT_EQ(out.size(), 8u);
  EXPECT_EQ(ReadInt64Le(out, 0), 0u);
}

TEST(CodecGtidSet, SingleGtidIsOneTransaction) {
  std::vector<uint8_t> out;
  ASSERT_EQ(Encode(std::string(kSid) + ":5", &out), Status::kOk);
  ASSERT_EQ(out.size(), 48u);
  EXPECT_EQ(ReadInt64Le(out, 0), 1u);
  EXPECT_EQ(ReadInt64Le(out, 24), 1u);
  EXPECT_EQ(ReadInt64Le(out, 32), 5u);
  EXPECT_EQ(ReadInt64Le(out, 40), 6u);
}

TEST(CodecGtidSet, SingleRange) {
  std::vector<uint8_t> out;
  ASSERT_EQ(Encode(std::string(kSid) + ":1-3", &out), Status::kOk);
  ASSERT_EQ(out.size(), 48u);
  EXPECT_EQ(ReadInt64Le(out, 24), 1u);
  EXPECT_EQ(ReadInt64Le(out, 32), 1u);
  EXPECT_EQ(ReadInt64Le(out, 40), 4u);
}

TEST(CodecGtidSet, MultipleIntervals) {
  std::vector<uint8_t> out;
  ASSERT_EQ(Encode(std::string(kSid) + ":1-3:5-7", &out), Status::kOk);
  ASSERT_EQ(out.size(), 64u);
  EXPECT_EQ(ReadInt64Le(out, 24), 2u);
  EXPECT_EQ(ReadInt64Le(out, 32), 1u);
  EXPECT_EQ(ReadInt64Le(out, 40), 4u);
  EXPECT_EQ(ReadInt64Le(out, 48), 5u);
  EXPECT_EQ(ReadInt64Le(out, 56), 8u);
}

TEST(CodecGtidSet, MultipleUuids) {
  std::vector<uint8_t> out;
  ASSERT_EQ(Encode(std::string(kSid) + ":1-3," + kSid2 + ":5-7", &out), Status::kOk);
  EXPECT_EQ(ReadInt64Le(out, 0), 2u);
  std::vector<uint8_t> uuid1(16, 0);
  uuid1[15] = 1;
  std::vector<uint8_t> uuid2(16, 0);
  uuid2[15] = 2;
  EXPECT_EQ(ExtractUuid(out, 8), uuid1);
  EXPECT_EQ(ReadInt64Le(out, 24), 1u);
  EXPECT_EQ(ReadInt64Le(out, 32), 1u);
  EXPECT_EQ(ReadInt64Le(out, 40), 4u);
  EXPECT_EQ(ExtractUuid(out, 48), uuid2);
  EXPECT_EQ(ReadInt64Le(out, 64), 1u);
  EXPECT_EQ(ReadInt64Le(out, 72), 5u);
  EXPECT_EQ(ReadInt64Le(out, 80), 8u);
}

TEST(CodecGtidSet, OverlappingAdjacentAndDuplicateIntervalsMerge) {
  std::vector<uint8_t> out;
  ASSERT_EQ(Encode(std::string(kSid) + ":1-5:3-8", &out), Status::kOk);
  EXPECT_EQ(ReadInt64Le(out, 24), 1u);
  EXPECT_EQ(ReadInt64Le(out, 32), 1u);
  EXPECT_EQ(ReadInt64Le(out, 40), 9u);

  ASSERT_EQ(Encode(std::string(kSid) + ":1-3:4-6", &out), Status::kOk);
  EXPECT_EQ(ReadInt64Le(out, 24), 1u);
  EXPECT_EQ(ReadInt64Le(out, 40), 7u);

  ASSERT_EQ(Encode(std::string(kSid) + ":1-3, " + kSid + ":5-7", &out), Status::kOk);
  EXPECT_EQ(ReadInt64Le(out, 0), 1u);
  EXPECT_EQ(ReadInt64Le(out, 24), 2u);
  EXPECT_EQ(ReadInt64Le(out, 48), 5u);
  EXPECT_EQ(ReadInt64Le(out, 56), 8u);

  ASSERT_EQ(Encode(std::string(kSid) + ":1-100," + kSid + ":50-150", &out), Status::kOk);
  EXPECT_EQ(ReadInt64Le(out, 0), 1u);
  EXPECT_EQ(ReadInt64Le(out, 24), 1u);
  EXPECT_EQ(ReadInt64Le(out, 40), 151u);
}

TEST(CodecGtidSet, UuidBytes) {
  std::vector<uint8_t> out;
  ASSERT_EQ(Encode("61d5b289-bccc-11f0-b921-cabbb4ee51f6:1-10", &out), Status::kOk);
  EXPECT_EQ(ExtractUuid(out, 8),
            (std::vector<uint8_t>{0x61, 0xd5, 0xb2, 0x89, 0xbc, 0xcc, 0x11, 0xf0, 0xb9, 0x21, 0xca,
                                  0xbb, 0xb4, 0xee, 0x51, 0xf6}));
  ASSERT_EQ(Encode("AbCdEf01-2345-6789-aBcD-ef0123456789:1-3", &out), Status::kOk);
  EXPECT_EQ(ExtractUuid(out, 8),
            (std::vector<uint8_t>{0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
                                  0x01, 0x23, 0x45, 0x67, 0x89}));
}

TEST(CodecGtidSet, RangeStartEqualsEndAndLargeGnos) {
  std::vector<uint8_t> out;
  ASSERT_EQ(Encode(std::string(kSid) + ":3-3", &out), Status::kOk);
  EXPECT_EQ(ReadInt64Le(out, 32), 3u);
  EXPECT_EQ(ReadInt64Le(out, 40), 4u);

  ASSERT_EQ(Encode(std::string(kSid) + ":1-9223372036854775805", &out), Status::kOk);
  EXPECT_EQ(ReadInt64Le(out, 40), 9223372036854775806ULL);

  ASSERT_EQ(Encode(std::string(kSid) + ":1000000000000-2000000000000", &out), Status::kOk);
  EXPECT_EQ(ReadInt64Le(out, 32), 1000000000000u);
  EXPECT_EQ(ReadInt64Le(out, 40), 2000000000001u);
}

TEST(CodecGtidSet, WhitespaceAroundEntriesAndCommas) {
  std::vector<uint8_t> out;
  ASSERT_EQ(Encode(std::string("  ") + kSid + ":1-3  ", &out), Status::kOk);
  EXPECT_EQ(ReadInt64Le(out, 0), 1u);
  ASSERT_EQ(Encode(std::string(kSid) + ":1 , " + kSid2 + ":2", &out), Status::kOk);
  EXPECT_EQ(ReadInt64Le(out, 0), 2u);
}

TEST(CodecGtidSet, MalformedTextIsRejected) {
  const std::string sid(kSid);
  for (const std::string& text : {
           std::string("00000000-0000-0000-0000-0000000000011-3"),  // no colon
           std::string("00000000-0000-0000-0000-00000001:1-3"),     // short UUID
           std::string("00000000-0000-0000-0000-0000000001:1-3"),
           std::string("00000000-0000-0000-0000-00000000000001:1-3"),  // long UUID
           std::string("0000000g-0000-0000-0000-000000000001:1-3"),    // non-hex
           std::string("Z0000000-0000-0000-0000-000000000001:1-3"),
           std::string("00000000-0000-x000-0000-000000000001:1-3"),
           sid + ":abc",
           sid + ":-1-3",
           sid + ":0-3",
           sid + ":5-3",
           sid + ":",
           sid + ":9223372036854775807",
           sid + ":1-9223372036854775807",
           sid + ":bad-tag:1-9",
       }) {
    GtidSet set;
    EXPECT_EQ(GtidSet::Parse(text, &set), Status::kMalformedValue) << "input: '" << text << "'";
  }
}

// --- Tagged encoding (format 1) ---

TEST(CodecGtidSet, TaggedSetUsesFormatOneAndLowerCasesTheTag) {
  std::vector<uint8_t> out;
  ASSERT_EQ(Encode(std::string(kSid) + ":MyTag:1-5", &out), Status::kOk);
  ASSERT_EQ(out.size(), 54u);
  EXPECT_EQ(ReadInt64Le(out, 0), (uint64_t{1} << 56) | (uint64_t{1} << 8) | 1);
  EXPECT_EQ(out[24], 10u);
  EXPECT_EQ(std::string(out.begin() + 25, out.begin() + 30), "mytag");
  EXPECT_EQ(ReadInt64Le(out, 30), 1u);
  EXPECT_EQ(ReadInt64Le(out, 38), 1u);
  EXPECT_EQ(ReadInt64Le(out, 46), 6u);
}

TEST(CodecGtidSet, TaggedSetEncodesUntaggedTsidWithEmptyTag) {
  std::vector<uint8_t> out;
  ASSERT_EQ(Encode(std::string(kSid) + ":1-2," + kSid2 + ":tag:7", &out), Status::kOk);
  EXPECT_EQ(ReadInt64Le(out, 0), (uint64_t{1} << 56) | (uint64_t{2} << 8) | 1);
  EXPECT_EQ(out[24], 0u);
  EXPECT_EQ(ReadInt64Le(out, 25), 1u);
  EXPECT_EQ(ReadInt64Le(out, 33), 1u);
  EXPECT_EQ(ReadInt64Le(out, 41), 3u);
}

TEST(CodecGtidSet, TagAcceptsUnderscoreAndDigits) {
  std::vector<uint8_t> out;
  EXPECT_EQ(Encode(std::string(kSid) + ":_tag_2:7", &out), Status::kOk);
}

TEST(CodecGtidSet, TaggedBinaryAndTextRoundTripPreserveEveryTsid) {
  GtidSet original;
  ASSERT_EQ(GtidSet::Parse(std::string(kSid) + ":Analytics:1-3," + kSid2 + ":7-9", &original),
            Status::kOk);
  std::vector<uint8_t> encoded;
  original.EncodeBinary(&encoded);

  GtidSet decoded;
  ASSERT_EQ(GtidSet::DecodeBinary(encoded.data(), encoded.size(), &decoded), Status::kOk);
  EXPECT_EQ(decoded.ToString(), std::string(kSid) + ":analytics:1-3," + kSid2 + ":7-9");

  GtidSet reparsed;
  ASSERT_EQ(GtidSet::Parse(decoded.ToString(), &reparsed), Status::kOk);
  EXPECT_EQ(reparsed.ToString(), decoded.ToString());
}

// Bytes written from the documented layout, not from this encoder.
TEST(CodecGtidSet, UntaggedWireLayoutMatchesTheServerEncoding) {
  const std::vector<uint8_t> wire = {
      0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // n_sids = 2
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // SID 1
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,  //
      0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // n_intervals = 1
      0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // start = 1
      0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // end = 4 (exclusive)
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // SID 2
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,  //
      0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // n_intervals = 2
      0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // start = 5
      0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // end = 8
      0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // start = 9
      0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // end = 10
  };
  const std::string text = std::string(kSid) + ":1-3," + kSid2 + ":5-7:9-9";

  GtidSet decoded;
  ASSERT_EQ(GtidSet::DecodeBinary(wire.data(), wire.size(), &decoded), Status::kOk);
  EXPECT_EQ(decoded.ToString(), text);

  GtidSet parsed;
  ASSERT_EQ(GtidSet::Parse(text, &parsed), Status::kOk);
  std::vector<uint8_t> encoded;
  parsed.EncodeBinary(&encoded);
  EXPECT_EQ(encoded, wire);
}

TEST(CodecGtidSet, DecodeBinaryRejectsDamagedEncodings) {
  GtidSet set;
  ASSERT_EQ(GtidSet::Parse(std::string(kSid) + ":1-3", &set), Status::kOk);
  std::vector<uint8_t> wire;
  set.EncodeBinary(&wire);

  GtidSet out;
  EXPECT_EQ(GtidSet::DecodeBinary(wire.data(), 7, &out), Status::kTruncated);
  EXPECT_EQ(GtidSet::DecodeBinary(wire.data(), wire.size() - 1, &out), Status::kTruncated);

  std::vector<uint8_t> trailing = wire;
  trailing.push_back(0);
  EXPECT_EQ(GtidSet::DecodeBinary(trailing.data(), trailing.size(), &out), Status::kMalformedValue);

  std::vector<uint8_t> format2 = wire;
  format2[7] = 2;
  EXPECT_EQ(GtidSet::DecodeBinary(format2.data(), format2.size(), &out), Status::kUnsupported);

  std::vector<uint8_t> inverted = wire;
  inverted[32] = 4;  // start 4 == end 4
  EXPECT_EQ(GtidSet::DecodeBinary(inverted.data(), inverted.size(), &out), Status::kMalformedValue);

  std::vector<uint8_t> signed_end = wire;
  signed_end[47] = 0x80;  // end above INT64_MAX reads negative upstream
  EXPECT_EQ(GtidSet::DecodeBinary(signed_end.data(), signed_end.size(), &out),
            Status::kMalformedValue);
}

// --- Text forms ---

TEST(CodecGtidSet, ParseTreatsOnlyAnEmptyStringAsTheEmptySet) {
  GtidSet empty;
  ASSERT_EQ(GtidSet::Parse("", &empty), Status::kOk);
  EXPECT_TRUE(empty.empty());
  EXPECT_EQ(empty.ToString(), "");

  for (const char* text : {",", ",,", " ", "  ", "\t", "\n", "\r", " , ", ",\n,"}) {
    GtidSet set;
    EXPECT_EQ(GtidSet::Parse(text, &set), Status::kMalformedValue) << "input: '" << text << "'";
  }
}

TEST(CodecGtidSet, ParseRejectsEveryIntervalTokenThatIsNotABareDecimal) {
  const char* const rejected[] = {
      "+5",
      "-5",
      "5+",
      "1-+9",
      "1--9",
      "1- 9",
      "1 -9",
      "5 6",
      "0x5",
      "5abc",
      "abc",
      " ",
      "",
      "9223372036854775807",
      "18446744073709551615",
      "18446744073709551616",
      "-18446744073709551615",
      "1-18446744073709551615",
      "1-18446744073709551616",
      "1--18446744073709551615",
      "99999999999999999999999999999",
  };
  for (const char* intervals : rejected) {
    const std::string text = std::string(kSid) + ":" + intervals;
    GtidSet set;
    EXPECT_EQ(GtidSet::Parse(text, &set), Status::kMalformedValue) << "input: '" << text << "'";
  }
}

TEST(CodecGtidSet, ParseKeepsEveryWellFormedFormAccepted) {
  const std::string sid(kSid);
  const std::string sid2(kSid2);
  const std::pair<std::string, std::string> cases[] = {
      {sid + ":5", sid + ":5-5"},
      {sid + ":1-3", sid + ":1-3"},
      {sid + ":1-3:5-7", sid + ":1-3:5-7"},
      {sid + ":MyTag:1-5", sid + ":mytag:1-5"},
      {sid + ":_tag_2:7", sid + ":_tag_2:7-7"},
      {sid + ":1-3,\n" + sid2 + ":5-7", sid + ":1-3," + sid2 + ":5-7"},
      {sid + ":1-3, " + sid2 + ":tag:5-7", sid + ":1-3," + sid2 + ":tag:5-7"},
      {"  " + sid + ":1-3  ", sid + ":1-3"},
      {sid + ":1-3,", sid + ":1-3"},
      {sid + ":9223372036854775805", sid + ":9223372036854775805-9223372036854775805"},
      {sid + ":1-9223372036854775805", sid + ":1-9223372036854775805"},
      // INT64_MAX - 1 is the largest GNO.
      {sid + ":9223372036854775806", sid + ":9223372036854775806-9223372036854775806"},
      {sid + ":1-9223372036854775806", sid + ":1-9223372036854775806"},
  };
  for (const auto& [text, canonical] : cases) {
    GtidSet set;
    ASSERT_EQ(GtidSet::Parse(text, &set), Status::kOk) << "input: '" << text << "'";
    EXPECT_EQ(set.ToString(), canonical) << "input: '" << text << "'";
  }
}

TEST(CodecGtidSet, ParseAcceptsEveryTagRunTheServerPrintsUnderOneUuid) {
  const std::string sid(kSid);
  const std::pair<std::string, std::string> cases[] = {
      {sid + ":5:mytag:1:other:3", sid + ":5-5," + sid + ":mytag:1-1," + sid + ":other:3-3"},
      {sid + ":a:1:b:2", sid + ":a:1-1," + sid + ":b:2-2"},
      {sid + ":1-3:7:Analytics:1-4:9,\n" + kSid2 + ":tag:2",
       sid + ":1-3:7-7," + sid + ":analytics:1-4:9-9," + kSid2 + ":tag:2-2"},
      {sid + ":a:1:b:2:a:5", sid + ":a:1-1:5-5," + sid + ":b:2-2"},
      {sid + ":tag:7:1-2", sid + ":tag:1-2:7-7"},
  };
  for (const auto& [text, canonical] : cases) {
    GtidSet from_server;
    GtidSet from_canonical;
    ASSERT_EQ(GtidSet::Parse(text, &from_server), Status::kOk) << "input: '" << text << "'";
    EXPECT_EQ(from_server.ToString(), canonical) << "input: '" << text << "'";
    ASSERT_EQ(GtidSet::Parse(canonical, &from_canonical), Status::kOk);
    std::vector<uint8_t> server_wire;
    std::vector<uint8_t> canonical_wire;
    from_server.EncodeBinary(&server_wire);
    from_canonical.EncodeBinary(&canonical_wire);
    EXPECT_EQ(server_wire, canonical_wire) << "input: '" << text << "'";
  }
}

TEST(CodecGtidSet, ParseRejectsEveryEmptyOrDanglingColonPiece) {
  const std::string sid(kSid);
  for (const std::string& text :
       {sid + ":1::2", sid + "::5", sid + ":1:", sid + ":tag:1:", sid + ":",
        sid + ":tag:", sid + ":tag", sid + ":a:b:1", sid + ":1:tag", sid + ":1: :2",
        sid + ":5:mytag:1:other:", sid + ":tag:other:5", sid + ":tag:0"}) {
    GtidSet set;
    EXPECT_EQ(GtidSet::Parse(text, &set), Status::kMalformedValue) << "input: '" << text << "'";
  }
}

// --- Set operations ---

TEST(CodecGtidSet, SubsetRequiresEveryTsidAndIntervalToBeCovered) {
  GtidSet subset;
  GtidSet superset;
  ASSERT_EQ(GtidSet::Parse(std::string(kSid) + ":2-4," + kSid2 + ":tag:7-9", &subset), Status::kOk);
  ASSERT_EQ(GtidSet::Parse(std::string(kSid) + ":1-6," + kSid2 + ":tag:1-10", &superset),
            Status::kOk);
  EXPECT_TRUE(subset.IsSubsetOf(superset));
  EXPECT_FALSE(superset.IsSubsetOf(subset));

  GtidSet wrong_tag;
  ASSERT_EQ(GtidSet::Parse(std::string(kSid2) + ":other:7-9", &wrong_tag), Status::kOk);
  EXPECT_FALSE(subset.IsSubsetOf(wrong_tag));

  // A tagged interval is a different source from the untagged one.
  GtidSet tagged;
  GtidSet untagged;
  ASSERT_EQ(GtidSet::Parse(std::string(kSid) + ":tag:7", &tagged), Status::kOk);
  ASSERT_EQ(GtidSet::Parse(std::string(kSid) + ":1-10", &untagged), Status::kOk);
  EXPECT_FALSE(tagged.IsSubsetOf(untagged));
}

TEST(CodecGtidSet, SubsetComparesTagRunsTsidByTsid) {
  const std::string sid(kSid);
  GtidSet purged;
  ASSERT_EQ(GtidSet::Parse(sid + ":5:mytag:1:other:3", &purged), Status::kOk);
  GtidSet covering;
  ASSERT_EQ(GtidSet::Parse(sid + ":1-5:mytag:1-2:other:1-3", &covering), Status::kOk);
  EXPECT_TRUE(purged.IsSubsetOf(covering));
  GtidSet missing_one_tag;
  ASSERT_EQ(GtidSet::Parse(sid + ":1-5:mytag:1-2", &missing_one_tag), Status::kOk);
  EXPECT_FALSE(purged.IsSubsetOf(missing_one_tag));
}

TEST(CodecGtidSet, UppercaseUuidIsTheSameSource) {
  GtidSet required;
  GtidSet available;
  ASSERT_EQ(GtidSet::Parse("3E11FA47-71CA-11E1-9E33-C80AA9429562:5", &required), Status::kOk);
  ASSERT_EQ(GtidSet::Parse("3e11fa47-71ca-11e1-9e33-c80aa9429562:1-10", &available), Status::kOk);
  EXPECT_TRUE(required.IsSubsetOf(available));
  ASSERT_EQ(available.Merge(required), Status::kOk);
  EXPECT_EQ(available.ToString(), "3e11fa47-71ca-11e1-9e33-c80aa9429562:1-10");
}

TEST(CodecGtidSet, AddAndMergeKeepIntervalsCanonical) {
  Uuid uuid{};
  ASSERT_EQ(ParseUuid(kSid, &uuid), Status::kOk);
  GtidSet set;
  ASSERT_EQ(set.Add(uuid, "", {1, 4}), Status::kOk);
  ASSERT_EQ(set.Add(uuid, "", {4, 5}), Status::kOk);
  ASSERT_EQ(set.Add(uuid, "Tag", {7, 8}), Status::kOk);
  EXPECT_EQ(set.ToString(), std::string(kSid) + ":1-4," + kSid + ":tag:7-7");

  EXPECT_EQ(set.Add(uuid, "", {0, 2}), Status::kMalformedValue);
  EXPECT_EQ(set.Add(uuid, "", {3, 3}), Status::kMalformedValue);
  EXPECT_EQ(set.Add(uuid, "9tag", {1, 2}), Status::kMalformedValue);
  EXPECT_EQ(set.Add(uuid, "", {1, kGnoEnd + 1}), Status::kMalformedValue);

  GtidSet other;
  ASSERT_EQ(GtidSet::Parse(std::string(kSid) + ":tag:8", &other), Status::kOk);
  ASSERT_EQ(set.Merge(other), Status::kOk);
  EXPECT_EQ(set.ToString(), std::string(kSid) + ":1-4," + kSid + ":tag:7-8");

  set.Clear();
  EXPECT_TRUE(set.empty());
}

TEST(CodecGtidSet, IntervalCountIsBounded) {
  // Disjoint single GTIDs 1, 3, 5, ... so none merge.
  const auto text_with = [](size_t intervals) {
    std::string text(kSid);
    for (size_t i = 0; i < intervals; ++i) text += ":" + std::to_string(2 * i + 1);
    return text;
  };
  GtidSet set;
  ASSERT_EQ(GtidSet::Parse(text_with(GtidSet::kMaxIntervalsPerTsid), &set), Status::kOk);
  EXPECT_EQ(GtidSet::Parse(text_with(GtidSet::kMaxIntervalsPerTsid + 1), &set),
            Status::kLimitExceeded);

  Uuid uuid{};
  ASSERT_EQ(ParseUuid(kSid, &uuid), Status::kOk);
  const uint64_t next = 2 * GtidSet::kMaxIntervalsPerTsid + 1;
  EXPECT_EQ(set.Add(uuid, "", {next, next + 1}), Status::kLimitExceeded);
  GtidSet one;
  ASSERT_EQ(one.Add(uuid, "", {next, next + 1}), Status::kOk);
  EXPECT_EQ(set.Merge(one), Status::kLimitExceeded);
}

TEST(CodecGtidSet, SourcesListEachTsidWithMergedIntervals) {
  GtidSet set;
  ASSERT_EQ(GtidSet::Parse(std::string(kSid2) + ":5," + kSid + ":4-6:1-3:9:Tag:2", &set),
            Status::kOk);
  const auto& sources = set.sources();
  ASSERT_EQ(sources.size(), 3u);
  auto it = sources.begin();
  EXPECT_EQ(FormatUuid(it->first.uuid), kSid);
  EXPECT_EQ(it->first.tag, "");
  ASSERT_EQ(it->second.size(), 2u);
  EXPECT_EQ(it->second[0].start, 1u);
  EXPECT_EQ(it->second[0].end, 7u);
  EXPECT_EQ(it->second[1].start, 9u);
  EXPECT_EQ(it->second[1].end, 10u);
  ++it;
  EXPECT_EQ(FormatUuid(it->first.uuid), kSid);
  EXPECT_EQ(it->first.tag, "tag");
  ASSERT_EQ(it->second.size(), 1u);
  EXPECT_EQ(it->second[0].start, 2u);
  ++it;
  EXPECT_EQ(FormatUuid(it->first.uuid), kSid2);
  EXPECT_EQ(it->second[0].start, 5u);
  EXPECT_EQ(it->second[0].end, 6u);
}

TEST(CodecGtidSet, TagNormalization) {
  std::string tag;
  EXPECT_EQ(NormalizeGtidTag("MyTag", &tag), Status::kOk);
  EXPECT_EQ(tag, "mytag");
  EXPECT_EQ(NormalizeGtidTag(std::string(32, 'a'), &tag), Status::kOk);
  for (const std::string& bad : {std::string(), std::string(33, 'a'), std::string("9tag"),
                                 std::string("bad-tag"), std::string("t\xc3\xa9")}) {
    EXPECT_EQ(NormalizeGtidTag(bad, &tag), Status::kMalformedValue) << bad;
  }
}

}  // namespace
}  // namespace mybinlog
