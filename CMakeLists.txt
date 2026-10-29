cmake_minimum_required(VERSION 3.18)
project(mybinlog VERSION 0.1.0 LANGUAGES CXX)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  set(MYBINLOG_IS_TOP_LEVEL ON)
else()
  set(MYBINLOG_IS_TOP_LEVEL OFF)
endif()

option(MYBINLOG_BUILD_TESTS "Build the mybinlog tests" ${MYBINLOG_IS_TOP_LEVEL})
option(MYBINLOG_INSTALL "Generate the mybinlog install rules" ${MYBINLOG_IS_TOP_LEVEL})
# Off when consumed so an embedding build never links a libzstd it did not ask for.
option(MYBINLOG_WITH_ZSTD "Decompress zstd TRANSACTION_PAYLOAD events" ${MYBINLOG_IS_TOP_LEVEL})

include(GNUInstallDirs)
include(GenerateExportHeader)

file(GLOB MYBINLOG_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)

# Static or shared follows BUILD_SHARED_LIBS.
add_library(mybinlog ${MYBINLOG_SOURCES})
add_library(mybinlog::mybinlog ALIAS mybinlog)

# Generated public headers (export.h, version.h) live under the build tree.
set(MYBINLOG_GENERATED_INCLUDE_DIR ${CMAKE_CURRENT_BINARY_DIR}/include)
generate_export_header(mybinlog
  BASE_NAME MYBINLOG
  EXPORT_MACRO_NAME MYBINLOG_API
  EXPORT_FILE_NAME ${MYBINLOG_GENERATED_INCLUDE_DIR}/mybinlog/export.h
)
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/include/mybinlog/version.h.in
  ${MYBINLOG_GENERATED_INCLUDE_DIR}/mybinlog/version.h
  @ONLY
)

get_target_property(MYBINLOG_LIBRARY_TYPE mybinlog TYPE)
if(MYBINLOG_LIBRARY_TYPE STREQUAL "STATIC_LIBRARY")
  set(MYBINLOG_STATIC ON)
  # export.h expands MYBINLOG_API to nothing for static consumers.
  target_compile_definitions(mybinlog PUBLIC MYBINLOG_STATIC_DEFINE)
else()
  set(MYBINLOG_STATIC OFF)
endif()

target_include_directories(mybinlog
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${MYBINLOG_GENERATED_INCLUDE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
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
set_target_properties(mybinlog PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
)

if(NOT TARGET ZLIB::ZLIB)
  find_package(ZLIB REQUIRED)
endif()
target_link_libraries(mybinlog PRIVATE ZLIB::ZLIB)

# zstd: the package's CMake config when it ships one, pkg-config otherwise. How it
# was found is recorded so the installed package config can find it the same way.
set(MYBINLOG_ZSTD_FOUND_VIA "")
if(MYBINLOG_WITH_ZSTD)
  find_package(zstd CONFIG QUIET)
  if(TARGET zstd::libzstd_shared)
    set(MYBINLOG_ZSTD_TARGET zstd::libzstd_shared)
    set(MYBINLOG_ZSTD_FOUND_VIA config)
  elseif(TARGET zstd::libzstd_static)
    set(MYBINLOG_ZSTD_TARGET zstd::libzstd_static)
    set(MYBINLOG_ZSTD_FOUND_VIA config)
  else()
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
      pkg_check_modules(MYBINLOG_ZSTD QUIET IMPORTED_TARGET libzstd)
    endif()
    if(MYBINLOG_ZSTD_FOUND)
      set(MYBINLOG_ZSTD_TARGET PkgConfig::MYBINLOG_ZSTD)
      set(MYBINLOG_ZSTD_FOUND_VIA pkg-config)
    endif()
  endif()
  if(MYBINLOG_ZSTD_FOUND_VIA)
    target_link_libraries(mybinlog PRIVATE ${MYBINLOG_ZSTD_TARGET})
    target_compile_definitions(mybinlog PRIVATE MYBINLOG_HAVE_ZSTD)
    message(STATUS "mybinlog: zstd TRANSACTION_PAYLOAD support via ${MYBINLOG_ZSTD_FOUND_VIA}")
  else()
    message(WARNING "mybinlog: zstd not found; zstd TRANSACTION_PAYLOAD events are kUnsupported")
  endif()
endif()

if(MYBINLOG_INSTALL)
  include(CMakePackageConfigHelpers)
  set(MYBINLOG_CMAKE_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/mybinlog)

  install(TARGETS mybinlog
    EXPORT mybinlogTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  )
  install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/mybinlog
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h"
  )
  install(FILES
    ${MYBINLOG_GENERATED_INCLUDE_DIR}/mybinlog/export.h
    ${MYBINLOG_GENERATED_INCLUDE_DIR}/mybinlog/version.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/mybinlog
  )
  install(EXPORT mybinlogTargets
    NAMESPACE mybinlog::
    DESTINATION ${MYBINLOG_CMAKE_DIR}
  )

  configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/mybinlogConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/mybinlogConfig.cmake
    INSTALL_DESTINATION ${MYBINLOG_CMAKE_DIR}
  )
  # Pre-1.0 a minor release may change the API.
  write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/mybinlogConfigVersion.cmake
    COMPATIBILITY SameMinorVersion
  )
  install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/mybinlogConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/mybinlogConfigVersion.cmake
    DESTINATION ${MYBINLOG_CMAKE_DIR}
  )

  # pkg-config: prefix is derived from the .pc location so the install relocates.
  file(RELATIVE_PATH MYBINLOG_PC_PREFIX_FROM_PCDIR
    ${CMAKE_INSTALL_FULL_LIBDIR}/pkgconfig ${CMAKE_INSTALL_PREFIX})
  string(REGEX REPLACE "/$" "" MYBINLOG_PC_PREFIX_FROM_PCDIR "${MYBINLOG_PC_PREFIX_FROM_PCDIR}")
  set(MYBINLOG_PC_REQUIRES_PRIVATE "")
  if(MYBINLOG_STATIC)
    set(MYBINLOG_PC_CFLAGS " -DMYBINLOG_STATIC_DEFINE")
    set(MYBINLOG_PC_LIBS_PRIVATE "Libs.private: -lz")
    if(MYBINLOG_ZSTD_FOUND_VIA)
      set(MYBINLOG_PC_REQUIRES_PRIVATE "Requires.private: libzstd")
    endif()
  else()
    set(MYBINLOG_PC_CFLAGS "")
    set(MYBINLOG_PC_LIBS_PRIVATE "")
  endif()
  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/mybinlog.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/mybinlog.pc @ONLY)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/mybinlog.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
endif()

if(MYBINLOG_BUILD_TESTS)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
  enable_testing()

  include(FetchContent)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.18.0
  )
  # googletest must not land in mybinlog's install tree.
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  include(GoogleTest)

  add_subdirectory(tests)
endif()
