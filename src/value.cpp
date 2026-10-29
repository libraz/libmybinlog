#include "mybinlog/value.h"

namespace mybinlog {

const char* ValueKindName(Value::Kind kind) noexcept {
  switch (kind) {
    case Value::Kind::kNull:
      return "kNull";
    case Value::Kind::kInt:
      return "kInt";
    case Value::Kind::kUInt:
      return "kUInt";
    case Value::Kind::kDouble:
      return "kDouble";
    case Value::Kind::kDecimal:
      return "kDecimal";
    case Value::Kind::kDate:
      return "kDate";
    case Value::Kind::kDateTime:
      return "kDateTime";
    case Value::Kind::kTime:
      return "kTime";
    case Value::Kind::kTimestamp:
      return "kTimestamp";
    case Value::Kind::kYear:
      return "kYear";
    case Value::Kind::kEnum:
      return "kEnum";
    case Value::Kind::kSet:
      return "kSet";
    case Value::Kind::kBit:
      return "kBit";
    case Value::Kind::kText:
      return "kText";
    case Value::Kind::kBytes:
      return "kBytes";
  }
  return "kUnknown";
}

}  // namespace mybinlog
