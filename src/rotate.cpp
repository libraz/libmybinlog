#include "mybinlog/rotate.h"

#include "bytes_internal.h"

namespace mybinlog {

Status ParseRotateEvent(const EventHeader& /*header*/, const uint8_t* body, size_t size,
                        RotateEvent* out) {
  constexpr size_t kPositionSize = 8;
  if (body == nullptr || size < kPositionSize) return Status::kTruncated;
  out->position = ReadU64Le(body);
  size_t name_size = size - kPositionSize;
  // Some servers terminate the name with a NUL.
  if (name_size > 0 && body[kPositionSize + name_size - 1] == '\0') --name_size;
  out->next_file.assign(reinterpret_cast<const char*>(body + kPositionSize), name_size);
  return Status::kOk;
}

}  // namespace mybinlog
