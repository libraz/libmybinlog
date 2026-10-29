#include "mybinlog/decoder.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

#include "bytes_internal.h"
#include "mybinlog/disposition.h"
#include "mybinlog/event_types.h"
#include "mybinlog/format_description.h"
#include "mybinlog/heartbeat.h"
#include "mybinlog/mariadb_gtid.h"
#include "mybinlog/mysql_gtid.h"
#include "mybinlog/query_event.h"
#include "mybinlog/rotate.h"
#include "mybinlog/xid.h"

namespace mybinlog {
namespace {

// Rows-event post-header: 6-byte table id, then 2-byte flags.
constexpr size_t kRowsPostHeaderSize = 8;

// Parse one body with a low-level parser and store it as the event's variant alternative.
template <typename T, typename Parser>
Status ParseInto(Parser parser, const EventHeader& header, const uint8_t* body, size_t size,
                 Event* event) {
  T parsed;
  const Status status = parser(header, body, size, &parsed);
  if (status == Status::kOk) event->body = std::move(parsed);
  return status;
}

}  // namespace

Decoder::Decoder(const DecoderOptions& options) : options_(options) {
  tracker_.SetDefaultAlgorithm(options_.default_checksum);
  tracker_.SetVerify(options_.verify_checksum);
}

void Decoder::Feed(const uint8_t* data, size_t size) {
  if (size == 0) return;
  if (pos_ > 0) {
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(pos_));
    buffer_base_ += pos_;
    pos_ = 0;
  }
  buffer_.insert(buffer_.end(), data, data + size);
}

void Decoder::Reset() {
  tracker_.Reset();
  flavor_ = ServerFlavor::kMySQL;
  tables_.clear();
  buffer_.clear();
  pos_ = 0;
  buffer_base_ = 0;
  magic_checked_ = false;
  decompressed_ = DecompressedEvent();
  payload_.clear();
  payload_pos_ = 0;
  payload_offset_ = 0;
  error_ = DecodeError();
}

Status Decoder::Fail(Status status, uint64_t offset, const EventHeader* header,
                     const char* reason) {
  char prefix[96];
  if (header != nullptr) {
    std::snprintf(prefix, sizeof(prefix), "%s at offset %llu: ", EventTypeName(header->type_code),
                  static_cast<unsigned long long>(offset));
  } else {
    std::snprintf(prefix, sizeof(prefix),
                  "at offset %llu: ", static_cast<unsigned long long>(offset));
  }
  error_.status = status;
  error_.offset = offset;
  error_.message = prefix;
  error_.message += reason;
  error_.message += " (";
  error_.message += StatusName(status);
  error_.message += ")";
  return status;
}

Status Decoder::Next(Event* out) {
  if (error_.status != Status::kOk) return error_.status;
  while (true) {
    if (payload_pos_ < payload_.size()) {
      PayloadEventView inner;
      const Status status =
          NextPayloadEvent(payload_.data(), payload_.size(), &payload_pos_, &inner);
      if (status != Status::kOk) {
        return Fail(status, payload_offset_, nullptr,
                    "event inside a TRANSACTION_PAYLOAD does not frame");
      }
      return DecodeEvent(inner.header, inner.body, inner.size, payload_offset_, true, out);
    }
    payload_.clear();
    payload_pos_ = 0;

    const size_t available = buffer_.size() - pos_;
    const uint8_t* frame = buffer_.data() + pos_;
    if (!magic_checked_) {
      const size_t n = available < kBinlogMagic.size() ? available : kBinlogMagic.size();
      if (std::memcmp(frame, kBinlogMagic.data(), n) != 0) {
        magic_checked_ = true;
      } else if (n < kBinlogMagic.size()) {
        return Status::kNeedMoreData;
      } else {
        pos_ += kBinlogMagic.size();
        magic_checked_ = true;
        continue;
      }
    }
    if (available < kEventHeaderSize) return Status::kNeedMoreData;

    const uint64_t offset = buffer_base_ + pos_;
    EventHeader header;
    Status status = ParseEventHeader(frame, available, &header);
    if (status != Status::kOk) return Fail(status, offset, nullptr, "event header does not parse");
    if (header.event_length < kEventHeaderSize) {
      return Fail(Status::kMalformedValue, offset, &header,
                  "event_length is shorter than the 19-byte header");
    }
    if (header.event_length > options_.max_event_size) {
      return Fail(Status::kLimitExceeded, offset, &header,
                  "event_length exceeds DecoderOptions::max_event_size");
    }
    if (available < header.event_length) return Status::kNeedMoreData;

    FrameLayout layout;
    status = tracker_.OnEvent(header, frame, header.event_length, &layout);
    if (status != Status::kOk) {
      return Fail(status, offset, &header,
                  status == Status::kChecksumMismatch ? "checksum does not verify"
                                                      : "event does not frame");
    }
    pos_ += header.event_length;
    const uint8_t* body = frame + kEventHeaderSize;
    const size_t size = layout.body_length();

    if (options_.expand_compressed &&
        header.type_code == static_cast<uint8_t>(EventType::kTransactionPayloadEvent)) {
      TransactionPayloadEvent payload;
      status = ParseTransactionPayloadEvent(header, body, size, &payload);
      if (status != Status::kOk) return Fail(status, offset, &header, "body does not parse");
      status = DecompressTransactionPayload(payload, options_.max_decompressed_size, &payload_);
      if (status != Status::kOk) return Fail(status, offset, &header, "payload does not expand");
      payload_pos_ = 0;
      payload_offset_ = offset;
      continue;
    }
    return DecodeEvent(header, body, size, offset, false, out);
  }
}

