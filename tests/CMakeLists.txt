file(GLOB MYBINLOG_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp)

add_executable(mybinlog_tests ${MYBINLOG_TEST_SOURCES} matrix_replay.cpp)
# ZLIB: test_rows_compressed.cpp deflates its own payloads; matrix_replay.cpp inflates fixtures.
target_link_libraries(mybinlog_tests PRIVATE mybinlog::mybinlog ZLIB::ZLIB GTest::gtest_main)
# Tests may include the header-only internal helpers under src/.
target_include_directories(mybinlog_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(mybinlog_tests PRIVATE
  MYBINLOG_GENERATED_INCLUDE_DIR="${MYBINLOG_GENERATED_INCLUDE_DIR}")
target_compile_options(mybinlog_tests PRIVATE -fno-exceptions -fno-rtti)
gtest_discover_tests(mybinlog_tests DISCOVERY_TIMEOUT 30 PROPERTIES TIMEOUT 10)

# mybinlog-dump on each fixture; MySQL fixtures hold 6 rows events, MariaDB ones 4.
if(TARGET mybinlog-dump)
  foreach(entry mysql84_none_tagged:6 mysql84_crc32_tagged:6 mysql97_none_tagged:6
                mysql97_crc32_tagged:6 mariadb118_none:4 mariadb118_crc32:4)
    string(REPLACE ":" ";" parts ${entry})
    list(GET parts 0 fixture)
    list(GET parts 1 rows_events)
    add_test(NAME CodecExample.Dump_${fixture}
      COMMAND ${CMAKE_COMMAND} -DDUMP=$<TARGET_FILE:mybinlog-dump>
        -DFIXTURE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/${fixture}.bin
        -DEXPECTED_ROWS_EVENTS=${rows_events}
        -P ${PROJECT_SOURCE_DIR}/examples/mybinlog-dump/smoke_test.cmake)
    set_tests_properties(CodecExample.Dump_${fixture} PROPERTIES TIMEOUT 30)
  endforeach()
endif()

# Installs the build into a scratch prefix and builds a consumer of each built
# library kind against it; when both kinds are built they share that prefix.
if(MYBINLOG_INSTALL)
  set(MYBINLOG_INSTALL_TEST_ARGS
    -DMYBINLOG_BUILD_DIR=${PROJECT_BINARY_DIR}
    -DMYBINLOG_CONSUMER_DIR=${CMAKE_CURRENT_SOURCE_DIR}/install/consumer
    -DMYBINLOG_FIXTURE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/mysql84_crc32_tagged.bin
    -DMYBINLOG_EXPECTED_VERSION=${PROJECT_VERSION}
    -DMYBINLOG_CXX_COMPILER=${CMAKE_CXX_COMPILER}
    -DMYBINLOG_GENERATOR=${CMAKE_GENERATOR}
  )
  set(MYBINLOG_INSTALL_TESTS "")
  foreach(Kind Static Shared)
    string(TOLOWER ${Kind} kind)
    if(NOT TARGET mybinlog_${kind})
      continue()
    endif()
    foreach(Mode FindPackage PkgConfig)
      set(test CodecInstall.${Mode}${Kind})
      add_test(NAME ${test}
        COMMAND ${CMAKE_COMMAND} ${MYBINLOG_INSTALL_TEST_ARGS} -DMYBINLOG_MODE=${Mode}
          -DMYBINLOG_KIND=${kind}
          -DMYBINLOG_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/install-${Mode}-${kind}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/install/run_install_test.cmake)
      list(APPEND MYBINLOG_INSTALL_TESTS ${test})
    endforeach()
  endforeach()
  set_tests_properties(${MYBINLOG_INSTALL_TESTS} PROPERTIES
    TIMEOUT 300
    SKIP_RETURN_CODE 77
    RESOURCE_LOCK mybinlog_install_manifest
  )
endif()
