// Decodes a binlog file through an installed libmybinlog and prints the library
// version and the writer's version from its FORMAT_DESCRIPTION_EVENT.

#include <mybinlog/mybinlog.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <variant>
#include <vector>

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <binlog file>\n", argv[0]);
    return 2;
  }
  if (std::strcmp(mybinlog::Version(), MYBINLOG_VERSION_STRING) != 0) {
    std::fprintf(stderr, "header %s, library %s\n", MYBINLOG_VERSION_STRING, mybinlog::Version());
    return 1;
  }
  std::ifstream in(argv[1], std::ios::binary);
  const std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());

  mybinlog::Decoder decoder;
  decoder.Feed(file.data(), file.size());
  mybinlog::Event event;
  mybinlog::Status status;
  std::string server_version;
  while ((status = decoder.Next(&event)) == mybinlog::Status::kOk) {
    if (const auto* fde = std::get_if<mybinlog::FormatDescriptionEvent>(&event.body)) {
      if (server_version.empty()) server_version = fde->server_version;
    }
  }
  if (status == mybinlog::Status::kNeedMoreData) status = decoder.Finish();
  if (status != mybinlog::Status::kOk || server_version.empty()) {
    std::fprintf(stderr, "%s\n", decoder.last_error().message.c_str());
    return 1;
  }
  std::printf(
      "mybinlog %s %s %s\n", mybinlog::Version(),
      mybinlog::EventTypeName(static_cast<uint8_t>(mybinlog::EventType::kFormatDescriptionEvent)),
      server_version.c_str());
  return 0;
}