Status Decoder::Finish() {
  if (error_.status != Status::kOk) return error_.status;
  const size_t available = buffered_bytes();
  if (available == 0) return Status::kOk;
  const uint64_t offset = buffer_base_ + pos_;
  if (!magic_checked_) {
    return Fail(Status::kTruncated, offset, nullptr, "stream ended inside the binlog magic");
  }
  EventHeader header;
  if (ParseEventHeader(buffer_.data() + pos_, available, &header) != Status::kOk) {
    return Fail(Status::kTruncated, offset, nullptr, "stream ended inside an event header");
  }
  std::string reason = "stream ended inside the event: " + std::to_string(available) + " of " +
                       std::to_string(header.event_length) + " bytes";
  return Fail(Status::kTruncated, offset, &header, reason.c_str());
}

Status Decoder::DecodeEvent(const EventHeader& header, const uint8_t* body, size_t size,
                            uint64_t offset, bool from_payload, Event* out) {
  if (options_.expand_compressed) {
    if (IsMariaDbCompressedEvent(header.type_code)) {
      const Status status = DecompressMariaDbEvent(header, body, size,
                                                   options_.max_decompressed_size, &decompressed_);
      if (status != Status::kOk) return Fail(status, offset, &header, "event does not expand");
      return DecodePlain(decompressed_.header, decompressed_.body.data(), decompressed_.body.size(),
                         offset, from_payload, out);
    }
    if (header.type_code == static_cast<uint8_t>(EventType::kTransactionPayloadEvent)) {
      return Fail(Status::kMalformedValue, offset, &header,
                  "TRANSACTION_PAYLOAD nested inside a TRANSACTION_PAYLOAD");
    }
  }
  return DecodePlain(header, body, size, offset, from_payload, out);
}

