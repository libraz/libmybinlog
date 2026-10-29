#include "mybinlog/crc32.h"

#include "crc32_internal.h"

namespace mybinlog {

uint32_t UpdateCrc32(uint32_t crc, const void* data, size_t size) {
  return internal::Crc32Update(crc, data, size);
}

}  // namespace mybinlog
