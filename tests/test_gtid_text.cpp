// GTID position text across flavors: rows of models/gtid_text.json.

#include <gtest/gtest.h>

#include <string>

#include "mybinlog/mariadb_gtid.h"
#include "mybinlog/mysql_gtid.h"

namespace mybinlog {
namespace {

constexpr char kSid[] = "00000000-0000-0000-0000-000000000001";
constexpr char kSid2[] = "00000000-0000-0000-0000-000000000002";
constexpr char kInt64Max[] = "9223372036854775807";

struct Row {
  const char* flavor;
  const char* shape;
  const char* anomaly;
};

// models/gtid_text.json "rows", same order.
constexpr Row kRows[] = {
    {"MariaDB", "separators_only", "none"},
    {"MySQL", "separators_only", "none"},
    {"MariaDB", "multi_domain", "gno_int64max"},
    {"MySQL", "interval", "gno_int64max"},
    {"MariaDB", "single", "gno0"},
    {"MariaDB", "multi_domain", "gno0"},
    {"MariaDB", "empty", "none"},
    {"MariaDB", "multi_domain", "none"},
    {"MySQL", "interval", "none"},
    {"MySQL", "multi_uuid", "gno0"},
    {"MySQL", "single", "gno_int64max"},
    {"MySQL", "multi_uuid", "none"},
    {"MySQL", "multi_uuid", "gno_int64max"},
    {"MySQL", "tagged", "none"},
    {"MySQL", "tagged", "bad_tag"},
    {"MariaDB", "single", "none"},
    {"MySQL", "interval", "gno0"},
    {"MySQL", "empty", "none"},
    {"MySQL", "tagged", "gno0"},
    {"MySQL", "tagged", "gno_int64max"},
};

struct Expectation {
  std::string input;
  bool ok;
  std::string canonical;  ///< ToString() when ok.
};

Expectation Build(const Row& row) {
  const std::string shape = row.shape;
  const std::string anomaly = row.anomaly;
  const std::string sid(kSid);
  const std::string sid2(kSid2);
  if (shape == "empty") return {"", true, ""};
  if (shape == "separators_only") return {",", false, ""};

  if (std::string(row.flavor) == "MariaDB") {
    // Sequence numbers are unsigned 64-bit: 0 and INT64_MAX are ordinary values.
    const std::string seq = anomaly == "gno0" ? "0" : anomaly == "gno_int64max" ? kInt64Max : "5";
    if (shape == "single") return {" 0-1-" + seq + " ", true, "0-1-" + seq};
    return {"0-1-3, 1-2-" + seq, true, "0-1-3,1-2-" + seq};  // multi_domain
  }

  // MySQL: 1 <= GNO < INT64_MAX.
  const bool bad_gno = anomaly == "gno0" || anomaly == "gno_int64max";
  const std::string gno = anomaly == "gno0" ? "0" : anomaly == "gno_int64max" ? kInt64Max : "5";
  if (shape == "single") return {sid + ":" + gno, !bad_gno, sid + ":5-5"};
  if (shape == "interval") {
    const std::string text = anomaly == "gno0" ? sid + ":0-5" : sid + ":1-" + gno;
    return {text, !bad_gno, sid + ":1-5"};
  }
  if (shape == "multi_uuid") {
    return {sid + ":1-3,\n" + sid2 + ":" + gno, !bad_gno, sid + ":1-3," + sid2 + ":5-5"};
  }
  // tagged
  const std::string tag = anomaly == "bad_tag" ? "9tag" : "Tag";
  return {sid + ":" + tag + ":" + gno, !bad_gno && anomaly != "bad_tag", sid + ":tag:5-5"};
}

TEST(CodecGtidText, PairwiseTextModel) {
  for (const Row& row : kRows) {
    const Expectation expected = Build(row);
    SCOPED_TRACE(testing::Message() << row.flavor << "/" << row.shape << "/" << row.anomaly
                                    << " input '" << expected.input << "'");
    if (std::string(row.flavor) == "MariaDB") {
      MariaDbGtidPosition position;
      const Status status = MariaDbGtidPosition::Parse(expected.input, &position);
      EXPECT_EQ(status, expected.ok ? Status::kOk : Status::kMalformedValue);
      if (expected.ok) EXPECT_EQ(position.ToString(), expected.canonical);
      // A MariaDB position is never a MySQL set, so flavors cannot be confused.
      GtidSet set;
      if (!expected.input.empty()) {
        EXPECT_EQ(GtidSet::Parse(expected.input, &set), Status::kMalformedValue);
      }
    } else {
      GtidSet set;
      const Status status = GtidSet::Parse(expected.input, &set);
      EXPECT_EQ(status, expected.ok ? Status::kOk : Status::kMalformedValue);
      if (expected.ok) EXPECT_EQ(set.ToString(), expected.canonical);
      MariaDbGtidPosition position;
      if (!expected.input.empty()) {
        EXPECT_EQ(MariaDbGtidPosition::Parse(expected.input, &position), Status::kMalformedValue);
      }
    }
  }
}

}  // namespace
}  // namespace mybinlog
