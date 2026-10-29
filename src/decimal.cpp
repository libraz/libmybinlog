#include <array>

#include "decimal_internal.h"

namespace mybinlog {
namespace internal {

namespace {

constexpr uint32_t kMaxPrecision = 65;
constexpr uint32_t kMaxScale = 38;
constexpr int kDigitsPerGroup = 9;
constexpr size_t kGroupBytes = 4;
constexpr int kDigitBytes[kDigitsPerGroup + 1] = {0, 1, 1, 2, 2, 3, 3, 4, 4, 4};
constexpr uint32_t kPowers10[kDigitsPerGroup + 1] = {
    1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000};
// Above the widest valid encoding (DECIMAL(65,38) is 29 bytes).
constexpr size_t kMaxEncodedBytes = 40;

struct Layout {
  int int_groups;
  int int_rest;
  int frac_groups;
  int frac_rest;
};

Layout LayoutOf(uint32_t precision, uint32_t scale) {
  const int intg = static_cast<int>(precision - scale);
  const int frac = static_cast<int>(scale);
  return {intg / kDigitsPerGroup, intg % kDigitsPerGroup, frac / kDigitsPerGroup,
          frac % kDigitsPerGroup};
}

// Reads one big-endian group of @p digits digits; false when it holds more.
bool ReadGroup(const uint8_t** cursor, int digits, uint32_t* value) {
  const int bytes = digits == kDigitsPerGroup ? static_cast<int>(kGroupBytes) : kDigitBytes[digits];
  uint32_t group = 0;
  for (int i = 0; i < bytes; ++i) group = (group << 8) | *(*cursor)++;
  *value = group;
  return group < kPowers10[digits];
}

// Sign, every digit of DECIMAL(65, s) and the point.
constexpr size_t kMaxTextBytes = 1 + kMaxPrecision + 1;

char* WritePadded(char* out, uint32_t value, int digits) {
  for (int i = digits - 1; i >= 0; --i) {
    out[i] = static_cast<char>('0' + value % 10);
    value /= 10;
  }
  return out + digits;
}

char* WriteUnpadded(char* out, uint32_t value) {
  char buf[kDigitsPerGroup + 1];
  int n = 0;
  do {
    buf[n++] = static_cast<char>('0' + value % 10);
    value /= 10;
  } while (value != 0);
  while (n > 0) *out++ = buf[--n];
  return out;
}

}  // namespace

Status DecimalWidth(uint32_t meta, size_t* width) noexcept {
  const uint32_t precision = meta >> 8;
  const uint32_t scale = meta & 0xFF;
  if (precision == 0 || precision > kMaxPrecision || scale > precision || scale > kMaxScale) {
    return Status::kInvalidMetadata;
  }
  const Layout layout = LayoutOf(precision, scale);
  *width = static_cast<size_t>(layout.int_groups) * kGroupBytes +
           static_cast<size_t>(kDigitBytes[layout.int_rest]) +
           static_cast<size_t>(layout.frac_groups) * kGroupBytes +
           static_cast<size_t>(kDigitBytes[layout.frac_rest]);
  return Status::kOk;
}

Status DecodeDecimal(uint32_t meta, const uint8_t* data, std::string* out) {
  size_t width = 0;
  const Status status = DecimalWidth(meta, &width);
  if (status != Status::kOk) return status;
  const Layout layout = LayoutOf(meta >> 8, meta & 0xFF);

  std::array<uint8_t, kMaxEncodedBytes> buf{};
  for (size_t i = 0; i < width; ++i) buf[i] = data[i];
  const bool negative = (buf[0] & 0x80) == 0;
  buf[0] ^= 0x80;
  if (negative) {
    for (size_t i = 0; i < width; ++i) buf[i] ^= 0xFF;
  }

  // The text is built in place and copied out once: appending digit by digit
  // regrows the string.
  char text[kMaxTextBytes];
  char* end = text;
  if (negative) *end++ = '-';
  const char* const digits_begin = end;
  const uint8_t* cursor = buf.data();
  uint32_t group = 0;
  if (layout.int_rest > 0) {
    if (!ReadGroup(&cursor, layout.int_rest, &group)) return Status::kMalformedValue;
    if (group != 0) end = WriteUnpadded(end, group);
  }
  for (int i = 0; i < layout.int_groups; ++i) {
    if (!ReadGroup(&cursor, kDigitsPerGroup, &group)) return Status::kMalformedValue;
    if (end > digits_begin) {
      end = WritePadded(end, group, kDigitsPerGroup);
    } else if (group != 0) {
      end = WriteUnpadded(end, group);
    }
  }
  if (end == digits_begin) *end++ = '0';

  if (layout.frac_groups > 0 || layout.frac_rest > 0) {
    *end++ = '.';
    for (int i = 0; i < layout.frac_groups; ++i) {
      if (!ReadGroup(&cursor, kDigitsPerGroup, &group)) return Status::kMalformedValue;
      end = WritePadded(end, group, kDigitsPerGroup);
    }
    if (layout.frac_rest > 0) {
      if (!ReadGroup(&cursor, layout.frac_rest, &group)) return Status::kMalformedValue;
      end = WritePadded(end, group, layout.frac_rest);
    }
  }
  out->assign(text, static_cast<size_t>(end - text));
  return Status::kOk;
}

}  // namespace internal
}  // namespace mybinlog
