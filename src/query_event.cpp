#include "mybinlog/query_event.h"

#include <cstring>
#include <utility>

#include "mybinlog/bytes.h"

namespace mybinlog {

namespace {

// Status variable codes: MySQL statement_events.h Query_event_status_vars,
// MariaDB sql/log_event.h.
constexpr uint8_t kFlags2 = 0;
constexpr uint8_t kSqlMode = 1;
constexpr uint8_t kCatalog = 2;  // Length, name, NUL (MySQL 5.0.0-5.0.3).
constexpr uint8_t kAutoIncrement = 3;
constexpr uint8_t kCharset = 4;
constexpr uint8_t kTimeZone = 5;
constexpr uint8_t kCatalogNz = 6;
constexpr uint8_t kLcTimeNames = 7;
constexpr uint8_t kCharsetDatabase = 8;
constexpr uint8_t kTableMapForUpdate = 9;
constexpr uint8_t kMasterDataWritten = 10;
constexpr uint8_t kInvoker = 11;
constexpr uint8_t kUpdatedDbNames = 12;
constexpr uint8_t kMicroseconds = 13;
constexpr uint8_t kExplicitDefaultsForTimestamp = 16;
constexpr uint8_t kDdlLoggedWithXid = 17;
constexpr uint8_t kDefaultCollationForUtf8mb4 = 18;
constexpr uint8_t kSqlRequirePrimaryKey = 19;
constexpr uint8_t kDefaultTableEncryption = 20;
constexpr uint8_t kEnableCascadeTriggers = 21;
constexpr uint8_t kMariaDbHrnow = 128;
constexpr uint8_t kMariaDbXid = 129;
constexpr uint8_t kMariaDbGtidFlags3 = 130;
constexpr uint8_t kMariaDbCharacterSetCollations = 131;
constexpr uint8_t kMariaDbDummy = 255;  // Padding to the end of the block.

// Q_UPDATED_DB_NAMES above this count carries the count alone (MAX_DBS_IN_EVENT_MTS).
constexpr uint8_t kMaxUpdatedDbNames = 16;
// Q_GTID_FLAGS3 bits followed by an 8-byte sequence number (FL_COMMIT_ALTER_E1,
// FL_ROLLBACK_ALTER_E1).
constexpr uint8_t kGtidFlags3AlterSequence = 4 | 8;

constexpr size_t kPostHeaderSize = 13;

Status WithinBlock(Status status) {
  return status == Status::kTruncated ? Status::kInvalidMetadata : status;
}

// One-byte length, then that many bytes.
Status SkipShortString(ByteReader* vars) {
  uint8_t length = 0;
  const Status status = vars->ReadU8(&length);
  if (status != Status::kOk) return status;
  return vars->Skip(length);
}

Status SkipUpdatedDbNames(ByteReader* vars) {
  uint8_t count = 0;
  Status status = vars->ReadU8(&count);
  if (status != Status::kOk) return status;
  if (count == 0) return Status::kInvalidMetadata;
  if (count > kMaxUpdatedDbNames) return Status::kOk;
  for (uint8_t i = 0; i < count; ++i) {
    const void* nul = std::memchr(vars->position(), 0, vars->remaining());
    if (nul == nullptr) return Status::kInvalidMetadata;
    status =
        vars->Skip(static_cast<size_t>(static_cast<const uint8_t*>(nul) - vars->position()) + 1);
    if (status != Status::kOk) return status;
  }
  return Status::kOk;
}

// Reads one variable; sets *stop when the walk must end here.
Status ReadStatusVar(ByteReader* vars, QueryEvent* event, bool* stop) {
  uint8_t code = 0;
  Status status = vars->ReadU8(&code);
  if (status != Status::kOk) return status;
  switch (code) {
    case kSqlMode:
      status = vars->ReadU64Le(&event->sql_mode);
      if (status == Status::kOk) event->has_sql_mode = true;
      return status;
    case kCharset: {
      const uint8_t* bytes = nullptr;
      status = vars->ReadBytes(6, &bytes);
      if (status != Status::kOk) return status;
      event->charset.client = ReadU16Le(bytes);
      event->charset.connection = ReadU16Le(bytes + 2);
      event->charset.server = ReadU16Le(bytes + 4);
      event->has_charset = true;
      return Status::kOk;
    }
    case kFlags2:
    case kAutoIncrement:
    case kMasterDataWritten:
      return vars->Skip(4);
    case kCatalog: {
      uint8_t length = 0;
      status = vars->ReadU8(&length);
      if (status != Status::kOk) return status;
      return vars->Skip(static_cast<size_t>(length) + 1);
    }
    case kTimeZone:
    case kCatalogNz:
      return SkipShortString(vars);
    case kLcTimeNames:
    case kCharsetDatabase:
    case kDefaultCollationForUtf8mb4:
      return vars->Skip(2);
    case kTableMapForUpdate:
    case kDdlLoggedWithXid:
    case kMariaDbXid:
      return vars->Skip(8);
    case kInvoker:
      status = SkipShortString(vars);  // user
      if (status != Status::kOk) return status;
      return SkipShortString(vars);  // host
    case kUpdatedDbNames:
      return SkipUpdatedDbNames(vars);
    case kMicroseconds:
    case kMariaDbHrnow:
      return vars->Skip(3);
    case kExplicitDefaultsForTimestamp:
    case kSqlRequirePrimaryKey:
    case kDefaultTableEncryption:
    case kEnableCascadeTriggers:
      return vars->Skip(1);
    case kMariaDbGtidFlags3: {
      uint8_t flags = 0;
      status = vars->ReadU8(&flags);
      if (status != Status::kOk) return status;
      return (flags & kGtidFlags3AlterSequence) != 0 ? vars->Skip(8) : Status::kOk;
    }
    case kMariaDbCharacterSetCollations: {
      uint8_t count = 0;
      status = vars->ReadU8(&count);
      if (status != Status::kOk) return status;
      return vars->Skip(static_cast<size_t>(count) * 4);
    }
    case kMariaDbDummy:
      *stop = true;
      event->status_vars_complete = true;
      return Status::kOk;
    default:
      // Servers write codes in increasing order and stop at one they do not know.
      *stop = true;
      return Status::kOk;
  }
}

}  // namespace

Status ParseQueryEvent(const uint8_t* body, size_t len, QueryEvent* out) {
  if (body == nullptr || len < kPostHeaderSize) return Status::kTruncated;
  QueryEvent event;
  event.thread_id = ReadU32Le(body);
  event.exec_time = ReadU32Le(body + 4);
  const uint8_t schema_length = body[8];
  event.error_code = ReadU16Le(body + 9);
  const uint16_t status_vars_length = ReadU16Le(body + 11);

  ByteReader reader(body + kPostHeaderSize, len - kPostHeaderSize);
  const uint8_t* status_vars = nullptr;
  Status status = reader.ReadBytes(status_vars_length, &status_vars);
  if (status != Status::kOk) return status;

  ByteReader vars(status_vars, status_vars_length);
  bool stop = false;
  while (!stop && vars.remaining() > 0) {
    status = WithinBlock(ReadStatusVar(&vars, &event, &stop));
    if (status != Status::kOk) return status;
  }
  if (!stop) event.status_vars_complete = true;

  // Schema, its NUL, then the statement to the end of the body.
  const uint8_t* schema = nullptr;
  status = reader.ReadBytes(static_cast<size_t>(schema_length) + 1, &schema);
  if (status != Status::kOk) return status;
  event.schema.assign(reinterpret_cast<const char*>(schema), schema_length);
  event.query.assign(reinterpret_cast<const char*>(reader.position()), reader.remaining());

  *out = std::move(event);
  return Status::kOk;
}

}  // namespace mybinlog
