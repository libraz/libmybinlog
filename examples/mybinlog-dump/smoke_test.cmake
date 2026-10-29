# Runs mybinlog-dump on one fixture: exit status 0, output, and the expected
# number of rows events (lines carrying decoded "rows").
# Inputs: -DDUMP=<executable> -DFIXTURE=<binlog file> -DEXPECTED_ROWS_EVENTS=<n>
execute_process(
  COMMAND ${DUMP} ${FIXTURE}
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE errors
)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "mybinlog-dump exited with ${result}: ${errors}")
endif()
if(output STREQUAL "")
  message(FATAL_ERROR "mybinlog-dump printed nothing")
endif()
string(REGEX MATCHALL "\"rows\":" rows_events "${output}")
list(LENGTH rows_events count)
if(NOT count EQUAL EXPECTED_ROWS_EVENTS)
  message(FATAL_ERROR "expected ${EXPECTED_ROWS_EVENTS} rows events, got ${count}")
endif()
