/**
 * @file test_include_audit.cpp
 * @brief Keeps the library self-contained: quoted includes resolve under
 *        include/ or src/, and public headers declare no virtual members.
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;

// tests/<this file> -> repository root.
fs::path RepoRoot() { return fs::path(__FILE__).parent_path().parent_path().lexically_normal(); }

// The error_code overloads keep this translation unit exception-free.
std::vector<fs::path> FilesUnder(const fs::path& root) {
  std::vector<fs::path> files;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
    if (it->is_regular_file(ec)) {
      files.push_back(it->path());
    }
  }
  return files;
}

bool IsSourceFile(const fs::path& path) {
  const std::string ext = path.extension().string();
  return ext == ".h" || ext == ".cpp";
}

// Returns the target of a `#include "..."` line, or empty if the line is not one.
std::string QuotedInclude(const std::string& line) {
  size_t i = line.find_first_not_of(" \t");
  if (i == std::string::npos || line[i] != '#') {
    return {};
  }
  i = line.find_first_not_of(" \t", i + 1);
  if (i == std::string::npos || line.compare(i, 7, "include") != 0) {
    return {};
  }
  i = line.find_first_not_of(" \t", i + 7);
  if (i == std::string::npos || line[i] != '"') {
    return {};
  }
  const size_t end = line.find('"', i + 1);
  if (end == std::string::npos) {
    return {};
  }
  return line.substr(i + 1, end - i - 1);
}

bool IsFile(const fs::path& path) {
  std::error_code ec;
  return fs::is_regular_file(path, ec);
}

TEST(CodecIncludeAudit, QuotedIncludesResolveInsideLibrary) {
  const fs::path include_root = RepoRoot() / "include";
  const fs::path src_root = RepoRoot() / "src";

  int scanned = 0;
  for (const fs::path& root : {include_root, src_root}) {
    const auto files = FilesUnder(root);
    ASSERT_FALSE(files.empty()) << "no files under " << root;
    for (const fs::path& file : files) {
      if (!IsSourceFile(file)) {
        continue;
      }
      std::ifstream in(file);
      ASSERT_TRUE(in.is_open()) << file;
      ++scanned;
      std::string line;
      while (std::getline(in, line)) {
        const std::string target = QuotedInclude(line);
        if (target.empty()) {
          continue;
        }
        const bool resolves = IsFile(file.parent_path() / target) ||
                              IsFile(include_root / target) || IsFile(src_root / target);
        EXPECT_TRUE(resolves) << file << ": \"" << target
                              << "\" does not resolve under include/ or src/";
      }
    }
  }
  EXPECT_GT(scanned, 0);
}

TEST(CodecIncludeAudit, PublicHeadersDeclareNoVirtual) {
  const fs::path include_root = RepoRoot() / "include";
  const auto files = FilesUnder(include_root);
  ASSERT_FALSE(files.empty()) << "no files under " << include_root;

  // Assembled from fragments so this file is not a hit in its own search.
  const std::string token = std::string("vir") + "tual";
  for (const fs::path& file : files) {
    if (file.extension() != ".h") {
      continue;
    }
    std::ifstream in(file);
    ASSERT_TRUE(in.is_open()) << file;
    std::string line;
    int hits = 0;
    while (std::getline(in, line)) {
      if (line.find(token) != std::string::npos) {
        ++hits;
      }
    }
    EXPECT_EQ(hits, 0) << file;
  }
}

}  // namespace
