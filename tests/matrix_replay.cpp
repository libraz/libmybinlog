#include "matrix_replay.h"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <system_error>

namespace mybinlog {
namespace matrix {

fs::path MatrixRoot() { return fs::path(__FILE__).parent_path() / "fixtures" / "matrix"; }

ServerFlavor MatrixConfig::flavor() const {
  return version.rfind("mariadb", 0) == 0 ? ServerFlavor::kMariaDB : ServerFlavor::kMySQL;
}

void PrintTo(const MatrixConfig& config, std::ostream* os) {
  *os << config.version << "/" << config.config;
}

std::vector<MatrixConfig> DiscoverConfigs() {
  std::vector<MatrixConfig> configs;
  std::error_code ec;
  for (fs::directory_iterator v(MatrixRoot(), ec), end; !ec && v != end; v.increment(ec)) {
    if (!v->is_directory(ec)) continue;
    std::error_code inner;
    for (fs::directory_iterator c(v->path(), inner); !inner && c != end; c.increment(inner)) {
      if (!c->is_directory(inner)) continue;
      configs.push_back({v->path().filename().string(), c->path().filename().string()});
    }
  }
  std::sort(configs.begin(), configs.end(), [](const MatrixConfig& a, const MatrixConfig& b) {
    return a.version != b.version ? a.version < b.version : a.config < b.config;
  });
  return configs;
}

std::vector<fs::path> BinlogFiles(const fs::path& dir) {
  std::vector<fs::path> files;
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = it->path().filename().string();
    if (name.rfind("binlog.", 0) == 0 && it->path().extension() == ".gz") {
      files.push_back(it->path());
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

bool ReadGzip(const fs::path& path, Bytes* out) {
  out->clear();
  gzFile file = gzopen(path.string().c_str(), "rb");
  if (file == nullptr) return false;
  uint8_t chunk[1 << 16];
  int n = 0;
  while ((n = gzread(file, chunk, sizeof(chunk))) > 0) out->insert(out->end(), chunk, chunk + n);
  const bool ok = n == 0;
  gzclose(file);
  return ok && !out->empty();
}

std::string SanitizedName(const MatrixConfig& config) {
  std::string name = config.version + "__" + config.config;
  for (char& c : name) {
    if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
  }
  return name;
}

std::string ConfigTestName(const testing::TestParamInfo<MatrixConfig>& info) {
  return SanitizedName(info.param);
}

// ---- JSON reader ----------------------------------------------------------------

const JsonNode* JsonNode::Get(const std::string& key) const {
  for (const auto& member : members) {
    if (member.first == key) return &member.second;
  }
  return nullptr;
}

namespace {

class JsonReader {
 public:
  explicit JsonReader(const std::string& s) : s_(s) {}

  bool Parse(JsonNode* out) {
    if (!Value(out, 0)) return false;
    Skip();
    return pos_ == s_.size();
  }

 private:
  static constexpr int kMaxDepth = 64;

  void Skip() {
    while (pos_ < s_.size() &&
           (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) {
      ++pos_;
    }
  }

  bool Eat(char c) {
    Skip();
    if (pos_ >= s_.size() || s_[pos_] != c) return false;
    ++pos_;
    return true;
  }

  bool Literal(const char* word) {
    const std::string w(word);
    if (s_.compare(pos_, w.size(), w) != 0) return false;
    pos_ += w.size();
    return true;
  }

  static void AppendUtf8(uint32_t cp, std::string* out) {
    if (cp < 0x80) {
      out->push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  bool Hex4(uint32_t* cp) {
    if (pos_ + 4 > s_.size()) return false;
    *cp = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = s_[pos_++];
      const int digit = c >= '0' && c <= '9'   ? c - '0'
                        : c >= 'a' && c <= 'f' ? c - 'a' + 10
                        : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                               : -1;
      if (digit < 0) return false;
      *cp = *cp * 16 + static_cast<uint32_t>(digit);
    }
    return true;
  }

  bool String(std::string* out) {
    if (!Eat('"')) return false;
    while (pos_ < s_.size() && s_[pos_] != '"') {
      const char c = s_[pos_++];
      if (c != '\\') {
        out->push_back(c);
        continue;
      }
      if (pos_ >= s_.size()) return false;
      const char e = s_[pos_++];
      switch (e) {
        case 'b':
          out->push_back('\b');
          break;
        case 'f':
          out->push_back('\f');
          break;
        case 'n':
          out->push_back('\n');
          break;
        case 'r':
          out->push_back('\r');
          break;
        case 't':
          out->push_back('\t');
          break;
        case 'u': {
          uint32_t cp = 0;
          if (!Hex4(&cp)) return false;
          if (cp >= 0xD800 && cp < 0xDC00) {
            uint32_t low = 0;
            if (!Literal("\\u") || !Hex4(&low) || low < 0xDC00 || low >= 0xE000) return false;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
          }
          AppendUtf8(cp, out);
          break;
        }
        default:
          out->push_back(e);
          break;
      }
    }
    return pos_ < s_.size() && s_[pos_++] == '"';
  }

  bool Value(JsonNode* out, int depth) {
    if (depth > kMaxDepth) return false;
    Skip();
    if (pos_ >= s_.size()) return false;
    const char c = s_[pos_];
    if (c == '"') {
      out->type = JsonNode::Type::kString;
      return String(&out->text);
    }
    if (c == '{' || c == '[') {
      const bool object = c == '{';
      out->type = object ? JsonNode::Type::kObject : JsonNode::Type::kArray;
      ++pos_;
      if (Eat(object ? '}' : ']')) return true;
      do {
        if (object) {
          out->members.emplace_back();
          if (!String(&out->members.back().first) || !Eat(':') ||
              !Value(&out->members.back().second, depth + 1)) {
            return false;
          }
        } else {
          out->items.emplace_back();
          if (!Value(&out->items.back(), depth + 1)) return false;
        }
      } while (Eat(','));
      return Eat(object ? '}' : ']');
    }
    if (Literal("null")) return out->type = JsonNode::Type::kNull, true;
    if (Literal("true")) return out->type = JsonNode::Type::kBool, out->text = "true", true;
    if (Literal("false")) return out->type = JsonNode::Type::kBool, out->text = "false", true;
    out->type = JsonNode::Type::kNumber;
    const size_t start = pos_;
    while (pos_ < s_.size() && std::string("+-0123456789.eE").find(s_[pos_]) != std::string::npos) {
      ++pos_;
    }
    out->text = s_.substr(start, pos_ - start);
    return pos_ > start;
  }

  const std::string& s_;
  size_t pos_ = 0;
};

const std::string* StringMember(const JsonNode& node, const std::string& key) {
  const JsonNode* member = node.Get(key);
  return member != nullptr && member->type == JsonNode::Type::kString ? &member->text : nullptr;
}

bool IntMember(const JsonNode& node, const std::string& key, int* out) {
  const JsonNode* member = node.Get(key);
  if (member == nullptr || member->type != JsonNode::Type::kNumber) return false;
  *out = std::atoi(member->text.c_str());
  return true;
}

bool ReadFile(const fs::path& path, std::string* out) {
  FILE* file = std::fopen(path.string().c_str(), "rb");
  if (file == nullptr) return false;
  char chunk[1 << 16];
  size_t n = 0;
  while ((n = std::fread(chunk, 1, sizeof(chunk), file)) > 0) out->append(chunk, n);
  const bool ok = std::ferror(file) == 0;
  std::fclose(file);
  return ok;
}

}  // namespace

bool ParseJson(const std::string& text, JsonNode* out) { return JsonReader(text).Parse(out); }

bool ReadJsonLines(const fs::path& path, std::vector<JsonNode>* out) {
  Bytes raw;
  if (!ReadGzip(path, &raw)) return false;
  const std::string all(raw.begin(), raw.end());
  size_t start = 0;
  while (start < all.size()) {
    size_t end = all.find('\n', start);
    if (end == std::string::npos) end = all.size();
    const std::string line = all.substr(start, end - start);
    start = end + 1;
    if (line.empty()) continue;
    JsonNode node;
    if (!ParseJson(line, &node)) return false;
    out->push_back(std::move(node));
  }
  return true;
}

const MetaTable* Meta::FindTable(const std::string& name) const {
  for (const MetaTable& table : tables) {
    if (table.name == name) return &table;
  }
  return nullptr;
}

bool ReadMeta(const fs::path& dir, Meta* out) {
  std::string text;
  JsonNode root;
  if (!ReadFile(dir / "meta.json", &text) || !ParseJson(text, &root)) return false;
  const std::string* flavor = StringMember(root, "flavor");
  const std::string* database = StringMember(root, "database");
  const JsonNode* tables = root.Get("tables");
  const JsonNode* transactions = root.Get("transactions");
  if (flavor == nullptr || database == nullptr || tables == nullptr || transactions == nullptr) {
    return false;
  }
  out->flavor = *flavor;
  out->database = *database;
  for (const JsonNode& t : tables->items) {
    MetaTable table;
    const std::string* name = StringMember(t, "name");
    const JsonNode* columns = t.Get("columns");
    if (name == nullptr || columns == nullptr) return false;
    table.name = *name;
    std::vector<std::pair<int, size_t>> pk;
    for (const JsonNode& c : columns->items) {
      MetaColumn column;
      const std::string* column_name = StringMember(c, "name");
      const std::string* norm = StringMember(c, "norm");
      const std::string* data_type = StringMember(c, "data_type");
      const std::string* column_type = StringMember(c, "column_type");
      const std::string* compressed = StringMember(c, "compressed");
      if (column_name == nullptr || norm == nullptr || data_type == nullptr ||
          column_type == nullptr || !IntMember(c, "pk_seq", &column.pk_seq)) {
        return false;
      }
      column.name = *column_name;
      column.norm = *norm;
      column.data_type = *data_type;
      column.column_type = *column_type;
      if (const std::string* charset = StringMember(c, "charset")) column.charset = *charset;
      column.compressed = compressed != nullptr && *compressed == "YES";
      if (column.pk_seq > 0) pk.emplace_back(column.pk_seq, table.columns.size());
      table.columns.push_back(std::move(column));
    }
    std::sort(pk.begin(), pk.end());
    for (const auto& entry : pk) table.primary_key.push_back(entry.second);
    if (table.primary_key.empty()) return false;
    out->tables.push_back(std::move(table));
  }
  for (const JsonNode& t : transactions->items) {
    MetaTransaction txn;
    const std::string* name = StringMember(t, "name");
    const std::string* kind = StringMember(t, "kind");
    const std::string* gtid_after = StringMember(t, "gtid_after");
    if (name == nullptr || kind == nullptr || gtid_after == nullptr ||
        !IntMember(t, "seq", &txn.seq)) {
      return false;
    }
    txn.name = *name;
    txn.kind = *kind;
    txn.gtid_after = *gtid_after;
    if (const JsonNode* emitted = t.Get("tables")) {
      for (const JsonNode& table : emitted->items) txn.tables.push_back(table.text);
    }
    out->transactions.push_back(std::move(txn));
  }
  return true;
}

bool ReadExpected(const fs::path& path, std::vector<ExpectedSnapshot>* out) {
  std::vector<JsonNode> lines;
  if (!ReadJsonLines(path, &lines)) return false;
  for (JsonNode& line : lines) {
    ExpectedSnapshot snapshot;
    const std::string* txn = StringMember(line, "txn");
    const std::string* table = StringMember(line, "table");
    const JsonNode* rows = line.Get("rows");
    if (txn == nullptr || table == nullptr || rows == nullptr ||
        rows->type != JsonNode::Type::kArray || !IntMember(line, "seq", &snapshot.seq)) {
      return false;
    }
    snapshot.txn = *txn;
    snapshot.table = *table;
    snapshot.rows = rows->items;
    out->push_back(std::move(snapshot));
  }
  return true;
}

}  // namespace matrix
}  // namespace mybinlog
