file(GLOB MYBINLOG_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp)

add_executable(mybinlog_tests ${MYBINLOG_TEST_SOURCES})
# ZLIB: test_rows_compressed.cpp deflates its own payloads.
target_link_libraries(mybinlog_tests PRIVATE mybinlog::mybinlog ZLIB::ZLIB GTest::gtest_main)
# Tests may include the header-only internal helpers under src/.
target_include_directories(mybinlog_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(mybinlog_tests PRIVATE
  MYBINLOG_GENERATED_INCLUDE_DIR="${MYBINLOG_GENERATED_INCLUDE_DIR}")
target_compile_options(mybinlog_tests PRIVATE -fno-exceptions -fno-rtti)
gtest_discover_tests(mybinlog_tests DISCOVERY_TIMEOUT 30 PROPERTIES TIMEOUT 10)

# Installs the built library into a scratch prefix and builds a consumer against it.
if(MYBINLOG_INSTALL)
  get_target_property(MYBINLOG_TEST_LIBRARY_TYPE mybinlog TYPE)
  set(MYBINLOG_INSTALL_TEST_ARGS
    -DMYBINLOG_BUILD_DIR=${PROJECT_BINARY_DIR}
    -DMYBINLOG_CONSUMER_DIR=${CMAKE_CURRENT_SOURCE_DIR}/install/consumer
    -DMYBINLOG_FIXTURE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/mysql84_crc32_tagged.bin
    -DMYBINLOG_EXPECTED_VERSION=${PROJECT_VERSION}
    -DMYBINLOG_LIBRARY_TYPE=${MYBINLOG_TEST_LIBRARY_TYPE}
    -DMYBINLOG_CXX_COMPILER=${CMAKE_CXX_COMPILER}
    -DMYBINLOG_GENERATOR=${CMAKE_GENERATOR}
  )
  add_test(NAME CodecInstall.FindPackage
    COMMAND ${CMAKE_COMMAND} ${MYBINLOG_INSTALL_TEST_ARGS} -DMYBINLOG_MODE=find_package
      -DMYBINLOG_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/install-find-package
      -P ${CMAKE_CURRENT_SOURCE_DIR}/install/run_install_test.cmake)
  add_test(NAME CodecInstall.PkgConfig
    COMMAND ${CMAKE_COMMAND} ${MYBINLOG_INSTALL_TEST_ARGS} -DMYBINLOG_MODE=pkg_config
      -DMYBINLOG_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/install-pkg-config
      -P ${CMAKE_CURRENT_SOURCE_DIR}/install/run_install_test.cmake)
  set_tests_properties(CodecInstall.FindPackage CodecInstall.PkgConfig PROPERTIES
    TIMEOUT 300
    SKIP_RETURN_CODE 77
    RESOURCE_LOCK mybinlog_install_manifest
  )
endif()
