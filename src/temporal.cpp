#include "bytes_internal.h"
#include "temporal_internal.h"

namespace mybinlog {
namespace internal {

namespace {

constexpr uint32_t kMaxFsp = 6;
constexpr uint64_t kMaxYear = 9999;
constexpr uint64_t kMaxMonth = 12;
constexpr uint64_t kMaxDay = 31;
constexpr uint64_t kMaxHour = 23;
constexpr uint64_t kMaxTimeHour = 838;
constexpr uint64_t kMaxMinuteOrSecond = 59;
constexpr uint64_t kMaxMicrosecond = 999999;

// DATETIMEF_INT_OFS, TIMEF_INT_OFS and TIMEF_OFS (mysys/my_time.cc).
constexpr int64_t kDatetimeIntOffset = 0x8000000000LL;
constexpr int64_t kTimeIntOffset = 0x800000LL;
constexpr int64_t kTimeOffset = 0x800000000000LL;
// A packed temporal keeps microseconds in its low 24 bits.
constexpr int64_t kFractionUnit = int64_t{1} << 24;

size_t FractionBytes(uint32_t fsp) { return (fsp + 1) / 2; }

uint64_t ReadBe(const uint8_t* data, size_t bytes) {
  uint64_t value = 0;
  for (size_t i = 0; i < bytes; ++i) value = (value << 8) | data[i];
  return value;
}

// Sign-extend the low @p bits of @p value.
int64_t SignExtend(uint64_t value, unsigned bits) {
  const uint64_t sign = uint64_t{1} << (bits - 1);
  return static_cast<int64_t>((value ^ sign) - sign);
}

bool ValidDate(uint64_t year, uint64_t month, uint64_t day) {
  return year <= kMaxYear && month <= kMaxMonth && day <= kMaxDay;
}

bool ValidClock(uint64_t hour, uint64_t minute, uint64_t second, uint64_t max_hour) {
  return hour <= max_hour && minute <= kMaxMinuteOrSecond && second <= kMaxMinuteOrSecond;
}

void SetDate(Temporal* t, uint64_t year, uint64_t month, uint64_t day) {
  t->year = static_cast<uint16_t>(year);
  t->month = static_cast<uint8_t>(month);
  t->day = static_cast<uint8_t>(day);
}

void SetClock(Temporal* t, uint64_t hour, uint64_t minute, uint64_t second) {
  t->hour = static_cast<uint16_t>(hour);
  t->minute = static_cast<uint8_t>(minute);
  t->second = static_cast<uint8_t>(second);
}

// Microseconds of a TIMESTAMP2 fraction stored in FractionBytes(fsp) bytes.
uint64_t UnsignedFractionMicros(uint64_t stored, uint32_t fsp) {
  switch (FractionBytes(fsp)) {
    case 1:
      return stored * 10000;
    case 2:
      return stored * 100;
    default:
      return stored;
  }
}

// my_datetime_packed_from_binary: the fraction is a signed big-endian number.
int64_t DatetimePacked(const uint8_t* data, uint32_t fsp) {
  const int64_t intpart = static_cast<int64_t>(ReadBe(data, 5)) - kDatetimeIntOffset;
  int64_t frac = 0;
  switch (FractionBytes(fsp)) {
    case 0:
      break;
    case 1:
      frac = SignExtend(data[5], 8) * 10000;
      break;
    case 2:
      frac = SignExtend(ReadBe(data + 5, 2), 16) * 100;
      break;
    default:
      frac = SignExtend(ReadBe(data + 5, 3), 24);
      break;
  }
  return intpart * kFractionUnit + frac;
}

// my_time_packed_from_binary: below fsp 5 a negative value stores its
// fraction complemented against the integer part.
int64_t TimePacked(const uint8_t* data, uint32_t fsp) {
  const size_t frac_bytes = FractionBytes(fsp);
  if (frac_bytes == 3) return static_cast<int64_t>(ReadBe(data, 6)) - kTimeOffset;
  int64_t intpart = static_cast<int64_t>(ReadBe(data, 3)) - kTimeIntOffset;
  if (frac_bytes == 0) return intpart * kFractionUnit;
  int64_t frac = static_cast<int64_t>(ReadBe(data + 3, frac_bytes));
  if (intpart < 0 && frac != 0) {
    ++intpart;
    frac -= int64_t{1} << (8 * frac_bytes);
  }
  return intpart * kFractionUnit + frac * (frac_bytes == 1 ? 10000 : 100);
}

Status DecodeDate(const uint8_t* data, Value* out) {
  const uint32_t packed = ReadU24Le(data);
  const uint64_t day = packed & 0x1F;
  const uint64_t month = (packed >> 5) & 0x0F;
  const uint64_t year = packed >> 9;
  if (!ValidDate(year, month, day)) return Status::kMalformedValue;
  out->kind = Value::Kind::kDate;
  SetDate(&out->t, year, month, day);
  return Status::kOk;
}

// Old TIME: a signed 24-bit integer whose decimal digits are HHMMSS.
Status DecodeTime(const uint8_t* data, Value* out) {
  const int64_t value = SignExtend(ReadU24Le(data), 24);
  const uint64_t magnitude = static_cast<uint64_t>(value < 0 ? -value : value);
  const uint64_t hour = magnitude / 10000;
  const uint64_t minute = magnitude / 100 % 100;
  const uint64_t second = magnitude % 100;
  if (!ValidClock(hour, minute, second, kMaxTimeHour)) return Status::kMalformedValue;
  out->kind = Value::Kind::kTime;
  out->t.negative = value < 0;
  SetClock(&out->t, hour, minute, second);
  return Status::kOk;
}

// Old DATETIME: an unsigned 64-bit integer whose decimal digits are YYYYMMDDhhmmss.
Status DecodeDatetime(const uint8_t* data, Value* out) {
  uint64_t value = ReadU64Le(data);
  const uint64_t second = value % 100;
  value /= 100;
  const uint64_t minute = value % 100;
  value /= 100;
  const uint64_t hour = value % 100;
  value /= 100;
  const uint64_t day = value % 100;
  value /= 100;
  const uint64_t month = value % 100;
  const uint64_t year = value / 100;
  if (!ValidDate(year, month, day) || !ValidClock(hour, minute, second, kMaxHour)) {
    return Status::kMalformedValue;
  }
  out->kind = Value::Kind::kDateTime;
  SetDate(&out->t, year, month, day);
  SetClock(&out->t, hour, minute, second);
  return Status::kOk;
}

Status DecodeTimestamp2(const uint8_t* data, uint32_t fsp, Value* out) {
  const uint32_t seconds = ReadU32Be(data);
  const uint64_t micros = UnsignedFractionMicros(ReadBe(data + 4, FractionBytes(fsp)), fsp);
  if (micros > kMaxMicrosecond) return Status::kMalformedValue;
  // Wire 0 is the zero timestamp; no instant within the first second exists.
  if (seconds == 0 && micros != 0) return Status::kMalformedValue;
  out->kind = Value::Kind::kTimestamp;
  out->epoch_seconds = seconds;
  out->microsecond = static_cast<uint32_t>(micros);
  out->fsp = static_cast<uint8_t>(fsp);
  return Status::kOk;
}

// Fields of a packed DATETIME2: year*13+month 17 bits, day 5, hour 5, minute 6, second 6.
Status DecodeDatetime2(const uint8_t* data, uint32_t fsp, Value* out) {
  const int64_t packed = DatetimePacked(data, fsp);
  if (packed < 0) return Status::kMalformedValue;
  const uint64_t micros = static_cast<uint64_t>(packed) & 0xFFFFFF;
  const uint64_t ymdhms = static_cast<uint64_t>(packed) >> 24;
  const uint64_t ymd = ymdhms >> 17;
  const uint64_t ym = ymd >> 5;
  const uint64_t hms = ymdhms & 0x1FFFF;
  const uint64_t year = ym / 13;
  const uint64_t month = ym % 13;
  const uint64_t day = ymd & 0x1F;
  const uint64_t hour = hms >> 12;
  const uint64_t minute = (hms >> 6) & 0x3F;
  const uint64_t second = hms & 0x3F;
  if (micros > kMaxMicrosecond || !ValidDate(year, month, day) ||
      !ValidClock(hour, minute, second, kMaxHour)) {
    return Status::kMalformedValue;
  }
  out->kind = Value::Kind::kDateTime;
  SetDate(&out->t, year, month, day);
  SetClock(&out->t, hour, minute, second);
  out->t.microsecond = static_cast<uint32_t>(micros);
  out->t.fsp = static_cast<uint8_t>(fsp);
  return Status::kOk;
}

// Fields of a packed TIME2: hour 10 bits, minute 6, second 6; the hour is not masked.
Status DecodeTime2(const uint8_t* data, uint32_t fsp, Value* out) {
  const int64_t packed = TimePacked(data, fsp);
  const uint64_t magnitude = static_cast<uint64_t>(packed < 0 ? -packed : packed);
  const uint64_t micros = magnitude & 0xFFFFFF;
  const uint64_t hms = magnitude >> 24;
  const uint64_t hour = hms >> 12;
  const uint64_t minute = (hms >> 6) & 0x3F;
  const uint64_t second = hms & 0x3F;
  if (micros > kMaxMicrosecond || !ValidClock(hour, minute, second, kMaxTimeHour)) {
    return Status::kMalformedValue;
  }
  out->kind = Value::Kind::kTime;
  out->t.negative = packed < 0;
  SetClock(&out->t, hour, minute, second);
  out->t.microsecond = static_cast<uint32_t>(micros);
  out->t.fsp = static_cast<uint8_t>(fsp);
  return Status::kOk;
}

}  // namespace

Status TemporalWidth(ColumnType type, uint32_t meta, size_t* width) noexcept {
  switch (type) {
    case ColumnType::kYear:
      *width = 1;
      return Status::kOk;
    case ColumnType::kDate:
    case ColumnType::kNewDate:
    case ColumnType::kTime:
      *width = 3;
      return Status::kOk;
    case ColumnType::kTimestamp:
      *width = 4;
      return Status::kOk;
    case ColumnType::kDatetime:
      *width = 8;
      return Status::kOk;
    case ColumnType::kTimestamp2:
    case ColumnType::kDatetime2:
    case ColumnType::kTime2: {
      if (meta > kMaxFsp) return Status::kInvalidMetadata;
      const size_t base = type == ColumnType::kTimestamp2  ? 4
                          : type == ColumnType::kDatetime2 ? 5
                                                           : 3;
      *width = base + FractionBytes(meta);
      return Status::kOk;
    }
    default:
      return Status::kUnsupported;
  }
}

Status DecodeTemporal(ColumnType type, uint32_t meta, const uint8_t* data, Value* out) noexcept {
  switch (type) {
    case ColumnType::kYear:
      out->kind = Value::Kind::kYear;
      out->u = data[0] == 0 ? 0 : 1900u + data[0];
      return Status::kOk;
    case ColumnType::kDate:
    case ColumnType::kNewDate:
      return DecodeDate(data, out);
    case ColumnType::kTime:
      return DecodeTime(data, out);
    case ColumnType::kDatetime:
      return DecodeDatetime(data, out);
    case ColumnType::kTimestamp:
      out->kind = Value::Kind::kTimestamp;
      out->epoch_seconds = ReadU32Le(data);
      return Status::kOk;
    case ColumnType::kTimestamp2:
      return DecodeTimestamp2(data, meta, out);
    case ColumnType::kDatetime2:
      return DecodeDatetime2(data, meta, out);
    case ColumnType::kTime2:
      return DecodeTime2(data, meta, out);
    default:
      return Status::kUnsupported;
  }
}

}  // namespace internal
}  // namespace mybinlog
