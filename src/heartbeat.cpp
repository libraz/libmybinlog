#include "mybinlog/heartbeat.h"

#include <utility>

#include "bytes_internal.h"
#include "mybinlog/event_types.h"

namespace mybinlog {
namespace {

// Field types of the v2 body (`Heartbeat::OTW_HB_*`, MySQL `codecs/binary.h`).
constexpr uint64_t kHeartbeatEndMark = 0;
constexpr uint64_t kHeartbeatLogFilename = 1;
constexpr uint64_t kHeartbeatLogPosition = 2;

Status ReadField(const uint8_t* body, size_t size, size_t* pos, uint64_t* value) {
  size_t consumed = 0;
  const Status status = ReadPackedInt(body + *pos, size - *pos, value, &consumed);
  if (status == Status::kOk) *pos += consumed;
  return status;
}

Status ParseV2(const uint8_t* body, size_t size, HeartbeatEvent* event) {
  size_t pos = 0;
  while (true) {
    if (pos == size) return Status::kTruncated;
    uint64_t type = 0;
    Status status = ReadField(body, size, &pos, &type);
    if (status != Status::kOk) return status;
    if (type == kHeartbeatEndMark) return Status::kOk;
    uint64_t length = 0;
    status = ReadField(body, size, &pos, &length);
    if (status != Status::kOk) return status;
    if (type == kHeartbeatLogPosition) {
      // Like the source's decoder, the position is read as a packed integer and its length ignored.
      status = ReadField(body, size, &pos, &event->position);
      if (status != Status::kOk) return status;
      continue;
    }
    if (length > size - pos) return Status::kTruncated;
    const auto field_size = static_cast<size_t>(length);
    if (type == kHeartbeatLogFilename) {
      event->log_name.assign(reinterpret_cast<const char*>(body + pos), field_size);
    }
    pos += field_size;
  }
}

}  // namespace

Status ParseHeartbeatEvent(const EventHeader& header, const uint8_t* body, size_t size,
                           HeartbeatEvent* out) {
  const bool v2 = header.type_code == static_cast<uint8_t>(EventType::kHeartbeatLogEventV2);
  if (!v2 && header.type_code != static_cast<uint8_t>(EventType::kHeartbeatLogEvent)) {
    return Status::kUnsupported;
  }
  if (body == nullptr && size != 0) return Status::kTruncated;
  HeartbeatEvent event;
  if (v2) {
    const Status status = ParseV2(body, size, &event);
    if (status != Status::kOk) return status;
  } else {
    if (size != 0) event.log_name.assign(reinterpret_cast<const char*>(body), size);
    event.position = header.next_position;
  }
  *out = std::move(event);
  return Status::kOk;
}

}  // namespace mybinlog
