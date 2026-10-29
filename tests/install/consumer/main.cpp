// Reads the FORMAT_DESCRIPTION_EVENT of a binlog file through an installed
// libmybinlog and prints the library version and the writer's version.

#include <mybinlog/mybinlog.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
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
  if (file.size() < mybinlog::kBinlogMagicOffset ||
      std::memcmp(file.data(), mybinlog::kBinlogMagic.data(), mybinlog::kBinlogMagic.size()) != 0) {
    std::fprintf(stderr, "not a binlog file\n");
    return 1;
  }
  const uint8_t* frame = file.data() + mybinlog::kBinlogMagicOffset;
  const size_t available = file.size() - mybinlog::kBinlogMagicOffset;

  mybinlog::EventHeader header;
  if (mybinlog::ParseEventHeader(frame, available, &header) != mybinlog::Status::kOk ||
      header.type_code != static_cast<uint8_t>(mybinlog::EventType::kFormatDescriptionEvent) ||
      header.event_length > available) {
    std::fprintf(stderr, "no format description event\n");
    return 1;
  }
  mybinlog::ChecksumTracker tracker;
  mybinlog::FrameLayout layout;
  mybinlog::Status status = tracker.OnEvent(header, frame, header.event_length, &layout);
  mybinlog::FormatDescriptionEvent fde;
  if (status == mybinlog::Status::kOk) {
    status = mybinlog::ParseFormatDescriptionEvent(header, frame + mybinlog::kEventHeaderSize,
                                                   layout.body_length(), &fde);
  }
  if (status != mybinlog::Status::kOk) {
    std::fprintf(stderr, "%s\n", mybinlog::StatusName(status));
    return 1;
  }
  std::printf("mybinlog %s %s %s\n", mybinlog::Version(), mybinlog::EventTypeName(header.type_code),
              fde.server_version.c_str());
  return 0;
}
