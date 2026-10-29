#include "mybinlog/xid.h"

#include "bytes_internal.h"

namespace mybinlog {

Status ParseXidEvent(const EventHeader& /*header*/, const uint8_t* body, size_t size,
                     XidEvent* out) {
  constexpr size_t kXidSize = 8;
  if (body == nullptr || size < kXidSize) return Status::kTruncated;
  out->xid = ReadU64Le(body);
  return Status::kOk;
}

}  // namespace mybinlog
