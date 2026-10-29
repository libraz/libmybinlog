// MariaDB GTID text, positions, coverage, and the GTID / GTID_LIST / ANNOTATE_ROWS bodies.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "mybinlog/event_header.h"
#include "mybinlog/event_types.h"
#include "mybinlog/mariadb_gtid.h"

namespace mybinlog {
namespace {

using Bytes = std::vector<uint8_t>;

void PutU32Le(Bytes* out, uint32_t value) {
  for (int i = 0; i < 4; ++i) out->push_back(static_cast<uint8_t>(value >> (8 * i)));
}

void PutU64Le(Bytes* out, uint64_t value) {
  for (int i = 0; i < 8; ++i) out->push_back(static_cast<uint8_t>(value >> (8 * i)));
}

MariaDbGtid Gtid(uint32_t domain, uint32_t server, uint64_t seq) {
  MariaDbGtid gtid;
  gtid.domain_id = domain;
  gtid.server_id = server;
  gtid.sequence_no = seq;
  return gtid;
}

MariaDbGtidPosition Position(const char* text) {
  MariaDbGtidPosition position;
  EXPECT_EQ(MariaDbGtidPosition::Parse(text, &position), Status::kOk) << text;
  return position;
}

// --- MariaDbGtid::Parse ---

TEST(CodecGtidMariaDb, ParseValid) {
  MariaDbGtid gtid;
  ASSERT_EQ(MariaDbGtid::Parse("0-1-42", &gtid), Status::kOk);
  EXPECT_EQ(gtid, Gtid(0, 1, 42));
  ASSERT_EQ(MariaDbGtid::Parse("100-4294967295-18446744073709551615", &gtid), Status::kOk);
  EXPECT_EQ(gtid, Gtid(100, 4294967295u, 18446744073709551615ULL));
  ASSERT_EQ(MariaDbGtid::Parse("0-0-0", &gtid), Status::kOk);
  EXPECT_EQ(gtid, Gtid(0, 0, 0));
  ASSERT_EQ(MariaDbGtid::Parse("  0-1-42  ", &gtid), Status::kOk);
  EXPECT_EQ(gtid, Gtid(0, 1, 42));
  ASSERT_EQ(MariaDbGtid::Parse("\n0-1-42\n", &gtid), Status::kOk);
  EXPECT_EQ(gtid, Gtid(0, 1, 42));
}

TEST(CodecGtidMariaDb, ParseInvalid) {
  for (const char* text :
       {"", "0-1", "0-1-2-3", "abc-1-42", "0-abc-42", "0-1-abc",
        "3e11fa47-71ca-11e1-9e33-c80aa9429562:1-77", "0--1-42", "4294967296-1-42",
        "0-4294967296-42", "0-1-18446744073709551616", "+0-1-42", "0-1-4 2", "0 -1-42"}) {
    MariaDbGtid gtid = Gtid(7, 7, 7);
    EXPECT_EQ(MariaDbGtid::Parse(text, &gtid), Status::kMalformedValue)
        << "input: '" << text << "'";
    EXPECT_EQ(gtid, Gtid(7, 7, 7)) << "input: '" << text << "'";
  }
}

TEST(CodecGtidMariaDb, IsMariaDbGtidFormat) {
  EXPECT_TRUE(MariaDbGtid::IsMariaDbGtidFormat("0-1-42"));
  EXPECT_TRUE(MariaDbGtid::IsMariaDbGtidFormat("100-200-300"));
  EXPECT_TRUE(MariaDbGtid::IsMariaDbGtidFormat("0-0-0"));
  EXPECT_TRUE(MariaDbGtid::IsMariaDbGtidFormat("  0-1-42  "));
  EXPECT_FALSE(MariaDbGtid::IsMariaDbGtidFormat("3e11fa47-71ca-11e1-9e33-c80aa9429562:1-77"));
  EXPECT_FALSE(MariaDbGtid::IsMariaDbGtidFormat(""));
  EXPECT_FALSE(MariaDbGtid::IsMariaDbGtidFormat("0-1"));
  EXPECT_FALSE(MariaDbGtid::IsMariaDbGtidFormat("0-1-2-3"));
  EXPECT_FALSE(MariaDbGtid::IsMariaDbGtidFormat("abc-1-42"));
  EXPECT_FALSE(MariaDbGtid::IsMariaDbGtidFormat("0-abc-42"));
  EXPECT_FALSE(MariaDbGtid::IsMariaDbGtidFormat("0-1-abc"));
}

TEST(CodecGtidMariaDb, ToStringAndEquality) {
  EXPECT_EQ(Gtid(0, 1, 42).ToString(), "0-1-42");
  EXPECT_EQ(Gtid(100, 4294967295u, 18446744073709551615ULL).ToString(),
            "100-4294967295-18446744073709551615");
  EXPECT_EQ(Gtid(0, 0, 0).ToString(), "0-0-0");
  EXPECT_EQ(Gtid(0, 1, 42), Gtid(0, 1, 42));
  EXPECT_NE(Gtid(0, 1, 42), Gtid(0, 1, 43));
  EXPECT_NE(Gtid(0, 1, 42), Gtid(1, 1, 42));
  EXPECT_NE(Gtid(0, 1, 42), Gtid(0, 2, 42));

  MariaDbGtid parsed;
  ASSERT_EQ(MariaDbGtid::Parse("0-1-42", &parsed), Status::kOk);
  EXPECT_EQ(parsed.ToString(), "0-1-42");
}

// --- MariaDbGtidPosition ---

TEST(CodecGtidMariaDb, PositionParse) {
  EXPECT_EQ(Position("0-1-42").gtids, (std::vector<MariaDbGtid>{Gtid(0, 1, 42)}));
  EXPECT_EQ(Position("0-1-42,1-2-100").gtids,
            (std::vector<MariaDbGtid>{Gtid(0, 1, 42), Gtid(1, 2, 100)}));
  EXPECT_EQ(Position("0-1-42,1-2-100,2-3-200").gtids.size(), 3u);
  EXPECT_EQ(Position("  0-1-42 , 1-2-100  ").gtids,
            (std::vector<MariaDbGtid>{Gtid(0, 1, 42), Gtid(1, 2, 100)}));
  EXPECT_EQ(Position("0-1-42,\n1-2-100").gtids.size(), 2u);
  EXPECT_EQ(Position("0-1-42,").gtids, (std::vector<MariaDbGtid>{Gtid(0, 1, 42)}));

  MariaDbGtidPosition position;
  EXPECT_EQ(MariaDbGtidPosition::Parse("0-1-42,invalid", &position), Status::kMalformedValue);
}

TEST(CodecGtidMariaDb, PositionTreatsOnlyTheEmptyStringAsTheEmptySet) {
  MariaDbGtidPosition empty;
  empty.gtids = {Gtid(7, 1, 10)};
  ASSERT_EQ(MariaDbGtidPosition::Parse("", &empty), Status::kOk);
  EXPECT_TRUE(empty.empty());

  for (const char* text : {",", ",,", " ", "  ", "\t", "\n", " , ", ", ,", ",\n,"}) {
    MariaDbGtidPosition position;
    position.gtids = {Gtid(7, 1, 10)};
    EXPECT_EQ(MariaDbGtidPosition::Parse(text, &position), Status::kMalformedValue)
        << "input: '" << text << "'";
    EXPECT_EQ(position.gtids, (std::vector<MariaDbGtid>{Gtid(7, 1, 10)}))
        << "input: '" << text << "'";
  }
}

TEST(CodecGtidMariaDb, PositionToString) {
  EXPECT_EQ(MariaDbGtidPosition{}.ToString(), "");
  MariaDbGtidPosition one;
  one.gtids = {Gtid(0, 1, 42)};
  EXPECT_EQ(one.ToString(), "0-1-42");
  MariaDbGtidPosition two;
  two.gtids = {Gtid(0, 1, 42), Gtid(1, 2, 100)};
  EXPECT_EQ(two.ToString(), "0-1-42,1-2-100");
  EXPECT_EQ(Position("0-1-42,1-2-100").ToString(), "0-1-42,1-2-100");
}

// --- PositionCovers ---

TEST(CodecGtidMariaDb, CoverageAllowsServerIdChangeAtSameOrLaterSequence) {
  EXPECT_TRUE(PositionCovers(Position("0-1-42,7-1-10"), Position("0-99-42,7-2-11")));
  EXPECT_TRUE(PositionCovers(Position("0-1-9,2-7-4"), Position("0-3-12,2-9-4,5-1-1")));
}

TEST(CodecGtidMariaDb, CoverageRejectsMissingDomainOrEarlierSequence) {
  EXPECT_FALSE(PositionCovers(Position("0-1-42,7-1-10"), Position("0-2-100")));
  EXPECT_FALSE(PositionCovers(Position("0-1-42"), Position("0-2-41")));
  EXPECT_FALSE(PositionCovers(Position("0-1-13,2-7-4"), Position("0-3-12,2-9-4")));
}

TEST(CodecGtidMariaDb, CoverageUsesTheHighestSequencePerDomain) {
  EXPECT_TRUE(PositionCovers(Position("0-1-42"), Position("0-1-10,0-2-50")));
  EXPECT_TRUE(PositionCovers(Position(""), Position("0-1-1")));
  EXPECT_TRUE(PositionCovers(Position(""), Position("")));
  EXPECT_FALSE(PositionCovers(Position("0-1-1"), Position("")));
}

// --- MARIADB_GTID_EVENT ---

Bytes GtidEventBody(uint32_t domain, uint64_t seq, uint8_t flags) {
  Bytes body;
  PutU64Le(&body, seq);
  PutU32Le(&body, domain);
  body.push_back(flags);
  return body;
}

EventHeader GtidHeader(uint32_t server_id) {
  EventHeader header;
  header.type_code = static_cast<uint8_t>(EventType::kMariaDBGtidEvent);
  header.server_id = server_id;
  return header;
}

TEST(CodecGtidMariaDb, GtidEvent) {
  struct Case {
    uint32_t domain;
    uint32_t server;
    uint64_t seq;
    const char* text;
  };
  for (const Case& c : {Case{0, 1, 42, "0-1-42"}, Case{100, 200, 999999999, "100-200-999999999"},
                        Case{0, 0, 0, "0-0-0"}, Case{7, 42, 12345, "7-42-12345"}}) {
    const Bytes body = GtidEventBody(c.domain, c.seq, 0);
    MariaDbGtidEvent event;
    ASSERT_EQ(ParseMariaDbGtidEvent(GtidHeader(c.server), body.data(), body.size(), &event),
              Status::kOk);
    EXPECT_EQ(event.gtid.ToString(), c.text);
    EXPECT_FALSE(event.standalone());
    MariaDbGtid parsed;
    ASSERT_EQ(MariaDbGtid::Parse(c.text, &parsed), Status::kOk);
    EXPECT_EQ(parsed, event.gtid);
  }
}

TEST(CodecGtidMariaDb, GtidEventMaxValuesRoundTrip) {
  const Bytes body = GtidEventBody(UINT32_MAX, UINT64_MAX, 0);
  MariaDbGtidEvent event;
  ASSERT_EQ(ParseMariaDbGtidEvent(GtidHeader(UINT32_MAX), body.data(), body.size(), &event),
            Status::kOk);
  MariaDbGtid parsed;
  ASSERT_EQ(MariaDbGtid::Parse(event.gtid.ToString(), &parsed), Status::kOk);
  EXPECT_EQ(parsed, Gtid(UINT32_MAX, UINT32_MAX, UINT64_MAX));
}

TEST(CodecGtidMariaDb, GtidEventStandaloneFlag) {
  const Bytes body = GtidEventBody(1, 100, 0x01);
  MariaDbGtidEvent event;
  ASSERT_EQ(ParseMariaDbGtidEvent(GtidHeader(2), body.data(), body.size(), &event), Status::kOk);
  EXPECT_EQ(event.gtid.ToString(), "1-2-100");
  EXPECT_EQ(event.flags, 0x01);
  EXPECT_TRUE(event.standalone());
}

TEST(CodecGtidMariaDb, GtidEventLength) {
  Bytes body = GtidEventBody(5, 50, 0);
  ASSERT_EQ(body.size(), 13u);
  MariaDbGtidEvent event;
  ASSERT_EQ(ParseMariaDbGtidEvent(GtidHeader(10), body.data(), body.size(), &event), Status::kOk);
  EXPECT_EQ(event.gtid.ToString(), "5-10-50");
  EXPECT_EQ(ParseMariaDbGtidEvent(GtidHeader(10), body.data(), 12, &event), Status::kTruncated);
  EXPECT_EQ(ParseMariaDbGtidEvent(GtidHeader(10), body.data(), 0, &event), Status::kTruncated);
  // The commit-id / padding bytes after flags2 are not needed.
  body.insert(body.end(), 6, 0);
  EXPECT_EQ(ParseMariaDbGtidEvent(GtidHeader(10), body.data(), body.size(), &event), Status::kOk);
}

// --- MARIADB_GTID_LIST_EVENT ---

Bytes GtidListBody(const std::vector<MariaDbGtid>& gtids, uint32_t flags = 0) {
  Bytes body;
  PutU32Le(&body, (flags << 28) | (static_cast<uint32_t>(gtids.size()) & 0x0FFFFFFFu));
  for (const auto& gtid : gtids) {
    PutU32Le(&body, gtid.domain_id);
    PutU32Le(&body, gtid.server_id);
    PutU64Le(&body, gtid.sequence_no);
  }
  return body;
}

TEST(CodecGtidMariaDb, GtidList) {
  for (const auto& gtids :
       {std::vector<MariaDbGtid>{}, std::vector<MariaDbGtid>{Gtid(0, 1, 42)},
        std::vector<MariaDbGtid>{Gtid(0, 1, 42), Gtid(1, 2, 100), Gtid(3, 5, 999)},
        std::vector<MariaDbGtid>{Gtid(UINT32_MAX, UINT32_MAX, UINT64_MAX)}}) {
    const Bytes body = GtidListBody(gtids);
    MariaDbGtidPosition position;
    ASSERT_EQ(ParseGtidListEvent(body.data(), body.size(), &position), Status::kOk);
    EXPECT_EQ(position.gtids, gtids);
  }
}

TEST(CodecGtidMariaDb, GtidListFlagsDoNotAffectTheCount) {
  const Bytes body = GtidListBody({Gtid(0, 1, 42)}, 0x5);
  MariaDbGtidPosition position;
  ASSERT_EQ(ParseGtidListEvent(body.data(), body.size(), &position), Status::kOk);
  EXPECT_EQ(position.gtids, (std::vector<MariaDbGtid>{Gtid(0, 1, 42)}));
}

TEST(CodecGtidMariaDb, GtidListToText) {
  const Bytes body = GtidListBody({Gtid(0, 1, 42), Gtid(1, 2, 100)});
  MariaDbGtidPosition position;
  ASSERT_EQ(ParseGtidListEvent(body.data(), body.size(), &position), Status::kOk);
  EXPECT_EQ(position.ToString(), "0-1-42,1-2-100");
}

TEST(CodecGtidMariaDb, GtidListTruncated) {
  MariaDbGtidPosition position;
  const Bytes short_body = {0, 0, 0};
  EXPECT_EQ(ParseGtidListEvent(short_body.data(), short_body.size(), &position),
            Status::kTruncated);

  Bytes body = GtidListBody({Gtid(0, 1, 42)});
  body[0] = 2;  // Claims two entries, holds one.
  EXPECT_EQ(ParseGtidListEvent(body.data(), body.size(), &position), Status::kTruncated);

  // The largest count cannot overflow the bound check.
  Bytes huge = GtidListBody({});
  huge[0] = huge[1] = huge[2] = 0xFF;
  huge[3] = 0x0F;
  EXPECT_EQ(ParseGtidListEvent(huge.data(), huge.size(), &position), Status::kTruncated);
}

// --- MARIADB_ANNOTATE_ROWS_EVENT ---

TEST(CodecGtidMariaDb, AnnotateRows) {
  for (const std::string& sql :
       {std::string("INSERT INTO t1 VALUES (1, 'hello')"),
        std::string("UPDATE articles SET content = 'new text', updated_at = NOW() WHERE id = 42"),
        std::string("INSERT INTO t1 VALUES (1, '\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e')"),
        std::string("DELETE FROM t1 WHERE id = 99")}) {
    std::string out;
    ASSERT_EQ(ExtractAnnotateRows(reinterpret_cast<const uint8_t*>(sql.data()), sql.size(), &out),
              Status::kOk);
    EXPECT_EQ(out, sql);
  }
}

TEST(CodecGtidMariaDb, AnnotateRowsRejectsAnEmptyBody) {
  const uint8_t body[] = {'x'};
  std::string out = "unchanged";
  EXPECT_EQ(ExtractAnnotateRows(body, 0, &out), Status::kTruncated);
  EXPECT_EQ(out, "unchanged");
}

}  // namespace
}  // namespace mybinlog
