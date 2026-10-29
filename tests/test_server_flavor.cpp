// Server flavor names, version-string detection and the gtid_binlog_pos probe.

#include <gtest/gtest.h>

#include "mybinlog/server_flavor.h"

namespace mybinlog {
namespace {

TEST(CodecServerFlavor, Name) {
  EXPECT_STREQ(ServerFlavorName(ServerFlavor::kMySQL), "MySQL");
  EXPECT_STREQ(ServerFlavorName(ServerFlavor::kMariaDB), "MariaDB");
}

TEST(CodecServerFlavor, DetectsMySQL) {
  EXPECT_EQ(DetectFlavorFromVersion("8.4.7"), ServerFlavor::kMySQL);
  EXPECT_EQ(DetectFlavorFromVersion("9.0.1"), ServerFlavor::kMySQL);
  EXPECT_EQ(DetectFlavorFromVersion("5.7.44"), ServerFlavor::kMySQL);
}

TEST(CodecServerFlavor, DetectsMariaDB) {
  EXPECT_EQ(DetectFlavorFromVersion("10.11.6-MariaDB"), ServerFlavor::kMariaDB);
  EXPECT_EQ(DetectFlavorFromVersion("11.4.0-MariaDB-1:11.4.0+maria~ubu2404"),
            ServerFlavor::kMariaDB);
  EXPECT_EQ(DetectFlavorFromVersion("10.6.12-MariaDB-log"), ServerFlavor::kMariaDB);
  EXPECT_EQ(DetectFlavorFromVersion("10.11.6-mariadb"), ServerFlavor::kMariaDB);
}

TEST(CodecServerFlavor, EmptyOrUnrecognizedVersionFallsBackToMySQL) {
  EXPECT_EQ(DetectFlavorFromVersion(""), ServerFlavor::kMySQL);
  EXPECT_EQ(DetectFlavorFromVersion("some-unexpected-string"), ServerFlavor::kMySQL);
}

TEST(CodecServerFlavor, ProbeClassification) {
  EXPECT_EQ(ClassifyFlavorProbe(true, true, 0), ServerFlavor::kMariaDB);
  EXPECT_EQ(ClassifyFlavorProbe(false, false, kErUnknownSystemVariable), ServerFlavor::kMySQL);
  EXPECT_EQ(kErUnknownSystemVariable, 1193u);
}

TEST(CodecServerFlavor, ProbeFailsClosedOnAnythingElse) {
  EXPECT_FALSE(ClassifyFlavorProbe(true, false, 0).has_value());
  EXPECT_FALSE(ClassifyFlavorProbe(false, false, 1045).has_value());  // access denied
  EXPECT_FALSE(ClassifyFlavorProbe(false, false, 2013).has_value());  // lost connection
  EXPECT_FALSE(ClassifyFlavorProbe(false, true, 0).has_value());
}

}  // namespace
}  // namespace mybinlog