Status Decoder::DecodePlain(const EventHeader& header, const uint8_t* body, size_t size,
                            uint64_t offset, bool from_payload, Event* out) {
  Event event;
  event.header = header;
  event.offset = offset;
  event.from_payload = from_payload;
  event.disposition = ClassifyEvent(header.type_code, header.flags, flavor_);

  Status status = Status::kOk;
  switch (static_cast<EventType>(header.type_code)) {
    case EventType::kFormatDescriptionEvent: {
      FormatDescriptionEvent fde;
      status = ParseFormatDescriptionEvent(header, body, size, &fde);
      if (status == Status::kOk) {
        flavor_ = DetectFlavorFromVersion(fde.server_version);
        tables_.clear();
        event.body = std::move(fde);
      }
      break;
    }
    case EventType::kRotateEvent:
      status = ParseInto<RotateEvent>(ParseRotateEvent, header, body, size, &event);
      if (status == Status::kOk) tables_.clear();
      break;
    case EventType::kTableMapEvent: {
      TableMap map;
      status = ParseTableMapEvent(header, body, size, flavor_, &map);
      if (status == Status::kOk) {
        tables_[map.table_id] = map;
        event.body = std::move(map);
      }
      break;
    }
    case EventType::kWriteRowsEventV1:
    case EventType::kUpdateRowsEventV1:
    case EventType::kDeleteRowsEventV1:
    case EventType::kWriteRowsEvent:
    case EventType::kUpdateRowsEvent:
    case EventType::kDeleteRowsEvent:
    case EventType::kPartialUpdateRowsEvent:
      status = DecodeRows(header, body, size, offset, &event);
      if (status != Status::kOk) return status;  // DecodeRows reported it
      break;
    case EventType::kQueryEvent:
      status = ParseInto<QueryEvent>(ParseQueryEvent, header, body, size, &event);
      break;
    case EventType::kXidEvent:
      status = ParseInto<XidEvent>(ParseXidEvent, header, body, size, &event);
      break;
    case EventType::kGtidLogEvent:
    case EventType::kGtidTaggedLogEvent:
      status = ParseInto<GtidEvent>(ParseGtidEvent, header, body, size, &event);
      break;
    case EventType::kPreviousGtidsEvent:
      status = ParseInto<PreviousGtidsEvent>(ParsePreviousGtidsEvent, header, body, size, &event);
      break;
    case EventType::kMariaDBGtidEvent:
      status = ParseInto<MariaDbGtidEvent>(ParseMariaDbGtidEvent, header, body, size, &event);
      break;
    case EventType::kMariaDBGtidListEvent:
      status =
          ParseInto<MariaDbGtidListEvent>(ParseMariaDbGtidListEvent, header, body, size, &event);
      break;
    case EventType::kMariaDBAnnotateRowsEvent:
      status = ParseInto<MariaDbAnnotateRowsEvent>(ParseMariaDbAnnotateRowsEvent, header, body,
                                                   size, &event);
      break;
    case EventType::kHeartbeatLogEvent:
    case EventType::kHeartbeatLogEventV2:
      status = ParseInto<HeartbeatEvent>(ParseHeartbeatEvent, header, body, size, &event);
      break;
    default:
      event.body = UnknownEvent{body, size};
      break;
  }
  if (status != Status::kOk) return Fail(status, offset, &header, "body does not parse");
  *out = std::move(event);
  return Status::kOk;
}

Status Decoder::DecodeRows(const EventHeader& header, const uint8_t* body, size_t size,
                           uint64_t offset, Event* out) {
  if (body == nullptr || size < kRowsPostHeaderSize) {
    return Fail(Status::kTruncated, offset, &header, "body ends inside the post-header");
  }
  TableRowsEvent rows;
  rows.table_id = ReadU48Le(body);
  rows.flags = ReadU16Le(body + 6);
  rows.body = body;
  rows.size = size;
  const auto table = tables_.find(rows.table_id);
  if (table == tables_.end()) {
    if (rows.table_id != kDummyTableId) {
      const std::string reason =
          "no TABLE_MAP for table id " + std::to_string(rows.table_id) + " precedes the event";
      return Fail(Status::kInvalidMetadata, offset, &header, reason.c_str());
    }
    out->body = std::move(rows);
    return Status::kOk;
  }
  rows.table = &table->second;
  if (options_.decode_rows) {
    // A null budget means one per event; options_ is borrowed for the call only.
    DecodeBudget budget;
    const bool own_budget = options_.rows.budget == nullptr;
    if (own_budget) {
      const size_t floor = options_.rows.max_decompressed_bytes == 0
                               ? std::numeric_limits<size_t>::max()
                               : options_.rows.max_decompressed_bytes;
      budget = DecodeBudget::ForEventBody(size, floor);
      options_.rows.budget = &budget;
    }
    const Status status =
        DecodeRowsEvent(header, body, size, *rows.table, options_.rows, &rows.rows);
    if (own_budget) options_.rows.budget = nullptr;
    if (status != Status::kOk) return Fail(status, offset, &header, "rows do not decode");
    rows.decoded = true;
  }
  out->body = std::move(rows);
  return Status::kOk;
}

}  // namespace mybinlog
