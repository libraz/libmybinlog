# Out-of-tree consumer of an installed libmybinlog, driven by ../run_install_test.cmake.
cmake_minimum_required(VERSION 3.18)
project(mybinlog_consumer LANGUAGES CXX)

find_package(mybinlog ${MYBINLOG_REQUIRED_VERSION} REQUIRED CONFIG)

add_executable(mybinlog_consumer main.cpp)
target_compile_features(mybinlog_consumer PRIVATE cxx_std_17)
target_link_libraries(mybinlog_consumer PRIVATE mybinlog::mybinlog)
