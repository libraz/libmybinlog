add_executable(mybinlog-dump main.cpp)
# zlib reads gzip-compressed binlogs (and plain files) through gzread.
target_link_libraries(mybinlog-dump PRIVATE mybinlog::mybinlog ZLIB::ZLIB)
target_compile_options(mybinlog-dump PRIVATE -Wall -Wextra -Wpedantic)
