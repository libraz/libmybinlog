cmake_minimum_required(VERSION 3.18)
project(mybinlog VERSION 0.2.0 LANGUAGES CXX)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  set(MYBINLOG_IS_TOP_LEVEL ON)
else()
  set(MYBINLOG_IS_TOP_LEVEL OFF)
endif()

option(MYBINLOG_BUILD_TESTS "Build the mybinlog tests" ${MYBINLOG_IS_TOP_LEVEL})
option(MYBINLOG_INSTALL "Generate the mybinlog install rules" ${MYBINLOG_IS_TOP_LEVEL})
option(MYBINLOG_BUILD_EXAMPLES "Build the mybinlog examples" ${MYBINLOG_IS_TOP_LEVEL})
# Off when consumed so an embedding build never links a libzstd it did not ask for.
option(MYBINLOG_WITH_ZSTD "Decompress zstd TRANSACTION_PAYLOAD events" ${MYBINLOG_IS_TOP_LEVEL})

# Read when each target is created, so it is set before the libraries; `make lint` needs it.
if(MYBINLOG_BUILD_TESTS)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

include(GNUInstallDirs)

# The static and the shared library can both come from one configure; both are
# named libmybinlog and install side by side. The defaults build the one
# BUILD_SHARED_LIBS asks for.
if(BUILD_SHARED_LIBS)
  set(MYBINLOG_STATIC_DEFAULT OFF)
else()
  set(MYBINLOG_STATIC_DEFAULT ON)
endif()
option(MYBINLOG_BUILD_STATIC "Build the static library" ${MYBINLOG_STATIC_DEFAULT})
option(MYBINLOG_BUILD_SHARED "Build the shared library" ${BUILD_SHARED_LIBS})
if(NOT MYBINLOG_BUILD_STATIC AND NOT MYBINLOG_BUILD_SHARED)
  message(FATAL_ERROR "mybinlog: MYBINLOG_BUILD_STATIC and MYBINLOG_BUILD_SHARED are both OFF")
endif()

file(GLOB MYBINLOG_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)

# version.h is generated under the build tree; every other public header is in include/.
set(MYBINLOG_GENERATED_INCLUDE_DIR ${CMAKE_CURRENT_BINARY_DIR}/include)
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/include/mybinlog/version.h.in
  ${MYBINLOG_GENERATED_INCLUDE_DIR}/mybinlog/version.h
  @ONLY
)

if(NOT TARGET ZLIB::ZLIB)
  find_package(ZLIB REQUIRED)
endif()

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
    message(STATUS "mybinlog: zstd TRANSACTION_PAYLOAD support via ${MYBINLOG_ZSTD_FOUND_VIA}")
  else()
    message(WARNING "mybinlog: zstd not found; zstd TRANSACTION_PAYLOAD events are kUnsupported")
  endif()
endif()

