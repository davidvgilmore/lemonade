cmake_minimum_required(VERSION 3.15)
file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}/source/web-app" "${TEST_ROOT}/source/docs/api"
    "${TEST_ROOT}/source/schemas")
file(WRITE "${TEST_ROOT}/source/architecture_defaults.json" "{\"test\":true}")
file(WRITE "${TEST_ROOT}/source/benchmark_forks.json" "{}")
file(WRITE "${TEST_ROOT}/source/docs/api/README.md" "API reference")
file(WRITE "${TEST_ROOT}/source/schemas/request.schema.json" "{}")
file(WRITE "${TEST_ROOT}/source/web-app/index.html" "<html>test browser app</html>")
file(WRITE "${TEST_ROOT}/source/web-app/main.js" "console.log('test')")

foreach(mode IN ITEMS ON OFF)
    execute_process(COMMAND "${CMAKE_COMMAND}"
        "-DRESOURCE_SOURCE=${TEST_ROOT}/source"
        "-DRESOURCE_DESTINATION=${TEST_ROOT}/${mode}"
        "-DINCLUDE_WEB_APP=${mode}" -P "${STAGING_SCRIPT}"
        RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Resource staging failed in ${mode} mode")
    endif()
    foreach(resource IN ITEMS architecture_defaults.json benchmark_forks.json
            docs/api/README.md schemas/request.schema.json)
        file(SHA256 "${TEST_ROOT}/source/${resource}" expected)
        file(SHA256 "${TEST_ROOT}/${mode}/${resource}" actual)
        if(NOT actual STREQUAL expected)
            message(FATAL_ERROR "Runtime resource changed: ${resource}")
        endif()
    endforeach()
endforeach()
if(EXISTS "${TEST_ROOT}/OFF/web-app")
    message(FATAL_ERROR "Disabled web app was packaged from stale build output")
endif()
foreach(resource IN ITEMS index.html main.js)
    file(SHA256 "${TEST_ROOT}/source/web-app/${resource}" expected)
    file(SHA256 "${TEST_ROOT}/ON/web-app/${resource}" actual)
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR "Browser asset changed: ${resource}")
    endif()
endforeach()
file(REMOVE "${TEST_ROOT}/source/web-app/index.html")
execute_process(COMMAND "${CMAKE_COMMAND}"
    "-DRESOURCE_SOURCE=${TEST_ROOT}/source"
    "-DRESOURCE_DESTINATION=${TEST_ROOT}/missing"
    -DINCLUDE_WEB_APP=ON -P "${STAGING_SCRIPT}"
    RESULT_VARIABLE result OUTPUT_QUIET ERROR_QUIET)
if(result EQUAL 0 OR EXISTS "${TEST_ROOT}/missing")
    message(FATAL_ERROR "Incomplete enabled browser app must refuse before staging")
endif()
file(REMOVE_RECURSE "${TEST_ROOT}")
