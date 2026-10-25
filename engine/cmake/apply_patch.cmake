# cmake -DPATCH=FILE -P apply_patch.cmake, in a git checkout: applies the patch unless it
# already is (FetchContent's patch step runs again on a reconfigure).
execute_process(COMMAND git apply --reverse --check ${PATCH} RESULT_VARIABLE applied OUTPUT_QUIET ERROR_QUIET)
if(NOT applied EQUAL 0)
    execute_process(COMMAND git apply --whitespace=nowarn ${PATCH} RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "cannot apply ${PATCH}")
    endif()
endif()