# Defines mybinlog_<kind> (kind is static or shared) and its mybinlog:: alias.
function(mybinlog_add_library kind)
  string(TOUPPER ${kind} type)
  set(target mybinlog_${kind})
  add_library(${target} ${type} ${MYBINLOG_SOURCES})
  add_library(mybinlog::${target} ALIAS ${target})
  if(kind STREQUAL "static")
    # export.h leaves MYBINLOG_API empty for the static library and its consumers.
    target_compile_definitions(${target} PUBLIC MYBINLOG_STATIC_DEFINE)
  endif()
  target_include_directories(${target}
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<BUILD_INTERFACE:${MYBINLOG_GENERATED_INCLUDE_DIR}>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_compile_features(${target} PUBLIC cxx_std_17)
  target_compile_options(${target} PRIVATE
    -fno-exceptions
    -fno-rtti
    -Wall
    -Wextra
    -Wpedantic
    -Werror=switch
  )
  set_target_properties(${target} PROPERTIES
    OUTPUT_NAME mybinlog
    EXPORT_NAME ${target}
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
  )
  target_link_libraries(${target} PRIVATE ZLIB::ZLIB)
  if(MYBINLOG_ZSTD_FOUND_VIA)
    target_link_libraries(${target} PRIVATE ${MYBINLOG_ZSTD_TARGET})
    target_compile_definitions(${target} PRIVATE MYBINLOG_HAVE_ZSTD)
  endif()
endfunction()

if(MYBINLOG_BUILD_STATIC)
  mybinlog_add_library(static)
endif()
if(MYBINLOG_BUILD_SHARED)
  mybinlog_add_library(shared)
endif()

# mybinlog::mybinlog is the shared library when BUILD_SHARED_LIBS is ON or it is
# the only one built, the static one otherwise. The installed package applies the
# same rule.
if(MYBINLOG_BUILD_SHARED AND (BUILD_SHARED_LIBS OR NOT MYBINLOG_BUILD_STATIC))
  add_library(mybinlog::mybinlog ALIAS mybinlog_shared)
else()
  add_library(mybinlog::mybinlog ALIAS mybinlog_static)
endif()

if(MYBINLOG_INSTALL)
  include(CMakePackageConfigHelpers)
  set(MYBINLOG_CMAKE_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/mybinlog)
  set(MYBINLOG_PC_DIR ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

  install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/mybinlog
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h"
  )
  install(FILES ${MYBINLOG_GENERATED_INCLUDE_DIR}/mybinlog/version.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/mybinlog
  )

  # Every file a static install and a shared install both write has the same
  # content, so separate builds can install into one prefix. The per-kind
  # targets files are what mybinlogConfig.cmake looks for.
  if(MYBINLOG_BUILD_STATIC)
    install(TARGETS mybinlog_static EXPORT mybinlogStaticTargets
      ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(EXPORT mybinlogStaticTargets NAMESPACE mybinlog:: DESTINATION ${MYBINLOG_CMAKE_DIR})
    # The static library carries its zlib and zstd dependencies to the consumer's link.
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/mybinlogStaticDependencies.cmake.in
      ${CMAKE_CURRENT_BINARY_DIR}/mybinlogStaticDependencies.cmake @ONLY)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/mybinlogStaticDependencies.cmake
      DESTINATION ${MYBINLOG_CMAKE_DIR})
  endif()
  if(MYBINLOG_BUILD_SHARED)
    install(TARGETS mybinlog_shared EXPORT mybinlogSharedTargets
      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(EXPORT mybinlogSharedTargets NAMESPACE mybinlog:: DESTINATION ${MYBINLOG_CMAKE_DIR})
  endif()

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
  # mybinlog.pc links whichever library the linker prefers (the shared one when
  # both are installed) and lists the static dependencies for --static;
  # mybinlog-static.pc names the archive, so it links statically next to a
  # shared library too.
  file(RELATIVE_PATH MYBINLOG_PC_PREFIX_FROM_PCDIR
    ${CMAKE_INSTALL_FULL_LIBDIR}/pkgconfig ${CMAKE_INSTALL_PREFIX})
  string(REGEX REPLACE "/$" "" MYBINLOG_PC_PREFIX_FROM_PCDIR "${MYBINLOG_PC_PREFIX_FROM_PCDIR}")
  set(MYBINLOG_PC_ZSTD "")
  if(MYBINLOG_ZSTD_FOUND_VIA)
    set(MYBINLOG_PC_ZSTD "libzstd")
  endif()
  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/mybinlog.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/mybinlog.pc @ONLY)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/mybinlog.pc DESTINATION ${MYBINLOG_PC_DIR})
  if(MYBINLOG_BUILD_STATIC)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/mybinlog-static.pc.in
      ${CMAKE_CURRENT_BINARY_DIR}/mybinlog-static.pc @ONLY)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/mybinlog-static.pc DESTINATION ${MYBINLOG_PC_DIR})
  endif()
endif()

if(MYBINLOG_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(MYBINLOG_BUILD_TESTS)
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
