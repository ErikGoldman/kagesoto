cmake_minimum_required(VERSION 3.16)

foreach(required_var IN ITEMS
    ASHIATO_SOURCE_DIR
    ASHIATO_BINARY_DIR
    ASHIATO_GCOV_EXECUTABLE
    ASHIATO_COVERAGE_OUTPUT_DIR
)
    if(NOT DEFINED ${required_var} OR "${${required_var}}" STREQUAL "")
        message(FATAL_ERROR "${required_var} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${ASHIATO_COVERAGE_OUTPUT_DIR}")
file(MAKE_DIRECTORY "${ASHIATO_COVERAGE_OUTPUT_DIR}")

file(GLOB_RECURSE ashiato_gcda_files
    "${ASHIATO_BINARY_DIR}/CMakeFiles/ashiato.dir/*.gcda"
    "${ASHIATO_BINARY_DIR}/CMakeFiles/ashiato_tests.dir/*.gcda"
)

if(NOT ashiato_gcda_files)
    message(FATAL_ERROR "No .gcda files found. Build with ASHIATO_ENABLE_COVERAGE=ON and run tests before generating coverage.")
endif()

set(ashiato_line_total 0)
set(ashiato_line_covered 0)
set(ashiato_branch_total 0)
set(ashiato_branch_covered 0)
set(ashiato_function_total 0)
set(ashiato_function_covered 0)
set(ashiato_seen_sources "")
set(ashiato_seen_lines "")
set(ashiato_seen_branches "")
set(ashiato_seen_functions "")

function(ashiato_project_source source_path out_var)
    if(source_path MATCHES "^${ASHIATO_SOURCE_DIR}/(include/ashiato/.*|src/.*\\.(cpp|hpp|h))$"
        AND NOT source_path MATCHES "\\.test\\.cpp$|src/ashiato_test_support\\.hpp$")
        set(${out_var} TRUE PARENT_SCOPE)
    else()
        set(${out_var} FALSE PARENT_SCOPE)
    endif()
endfunction()

function(ashiato_record_covered_key list_var prefix key is_covered)
    string(MAKE_C_IDENTIFIER "${key}" key_id)
    set(value_var "${prefix}_${key_id}")

    if(NOT key IN_LIST ${list_var})
        list(APPEND ${list_var} "${key}")
        set(${value_var} FALSE PARENT_SCOPE)
    endif()

    if(is_covered)
        set(${value_var} TRUE PARENT_SCOPE)
    endif()

    set(${list_var} "${${list_var}}" PARENT_SCOPE)
endfunction()

foreach(gcda_file IN LISTS ashiato_gcda_files)
    get_filename_component(gcda_dir "${gcda_file}" DIRECTORY)
    execute_process(
        COMMAND "${ASHIATO_GCOV_EXECUTABLE}" -b -c -p -o "${gcda_dir}" "${gcda_file}"
        WORKING_DIRECTORY "${ASHIATO_COVERAGE_OUTPUT_DIR}"
        RESULT_VARIABLE gcov_result
        OUTPUT_VARIABLE gcov_output
        ERROR_VARIABLE gcov_error
    )

    if(NOT gcov_result EQUAL 0)
        message(FATAL_ERROR "gcov failed for ${gcda_file}\n${gcov_output}\n${gcov_error}")
    endif()
endforeach()

file(GLOB ashiato_gcov_reports "${ASHIATO_COVERAGE_OUTPUT_DIR}/*.gcov")

