#include "mybinlog/heartbeat.h"

#include <utility>

#include "mybinlog/event_types.h"

namespace mybinlog {

Status ParseHeartbeatEvent(const EventHeader& header, const uint8_t* body, size_t size,
                           HeartbeatEvent* out) {
  if (header.type_code != static_cast<uint8_t>(EventType::kHeartbeatLogEvent)) {
    return Status::kUnsupported;
  }
  if (body == nullptr && size != 0) return Status::kTruncated;
  HeartbeatEvent event;
  if (size != 0) event.log_name.assign(reinterpret_cast<const char*>(body), size);
  event.position = header.next_position;
  *out = std::move(event);
  return Status::kOk;
}

}  // namespace mybinlog
