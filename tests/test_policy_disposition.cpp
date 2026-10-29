// Event disposition: every EventType enumerator and unknown codes.

#include <gtest/gtest.h>

#include <cstring>

#include "mybinlog/disposition.h"

namespace mybinlog {
namespace {

using D = Disposition;

void Expect(EventType type, D mysql, D maria) {
  const auto code = static_cast<uint8_t>(type);
  const EventDisposition m = ClassifyEvent(type, ServerFlavor::kMySQL);
  const EventDisposition b = ClassifyEvent(type, ServerFlavor::kMariaDB);
  EXPECT_EQ(m.disposition, mysql) << EventTypeName(type) << " on MySQL";
  EXPECT_EQ(b.disposition, maria) << EventTypeName(type) << " on MariaDB";
  ASSERT_NE(m.reason, nullptr);
  ASSERT_NE(b.reason, nullptr);
  EXPECT_NE(std::strlen(m.reason), 0u);
  // The raw-code overload agrees for known types, with or without the ignorable flag.
  EXPECT_EQ(ClassifyEvent(code, 0, ServerFlavor::kMySQL).disposition, mysql) << int{code};
  EXPECT_EQ(ClassifyEvent(code, kLogEventIgnorableFlag, ServerFlavor::kMariaDB).disposition, maria)
      << int{code};
}

void Expect(EventType type, D both) { Expect(type, both, both); }

TEST(CodecPolicy, ClassifiesEveryEventType) {
  // Decoded and applied.
  Expect(EventType::kQueryEvent, D::kQuery);
  Expect(EventType::kXidEvent, D::kTransactionBoundary);
  Expect(EventType::kTableMapEvent, D::kTableMap);
  Expect(EventType::kWriteRowsEventV1, D::kRows);
  Expect(EventType::kUpdateRowsEventV1, D::kRows);
  Expect(EventType::kDeleteRowsEventV1, D::kRows);
  Expect(EventType::kWriteRowsEvent, D::kRows);
  Expect(EventType::kUpdateRowsEvent, D::kRows);
  Expect(EventType::kDeleteRowsEvent, D::kRows);
  Expect(EventType::kPartialUpdateRowsEvent, D::kRows);
  Expect(EventType::kGtidLogEvent, D::kGtid, D::kFailClosed);
  Expect(EventType::kGtidTaggedLogEvent, D::kGtid, D::kFailClosed);
  Expect(EventType::kMariaDBGtidEvent, D::kFailClosed, D::kGtid);
  Expect(EventType::kMariaDBGtidListEvent, D::kFailClosed, D::kGtid);

  // Compressed: expanded by compression.h, then classified by their inner type.
  Expect(EventType::kTransactionPayloadEvent, D::kCompressed);
  Expect(EventType::kMariaDBQueryCompressedEvent, D::kFailClosed, D::kCompressed);
  Expect(EventType::kMariaDBWriteRowsCompressedEventV1, D::kFailClosed, D::kCompressed);
  Expect(EventType::kMariaDBUpdateRowsCompressedEventV1, D::kFailClosed, D::kCompressed);
  Expect(EventType::kMariaDBDeleteRowsCompressedEventV1, D::kFailClosed, D::kCompressed);
  Expect(EventType::kMariaDBWriteRowsCompressedEvent, D::kFailClosed, D::kCompressed);
  Expect(EventType::kMariaDBUpdateRowsCompressedEvent, D::kFailClosed, D::kCompressed);
  Expect(EventType::kMariaDBDeleteRowsCompressedEvent, D::kFailClosed, D::kCompressed);

  // Framing.
  Expect(EventType::kStopEvent, D::kFraming);
  Expect(EventType::kRotateEvent, D::kFraming);
  Expect(EventType::kFormatDescriptionEvent, D::kFraming);

  // Data-neutral.
  Expect(EventType::kHeartbeatLogEvent, D::kDataNeutral);
  Expect(EventType::kHeartbeatLogEventV2, D::kDataNeutral);
  Expect(EventType::kIntvarEvent, D::kDataNeutral);
  Expect(EventType::kRandEvent, D::kDataNeutral);
  Expect(EventType::kUserVarEvent, D::kDataNeutral);
  Expect(EventType::kBeginLoadQueryEvent, D::kDataNeutral);
  Expect(EventType::kAppendBlockEvent, D::kDataNeutral);
  Expect(EventType::kDeleteFileEvent, D::kDataNeutral);
  Expect(EventType::kIgnorableLogEvent, D::kDataNeutral);
  Expect(EventType::kRowsQueryLogEvent, D::kDataNeutral);
  Expect(EventType::kPreviousGtidsEvent, D::kDataNeutral);
  Expect(EventType::kTransactionContextEvent, D::kDataNeutral);
  Expect(EventType::kViewChangeEvent, D::kDataNeutral);
  Expect(EventType::kMariaDBAnnotateRowsEvent, D::kFailClosed, D::kDataNeutral);
  Expect(EventType::kMariaDBBinlogCheckpointEvent, D::kFailClosed, D::kDataNeutral);
  Expect(EventType::kMariaDBStartEncryptionEvent, D::kFailClosed, D::kDataNeutral);

  // Fail closed.
  Expect(EventType::kUnknown, D::kFailClosed);
  Expect(EventType::kStartEventV3, D::kFailClosed);
  Expect(EventType::kSlaveEvent, D::kFailClosed);
  Expect(EventType::kExecuteLoadQueryEvent, D::kFailClosed);
  Expect(EventType::kIncidentEvent, D::kFailClosed);
  Expect(EventType::kAnonymousGtidLogEvent, D::kFailClosed);
  Expect(EventType::kXaPrepareLogEvent, D::kFailClosed);
  Expect(EventType::kMariaDBPartialRowDataEvent, D::kFailClosed);
  Expect(EventType::kEnumEndEvent, D::kFailClosed);
}

TEST(CodecPolicy, KnownTypesIgnoreTheIgnorableFlag) {
  // A flag on a type the library knows never downgrades a fail-closed type.
  const auto incident = static_cast<uint8_t>(EventType::kIncidentEvent);
  EXPECT_EQ(ClassifyEvent(incident, kLogEventIgnorableFlag, ServerFlavor::kMySQL).disposition,
            D::kFailClosed);
  const auto end = static_cast<uint8_t>(EventType::kEnumEndEvent);
  EXPECT_EQ(ClassifyEvent(end, kLogEventIgnorableFlag, ServerFlavor::kMySQL).disposition,
            D::kFailClosed);
}

TEST(CodecPolicy, UnknownCodesFollowTheIgnorableFlag) {
  for (const uint8_t code :
       {uint8_t{6}, uint8_t{43}, uint8_t{100}, uint8_t{159}, uint8_t{173}, uint8_t{254}}) {
    ASSERT_STREQ(EventTypeName(code), "UNKNOWN") << int{code};
    for (const ServerFlavor flavor : {ServerFlavor::kMySQL, ServerFlavor::kMariaDB}) {
      const EventDisposition plain = ClassifyEvent(code, 0, flavor);
      EXPECT_EQ(plain.disposition, D::kFailClosed) << int{code};
      EXPECT_NE(plain.reason, nullptr);
      const EventDisposition ignorable = ClassifyEvent(code, kLogEventIgnorableFlag, flavor);
      EXPECT_EQ(ignorable.disposition, D::kDataNeutral) << int{code};
      EXPECT_NE(ignorable.reason, nullptr);
      // Other flags do not make an unknown type skippable.
      EXPECT_EQ(ClassifyEvent(code, kLogEventArtificialFlag | kLogEventBinlogInUseFlag, flavor)
                    .disposition,
                D::kFailClosed);
      EXPECT_EQ(ClassifyEvent(code, 0xFFFF, flavor).disposition, D::kDataNeutral);
    }
  }
}

TEST(CodecPolicy, EveryCodeHasAReason) {
  for (unsigned code = 0; code < 256; ++code) {
    for (const ServerFlavor flavor : {ServerFlavor::kMySQL, ServerFlavor::kMariaDB}) {
      const EventDisposition d = ClassifyEvent(static_cast<uint8_t>(code), 0, flavor);
      ASSERT_NE(d.reason, nullptr) << code;
      EXPECT_NE(std::strlen(d.reason), 0u) << code;
    }
  }
}

TEST(CodecPolicy, DispositionNames) {
  EXPECT_STREQ(DispositionName(D::kFraming), "kFraming");
  EXPECT_STREQ(DispositionName(D::kTableMap), "kTableMap");
  EXPECT_STREQ(DispositionName(D::kRows), "kRows");
  EXPECT_STREQ(DispositionName(D::kGtid), "kGtid");
  EXPECT_STREQ(DispositionName(D::kTransactionBoundary), "kTransactionBoundary");
  EXPECT_STREQ(DispositionName(D::kQuery), "kQuery");
  EXPECT_STREQ(DispositionName(D::kCompressed), "kCompressed");
  EXPECT_STREQ(DispositionName(D::kDataNeutral), "kDataNeutral");
  EXPECT_STREQ(DispositionName(D::kFailClosed), "kFailClosed");
}

}  // namespace
}  // namespace mybinlog
