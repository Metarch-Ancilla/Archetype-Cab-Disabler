# This disposable path is fixed by CMake under the build directory, never user data.
file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")
execute_process(COMMAND "${PROGRAM}" "${TEST_ROOT}" RESULT_VARIABLE result)
file(REMOVE_RECURSE "${TEST_ROOT}")
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Native regression failed: ${result}")
endif()
