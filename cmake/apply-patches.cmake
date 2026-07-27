if(GIT_EXECUTABLE AND EXISTS "${GIT_EXECUTABLE}")
  set(_JE2BE_GIT_EXECUTABLE "${GIT_EXECUTABLE}")
else()
  # This file is executed with cmake -P from FetchContent's sub-build. Script
  # mode has no project() call, so FindGit cannot be used here.
  find_program(_JE2BE_GIT_EXECUTABLE NAMES git)
endif()

if(NOT _JE2BE_GIT_EXECUTABLE)
  message(FATAL_ERROR "Could not find the git executable needed to apply dependency patches")
endif()

foreach(PATCH_FILE IN LISTS PATCH_FILES)
  execute_process(
    COMMAND "${_JE2BE_GIT_EXECUTABLE}" apply --check "${PATCH_FILE}"
    WORKING_DIRECTORY "${SOURCE_DIR}"
    RESULT_VARIABLE APPLY_CHECK
    OUTPUT_QUIET
    ERROR_QUIET)
  if(APPLY_CHECK EQUAL 0)
    execute_process(
      COMMAND "${_JE2BE_GIT_EXECUTABLE}" apply "${PATCH_FILE}"
      WORKING_DIRECTORY "${SOURCE_DIR}"
      RESULT_VARIABLE APPLY_RESULT)
    if(NOT APPLY_RESULT EQUAL 0)
      message(FATAL_ERROR "Failed to apply ${PATCH_FILE}")
    endif()
    continue()
  endif()

  execute_process(
    COMMAND "${_JE2BE_GIT_EXECUTABLE}" apply --reverse --check "${PATCH_FILE}"
    WORKING_DIRECTORY "${SOURCE_DIR}"
    RESULT_VARIABLE REVERSE_CHECK
    OUTPUT_QUIET
    ERROR_QUIET)
  if(NOT REVERSE_CHECK EQUAL 0)
    message(FATAL_ERROR "Patch is neither applicable nor already applied: ${PATCH_FILE}")
  endif()
endforeach()
