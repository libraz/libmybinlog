# Installs an already-built libmybinlog into a scratch prefix, then builds and
# runs tests/install/consumer against it with find_package (MYBINLOG_MODE
# find_package) or with pkg-config and the compiler directly (pkg_config).
# Run by ctest as `cmake -D<var>=<value>... -P run_install_test.cmake`.

foreach(var MYBINLOG_BUILD_DIR MYBINLOG_CONSUMER_DIR MYBINLOG_FIXTURE MYBINLOG_EXPECTED_VERSION
            MYBINLOG_LIBRARY_TYPE MYBINLOG_CXX_COMPILER MYBINLOG_GENERATOR MYBINLOG_MODE
            MYBINLOG_WORK_DIR)
  if(NOT DEFINED ${var})
    message(FATAL_ERROR "${var} is not set")
  endif()
endforeach()

function(run_checked)
  execute_process(COMMAND ${ARGN}
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(NOT result EQUAL 0)
    string(REPLACE ";" " " command "${ARGN}")
    message(FATAL_ERROR "failed (${result}): ${command}\n${output}\n${error}")
  endif()
  set(RUN_OUTPUT "${output}" PARENT_SCOPE)
endfunction()

# Exit code 77 is registered as SKIP_RETURN_CODE; older CMake can only pass.
macro(skip reason)
  message(STATUS "skipped: ${reason}")
  if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
    cmake_language(EXIT 77)
  endif()
  return()
endmacro()

set(prefix ${MYBINLOG_WORK_DIR}/prefix)
file(REMOVE_RECURSE ${MYBINLOG_WORK_DIR})
run_checked(${CMAKE_COMMAND} --install ${MYBINLOG_BUILD_DIR} --prefix ${prefix})

# The install carries the public and generated headers only.
foreach(header mybinlog.h export.h version.h format_description.h)
  if(NOT EXISTS ${prefix}/include/mybinlog/${header})
    message(FATAL_ERROR "include/mybinlog/${header} was not installed")
  endif()
endforeach()
foreach(stray include/mybinlog/bytes_internal.h include/mybinlog/version.h.in include/gtest
              include/gmock)
  if(EXISTS ${prefix}/${stray})
    message(FATAL_ERROR "${stray} must not be installed")
  endif()
endforeach()

string(REGEX MATCH "^[0-9]+\\.[0-9]+" major_minor ${MYBINLOG_EXPECTED_VERSION})
set(expected_output "mybinlog ${MYBINLOG_EXPECTED_VERSION} FORMAT_DESCRIPTION_EVENT 8.4")

if(MYBINLOG_MODE STREQUAL "find_package")
  set(build ${MYBINLOG_WORK_DIR}/build)
  run_checked(${CMAKE_COMMAND} -S ${MYBINLOG_CONSUMER_DIR} -B ${build} -G ${MYBINLOG_GENERATOR}
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=${MYBINLOG_CXX_COMPILER}
    -DCMAKE_PREFIX_PATH=${prefix} -DMYBINLOG_REQUIRED_VERSION=${major_minor})
  run_checked(${CMAKE_COMMAND} --build ${build})
  set(consumer ${build}/mybinlog_consumer)

  # SameMinorVersion: a request for the next minor must not match.
  string(REGEX MATCH "^[0-9]+" major ${major_minor})
  string(REGEX REPLACE "^[0-9]+\\." "" minor ${major_minor})
  math(EXPR next_minor "${minor} + 1")
  execute_process(
    COMMAND ${CMAKE_COMMAND} -S ${MYBINLOG_CONSUMER_DIR} -B ${MYBINLOG_WORK_DIR}/build-next
      -G ${MYBINLOG_GENERATOR} -DCMAKE_CXX_COMPILER=${MYBINLOG_CXX_COMPILER}
      -DCMAKE_PREFIX_PATH=${prefix} -DMYBINLOG_REQUIRED_VERSION=${major}.${next_minor}
    RESULT_VARIABLE result OUTPUT_QUIET ERROR_QUIET)
  if(result EQUAL 0)
    message(FATAL_ERROR "find_package(mybinlog ${major}.${next_minor}) accepted ${major_minor}")
  endif()
elseif(MYBINLOG_MODE STREQUAL "pkg_config")
  find_program(pkg_config NAMES pkg-config pkgconf)
  if(NOT pkg_config)
    skip("pkg-config not found")
  endif()
  file(GLOB_RECURSE pc_files ${prefix}/*/mybinlog.pc)
  list(LENGTH pc_files pc_count)
  if(NOT pc_count EQUAL 1)
    message(FATAL_ERROR "expected one mybinlog.pc under ${prefix}, found: ${pc_files}")
  endif()
  get_filename_component(pc_dir ${pc_files} DIRECTORY)
  set(pkg ${CMAKE_COMMAND} -E env PKG_CONFIG_PATH=${pc_dir} ${pkg_config})

  run_checked(${pkg} --modversion mybinlog)
  string(STRIP "${RUN_OUTPUT}" modversion)
  if(NOT modversion STREQUAL MYBINLOG_EXPECTED_VERSION)
    message(FATAL_ERROR "pkg-config version ${modversion}, expected ${MYBINLOG_EXPECTED_VERSION}")
  endif()
  run_checked(${pkg} --cflags mybinlog)
  separate_arguments(cflags UNIX_COMMAND "${RUN_OUTPUT}")
  if(MYBINLOG_LIBRARY_TYPE STREQUAL "STATIC_LIBRARY")
    run_checked(${pkg} --libs --static mybinlog)
    separate_arguments(libs UNIX_COMMAND "${RUN_OUTPUT}")
  else()
    run_checked(${pkg} --libs mybinlog)
    separate_arguments(libs UNIX_COMMAND "${RUN_OUTPUT}")
    run_checked(${pkg} --variable=libdir mybinlog)
    string(STRIP "${RUN_OUTPUT}" libdir)
    list(APPEND libs "-Wl,-rpath,${libdir}")
  endif()
  set(consumer ${MYBINLOG_WORK_DIR}/mybinlog_consumer)
  run_checked(${MYBINLOG_CXX_COMPILER} -std=c++17 ${cflags} ${MYBINLOG_CONSUMER_DIR}/main.cpp
    -o ${consumer} ${libs})
else()
  message(FATAL_ERROR "unknown MYBINLOG_MODE '${MYBINLOG_MODE}'")
endif()

run_checked(${consumer} ${MYBINLOG_FIXTURE})
string(FIND "${RUN_OUTPUT}" "${expected_output}" found)
if(NOT found EQUAL 0)
  message(FATAL_ERROR "consumer printed '${RUN_OUTPUT}', expected '${expected_output}...'")
endif()
message(STATUS "${RUN_OUTPUT}")
