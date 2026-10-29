#include "mybinlog/disposition.h"

#include <cstring>

namespace mybinlog {
namespace {

constexpr EventDisposition Framing(const char* r) { return {Disposition::kFraming, r}; }
constexpr EventDisposition Neutral(const char* r) { return {Disposition::kDataNeutral, r}; }
constexpr EventDisposition Closed(const char* r) { return {Disposition::kFailClosed, r}; }

constexpr const char* kForeignFlavor = "event type is not written by this server flavor";

}  // namespace

const char* DispositionName(Disposition disposition) noexcept {
  switch (disposition) {
    case Disposition::kFraming:
      return "kFraming";
    case Disposition::kTableMap:
      return "kTableMap";
    case Disposition::kRows:
      return "kRows";
    case Disposition::kGtid:
      return "kGtid";
    case Disposition::kTransactionBoundary:
      return "kTransactionBoundary";
    case Disposition::kQuery:
      return "kQuery";
    case Disposition::kDataNeutral:
      return "kDataNeutral";
    case Disposition::kFailClosed:
      return "kFailClosed";
  }
  return "kFailClosed";
}

EventDisposition ClassifyEvent(EventType type, ServerFlavor flavor) noexcept {
  const bool maria = flavor == ServerFlavor::kMariaDB;
  switch (type) {
    // Decoded and applied.
    case EventType::kQueryEvent:
      return {Disposition::kQuery, "statement, BEGIN or DDL text"};
    case EventType::kXidEvent:
      return {Disposition::kTransactionBoundary, "commits the open transaction"};
    case EventType::kTableMapEvent:
      return {Disposition::kTableMap, "describes the tables of the rows events that follow"};
    case EventType::kWriteRowsEventV1:
    case EventType::kUpdateRowsEventV1:
    case EventType::kDeleteRowsEventV1:
    case EventType::kWriteRowsEvent:
    case EventType::kUpdateRowsEvent:
    case EventType::kDeleteRowsEvent:
      return {Disposition::kRows, "row images"};
    case EventType::kGtidLogEvent:
      if (maria) {
        return Closed(kForeignFlavor);
      }
      return {Disposition::kGtid, "MySQL transaction position"};
    case EventType::kGtidTaggedLogEvent:
      if (maria) {
        return Closed(kForeignFlavor);
      }
      return {Disposition::kGtid, "MySQL tagged transaction position"};
    case EventType::kMariaDBGtidEvent:
      if (!maria) {
        return Closed(kForeignFlavor);
      }
      return {Disposition::kGtid, "MariaDB transaction position"};
    case EventType::kMariaDBGtidListEvent:
      if (!maria) {
        return Closed(kForeignFlavor);
      }
      return {Disposition::kGtid, "MariaDB position at the start of a file"};

    // Stream framing, consumed by the reader.
    case EventType::kStopEvent:
      return Framing("clean shutdown; the next file continues the same GTID stream");
    case EventType::kRotateEvent:
      return Framing("names the next binlog file");
    case EventType::kFormatDescriptionEvent:
      return Framing("describes the format and checksum of the file that follows");

    // Skipped: informational, no row image and no position of their own.
    case EventType::kHeartbeatLogEvent:
    case EventType::kHeartbeatLogEventV2:
      return Neutral("keepalive frame carrying no transaction");
    case EventType::kIntvarEvent:
    case EventType::kRandEvent:
    case EventType::kUserVarEvent:
      return Neutral("statement execution context; the statement is judged at its QUERY_EVENT");
    case EventType::kBeginLoadQueryEvent:
    case EventType::kAppendBlockEvent:
    case EventType::kDeleteFileEvent:
      return Neutral("stages LOAD DATA INFILE contents; EXECUTE_LOAD_QUERY applies them");
    case EventType::kIgnorableLogEvent:
      return Neutral("marked ignorable by the source");
    case EventType::kRowsQueryLogEvent:
      return Neutral("SQL text behind the rows events that follow");
    case EventType::kPreviousGtidsEvent:
      return Neutral("restates GTIDs of earlier files");
    case EventType::kTransactionContextEvent:
    case EventType::kViewChangeEvent:
      return Neutral("Group Replication bookkeeping carrying no row image");
    case EventType::kMariaDBAnnotateRowsEvent:
      return maria ? Neutral("SQL text behind the rows events that follow")
                   : Closed(kForeignFlavor);
    case EventType::kMariaDBBinlogCheckpointEvent:
      return maria ? Neutral("XA crash-recovery checkpoint") : Closed(kForeignFlavor);
    case EventType::kMariaDBStartEncryptionEvent:
      return maria ? Neutral("encrypted-file marker; the source decrypts before sending")
                   : Closed(kForeignFlavor);

    // Fail closed.
    case EventType::kUnknown:
    case EventType::kSlaveEvent:
    case EventType::kEnumEndEvent:
      return Closed("no supported server writes this type to a binlog");
    case EventType::kStartEventV3:
      return Closed("binlog format of a server older than MySQL 5.0");
    case EventType::kExecuteLoadQueryEvent:
      return Closed("statement-based LOAD DATA INFILE; its rows are never materialized");
    case EventType::kIncidentEvent:
      return Closed("the source declared that it lost events");
    case EventType::kAnonymousGtidLogEvent:
      return Closed("transaction without a GTID has no position to record");
    case EventType::kXaPrepareLogEvent:
      return Closed("prepared XA rows cannot be published before XA COMMIT");
    case EventType::kPartialUpdateRowsEvent:
      return Closed("partial JSON updates are not decoded");
    case EventType::kTransactionPayloadEvent:
      return Closed("compressed transaction payloads are not decoded");
    case EventType::kMariaDBQueryCompressedEvent:
    case EventType::kMariaDBWriteRowsCompressedEventV1:
    case EventType::kMariaDBUpdateRowsCompressedEventV1:
    case EventType::kMariaDBDeleteRowsCompressedEventV1:
    case EventType::kMariaDBWriteRowsCompressedEvent:
    case EventType::kMariaDBUpdateRowsCompressedEvent:
    case EventType::kMariaDBDeleteRowsCompressedEvent:
      return Closed("MariaDB compressed events are not decoded");
    case EventType::kMariaDBPartialRowDataEvent:
      return Closed("partial row data is not decoded");
  }
  return Closed("event type outside the enumeration");
}

EventDisposition ClassifyEvent(uint8_t type_code, uint16_t flags, ServerFlavor flavor) noexcept {
  // Code 0 is a real enumerator; every other unnamed code is outside EventType.
  if (std::strcmp(EventTypeName(type_code), "UNKNOWN") == 0) {
    if ((flags & kLogEventIgnorableFlag) != 0) {
      return Neutral("unrecognized type marked ignorable by the source");
    }
    return Closed("unrecognized type not marked ignorable");
  }
  return ClassifyEvent(static_cast<EventType>(type_code), flavor);
}

}  // namespace mybinlog