foreach(gcov_report IN LISTS ashiato_gcov_reports)
    file(READ "${gcov_report}" gcov_report_content)
    string(REPLACE "\r\n" "\n" gcov_report_content "${gcov_report_content}")
    string(REPLACE "\n" ";" gcov_lines "${gcov_report_content}")

    set(current_source "")
    set(current_line 0)
    set(include_current_source FALSE)

    foreach(gcov_line IN LISTS gcov_lines)
        if(gcov_line MATCHES "^ *-: *0:Source:(.+)$")
            set(current_source "${CMAKE_MATCH_1}")
            ashiato_project_source("${current_source}" include_current_source)
            if(include_current_source)
                list(APPEND ashiato_seen_sources "${current_source}")
            endif()
        elseif(include_current_source AND gcov_line MATCHES "^function ([^ ]+) called ([0-9]+)")
            set(function_key "${current_source}:${CMAKE_MATCH_1}")
            set(is_covered FALSE)
            if(CMAKE_MATCH_2 GREATER 0)
                set(is_covered TRUE)
            endif()
            ashiato_record_covered_key(ashiato_seen_functions "ashiato_function_covered_key" "${function_key}" "${is_covered}")
        elseif(include_current_source AND gcov_line MATCHES "^ *([^:]+): *([0-9]+):(.*)$")
            set(line_count "${CMAKE_MATCH_1}")
            set(current_line "${CMAKE_MATCH_2}")
            if(NOT line_count STREQUAL "-")
                set(line_key "${current_source}:${current_line}")
                set(is_covered FALSE)
                if(line_count MATCHES "^[1-9]")
                    set(is_covered TRUE)
                endif()
                ashiato_record_covered_key(ashiato_seen_lines "ashiato_line_covered_key" "${line_key}" "${is_covered}")
            endif()
        elseif(include_current_source AND gcov_line MATCHES "^branch +([0-9]+) +taken +([0-9]+)")
            set(branch_key "${current_source}:${current_line}:${CMAKE_MATCH_1}")
            set(is_covered FALSE)
            if(CMAKE_MATCH_2 GREATER 0)
                set(is_covered TRUE)
            endif()
            ashiato_record_covered_key(ashiato_seen_branches "ashiato_branch_covered_key" "${branch_key}" "${is_covered}")
        elseif(include_current_source AND gcov_line MATCHES "^branch +([0-9]+) +never executed")
            set(branch_key "${current_source}:${current_line}:${CMAKE_MATCH_1}")
            ashiato_record_covered_key(ashiato_seen_branches "ashiato_branch_covered_key" "${branch_key}" FALSE)
        endif()
    endforeach()
endforeach()

list(REMOVE_DUPLICATES ashiato_seen_sources)
list(LENGTH ashiato_seen_sources ashiato_source_count)

foreach(line_key IN LISTS ashiato_seen_lines)
    string(MAKE_C_IDENTIFIER "${line_key}" key_id)
    math(EXPR ashiato_line_total "${ashiato_line_total} + 1")
    if(ashiato_line_covered_key_${key_id})
        math(EXPR ashiato_line_covered "${ashiato_line_covered} + 1")
    endif()
endforeach()

foreach(branch_key IN LISTS ashiato_seen_branches)
    string(MAKE_C_IDENTIFIER "${branch_key}" key_id)
    math(EXPR ashiato_branch_total "${ashiato_branch_total} + 1")
    if(ashiato_branch_covered_key_${key_id})
        math(EXPR ashiato_branch_covered "${ashiato_branch_covered} + 1")
    endif()
endforeach()

foreach(function_key IN LISTS ashiato_seen_functions)
    string(MAKE_C_IDENTIFIER "${function_key}" key_id)
    math(EXPR ashiato_function_total "${ashiato_function_total} + 1")
    if(ashiato_function_covered_key_${key_id})
        math(EXPR ashiato_function_covered "${ashiato_function_covered} + 1")
    endif()
endforeach()

function(ashiato_print_coverage label covered total)
    if(${total} EQUAL 0)
        message(STATUS "${label}: n/a")
    else()
        math(EXPR percent_x100 "(${covered} * 10000 + (${total} / 2)) / ${total}")
        math(EXPR percent_whole "${percent_x100} / 100")
        math(EXPR percent_fraction "${percent_x100} % 100")
        if(percent_fraction LESS 10)
            set(percent_fraction "0${percent_fraction}")
        endif()
        message(STATUS "${label}: ${percent_whole}.${percent_fraction}% (${covered}/${total})")
    endif()
endfunction()

message(STATUS "Coverage report files: ${ASHIATO_COVERAGE_OUTPUT_DIR}")
message(STATUS "Project source files reported: ${ashiato_source_count}")
ashiato_print_coverage("Line coverage" "${ashiato_line_covered}" "${ashiato_line_total}")
ashiato_print_coverage("Branch coverage" "${ashiato_branch_covered}" "${ashiato_branch_total}")
ashiato_print_coverage("Function coverage" "${ashiato_function_covered}" "${ashiato_function_total}")
