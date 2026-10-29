file(GLOB MYBINLOG_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp)

add_executable(mybinlog_tests ${MYBINLOG_TEST_SOURCES})
target_link_libraries(mybinlog_tests PRIVATE mybinlog::mybinlog GTest::gtest_main)
target_compile_options(mybinlog_tests PRIVATE -fno-exceptions -fno-rtti)
gtest_discover_tests(mybinlog_tests DISCOVERY_TIMEOUT 30 PROPERTIES TIMEOUT 10)
