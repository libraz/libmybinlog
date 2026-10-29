#include "mybinlog/event_header.h"

#include "bytes_internal.h"

namespace mybinlog {

Status ParseEventHeader(const uint8_t* data, size_t size, EventHeader* out) {
  if (data == nullptr || size < kEventHeaderSize) return Status::kTruncated;
  out->timestamp = ReadU32Le(data);
  out->type_code = ReadU8(data + 4);
  out->server_id = ReadU32Le(data + 5);
  out->event_length = ReadU32Le(data + 9);
  out->next_position = ReadU32Le(data + 13);
  out->flags = ReadU16Le(data + 17);
  return Status::kOk;
}

}  // namespace mybinlog
