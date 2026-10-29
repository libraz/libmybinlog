cmake_minimum_required(VERSION 3.18)
project(mybinlog VERSION 0.1.0 LANGUAGES CXX)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  set(MYBINLOG_IS_TOP_LEVEL ON)
else()
  set(MYBINLOG_IS_TOP_LEVEL OFF)
endif()

option(MYBINLOG_BUILD_TESTS "Build the mybinlog tests" ${MYBINLOG_IS_TOP_LEVEL})

file(GLOB MYBINLOG_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)

add_library(mybinlog STATIC ${MYBINLOG_SOURCES})
add_library(mybinlog::mybinlog ALIAS mybinlog)

target_include_directories(mybinlog
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(mybinlog PUBLIC cxx_std_17)
target_compile_options(mybinlog PRIVATE
  -fno-exceptions
  -fno-rtti
  -Wall
  -Wextra
  -Wpedantic
  -Werror=switch
)
set_target_properties(mybinlog PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(NOT TARGET ZLIB::ZLIB)
  find_package(ZLIB REQUIRED)
endif()
target_link_libraries(mybinlog PRIVATE ZLIB::ZLIB)

if(MYBINLOG_BUILD_TESTS)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
  enable_testing()

  include(FetchContent)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.18.0
  )
  FetchContent_MakeAvailable(googletest)
  include(GoogleTest)

  add_subdirectory(tests)
endif()
