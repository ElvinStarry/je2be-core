if(NOT GIT_EXECUTABLE)
  find_package(Git REQUIRED)
endif()

foreach(PATCH_FILE IN LISTS PATCH_FILES)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply --check "${PATCH_FILE}"
    WORKING_DIRECTORY "${SOURCE_DIR}"
    RESULT_VARIABLE APPLY_CHECK
    OUTPUT_QUIET
    ERROR_QUIET)
  if(APPLY_CHECK EQUAL 0)
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" apply "${PATCH_FILE}"
      WORKING_DIRECTORY "${SOURCE_DIR}"
      RESULT_VARIABLE APPLY_RESULT)
    if(NOT APPLY_RESULT EQUAL 0)
      message(FATAL_ERROR "Failed to apply ${PATCH_FILE}")
    endif()
    continue()
  endif()

  execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply --reverse --check "${PATCH_FILE}"
    WORKING_DIRECTORY "${SOURCE_DIR}"
    RESULT_VARIABLE REVERSE_CHECK
    OUTPUT_QUIET
    ERROR_QUIET)
  if(NOT REVERSE_CHECK EQUAL 0)
    message(FATAL_ERROR "Patch is neither applicable nor already applied: ${PATCH_FILE}")
  endif()
endforeach()
