cmake_minimum_required(VERSION 3.31)
project(minidb VERSION 0.1.0 LANGUAGES C)
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
include(CTest)
add_library(yyjson STATIC vendor/yyjson/yyjson.c)
target_include_directories(yyjson PUBLIC vendor/yyjson)
add_library(engine STATIC src/engine.c src/result.c src/sql.c src/catalog.c src/storage.c src/bptree.c)
target_include_directories(engine PUBLIC include PRIVATE src)
target_link_libraries(engine PRIVATE yyjson)
target_compile_options(engine PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(engine PRIVATE MINIDB_ENGINE=1)
add_executable(minidb src/cli.c src/format.c)
target_link_libraries(minidb PRIVATE engine)
target_link_options(minidb PRIVATE -municode)
add_executable(bench_m11 tools/bench_m11.c)
target_link_libraries(bench_m11 PRIVATE engine yyjson)
target_compile_options(bench_m11 PRIVATE -Wall -Wextra -Wpedantic)
target_link_options(bench_m11 PRIVATE -municode)
add_executable(demo_timing tools/demo_timing.c src/format.c)
target_include_directories(demo_timing PRIVATE src)
target_link_libraries(demo_timing PRIVATE engine)
target_compile_options(demo_timing PRIVATE -Wall -Wextra -Wpedantic)
target_link_options(demo_timing PRIVATE -municode)
if(BUILD_TESTING)
  set(CMOCKA_SOURCE_DIR "${CMAKE_SOURCE_DIR}/.cache/tooling/cmocka/2.0.1/cmocka-2.0.1" CACHE PATH "Pinned cmocka source")
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(WITH_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(UNIT_TESTING OFF CACHE BOOL "" FORCE)
  if(NOT EXISTS "${CMOCKA_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Pinned cmocka source missing: ${CMOCKA_SOURCE_DIR}")
  endif()
  # cmocka 자체는 upstream C99로 빌드하고 DBMS 타깃의 C23과 구분한다.
  set(CMAKE_C_STANDARD 99)
  add_subdirectory("${CMOCKA_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/cmocka" EXCLUDE_FROM_ALL)
  set(CMAKE_C_STANDARD 23)
  add_executable(unit_milestone tests/unit_milestone.c)
  target_link_libraries(unit_milestone PRIVATE engine cmocka::cmocka)
  target_include_directories(unit_milestone PRIVATE "${CMOCKA_SOURCE_DIR}/include" src vendor/yyjson)
  # 실패 주입은 별도 라이브러리/실행 파일에만 컴파일한다.
  add_library(engine_fault STATIC src/engine.c src/result.c src/sql.c src/catalog.c src/storage.c src/bptree.c)
  target_include_directories(engine_fault PUBLIC include PRIVATE src)
  target_link_libraries(engine_fault PRIVATE yyjson)
  target_compile_definitions(engine_fault PRIVATE MINIDB_TEST_FAULTS=1 MINIDB_ENGINE=1)
  target_compile_options(engine_fault PRIVATE -Wall -Wextra -Wpedantic)
  add_executable(unit_join tests/unit_milestone.c)
  target_link_libraries(unit_join PRIVATE engine_fault cmocka::cmocka)
  target_include_directories(unit_join PRIVATE "${CMOCKA_SOURCE_DIR}/include" src vendor/yyjson)
  target_compile_definitions(unit_join PRIVATE MINIDB_TEST_FAULTS=1)
  add_executable(minidb_fault src/cli.c src/format.c)
  target_compile_definitions(minidb_fault PRIVATE MINIDB_TEST_FAULTS=1)
  target_link_libraries(minidb_fault PRIVATE engine_fault)
  target_link_options(minidb_fault PRIVATE -municode)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  foreach(case IN ITEMS GENERATE-01 DECODE-01 ORACLE-01 BENCH-01 STATISTICS-01 ARGUMENT-01 OUTPUT-01 REGRESSION-01)
    add_test(NAME M11-${case} COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/m11_benchmark.py"
      --exe "$<TARGET_FILE:minidb>" --bench "$<TARGET_FILE:bench_m11>"
      --case "M11-${case}" --work "${CMAKE_BINARY_DIR}/test-data/M11-${case}")
    set_tests_properties(M11-${case} PROPERTIES TIMEOUT 120)
  endforeach()
  foreach(capacity IN ITEMS production small)
    add_executable(unit_bptree_${capacity} tests/unit_bptree.c src/bptree.c)
    target_include_directories(unit_bptree_${capacity} PRIVATE src "${CMOCKA_SOURCE_DIR}/include")
    target_link_libraries(unit_bptree_${capacity} PRIVATE cmocka::cmocka)
    target_compile_definitions(unit_bptree_${capacity} PRIVATE MD_BT_TEST=1)
    target_compile_options(unit_bptree_${capacity} PRIVATE -Wall -Wextra -Wpedantic)
  endforeach()
  target_compile_definitions(unit_bptree_small PRIVATE MD_BT_CHILDREN=4)
  add_executable(unit_m10 tests/unit_m10.c src/bptree.c)
  target_include_directories(unit_m10 PRIVATE include src vendor/yyjson "${CMOCKA_SOURCE_DIR}/include")
  target_link_libraries(unit_m10 PRIVATE engine_fault cmocka::cmocka)
  target_compile_definitions(unit_m10 PRIVATE MD_BT_TEST=1 MINIDB_TEST_FAULTS=1)
  add_test(NAME M10-RESERVE-01 COMMAND unit_m10)
  foreach(case IN ITEMS STARTUP-01 AUTO-01 AUTO-02 AUTO-03 INSERT-01 LOOKUP-01 LOOKUP-02 LOOKUP-03
      UPDATE-01 DELETE-01 DELETE-02 STORAGE-01 RESTART-01 MIGRATE-01)
    add_test(NAME M10-${case} COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/m10_cli.py"
      --exe "$<TARGET_FILE:minidb>" --fault-exe "$<TARGET_FILE:minidb_fault>"
      --unit-exe "$<TARGET_FILE:unit_m10>" --case "M10-${case}" --work "${CMAKE_BINARY_DIR}/test-data/M10-${case}")
    set_tests_properties(M10-${case} PROPERTIES TIMEOUT 120)
  endforeach()
  add_executable(unit_bptree_runtime tests/unit_bptree.c)
  target_compile_definitions(unit_bptree_runtime PRIVATE MD_BT_RUNTIME=1)
  target_include_directories(unit_bptree_runtime PRIVATE src "${CMOCKA_SOURCE_DIR}/include")
  target_link_libraries(unit_bptree_runtime PRIVATE engine cmocka::cmocka)
  target_compile_options(unit_bptree_runtime PRIVATE -Wall -Wextra -Wpedantic)
  foreach(case IN ITEMS EMPTY-01 INSERT-01 ORDER-01 DUPLICATE-01 SPLIT-01 SPLIT-02
      RANGE-01 BOUNDARY-01 FAILURE-01 FAILURE-02 OWNERSHIP-01 INVARIANT-01)
    if(case STREQUAL "INSERT-01")
      add_test(NAME M9-${case} COMMAND unit_bptree_production "M9-${case}"
        "$<TARGET_FILE:unit_bptree_small>" "$<TARGET_FILE:unit_bptree_runtime>")
    else()
      add_test(NAME M9-${case} COMMAND unit_bptree_production "M9-${case}" "$<TARGET_FILE:unit_bptree_small>")
    endif()
    set_tests_properties(M9-${case} PROPERTIES TIMEOUT 120)
  endforeach()
  add_test(NAME M1-unit-event-lifetime COMMAND unit_milestone)
  foreach(case IN ITEMS roundtrip literals stop-on-error invalid-id strict-schema corruption pipe
      lease pending append-rollback rollback-failure cleanup-committed output-failure read-ahead argv-wire)
    add_test(NAME M1-${case} COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/milestone_cli.py"
      --exe "$<TARGET_FILE:minidb>" --fault-exe "$<TARGET_FILE:minidb_fault>"
      --case "${case}" --work "${CMAKE_BINARY_DIR}/test-data/${case}")
    set_tests_properties(M1-${case} PROPERTIES TIMEOUT 90)
  endforeach()
  foreach(case IN ITEMS INSERT-01 INSERT-02 INSERT-03 INSERT-04 INSERT-05
      WHERE-01 WHERE-02 WHERE-03 NAME-01 NAME-02 VALUES-01 VALUES-02 VALUES-03 REGRESSION-01)
    add_test(NAME M2-${case} COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/m2_cli.py"
      --exe "$<TARGET_FILE:minidb>" --fault-exe "$<TARGET_FILE:minidb_fault>"
      --case "M2-${case}" --work "${CMAKE_BINARY_DIR}/test-data/M2-${case}")
    set_tests_properties(M2-${case} PROPERTIES TIMEOUT 90)
  endforeach()
  foreach(case IN ITEMS UPDATE-01 UPDATE-02 UPDATE-03 UPDATE-04 UPDATE-05 UPDATE-06 UPDATE-07 UPDATE-08
      DELETE-01 DELETE-02 DELETE-03 STORE-01 STORE-02 STORE-03 STORE-04 STORE-05 STORE-06 STORE-07 CLI-01 REGRESSION-01)
    add_test(NAME M3-${case} COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/m3_cli.py"
      --exe "$<TARGET_FILE:minidb>" --fault-exe "$<TARGET_FILE:minidb_fault>"
      --case "M3-${case}" --work "${CMAKE_BINARY_DIR}/test-data/M3-${case}")
    set_tests_properties(M3-${case} PROPERTIES TIMEOUT 120)
  endforeach()
  foreach(case IN ITEMS ORDER-01 ORDER-02 ORDER-03 ORDER-04 PAGE-01 PAGE-02 PAGE-03
      ERROR-01 ERROR-02 VALUES-01 VALUES-02 CLI-01 REGRESSION-01)
    add_test(NAME M4-${case} COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/m4_cli.py"
      --exe "$<TARGET_FILE:minidb>" --fault-exe "$<TARGET_FILE:minidb_fault>"
      --case "M4-${case}" --work "${CMAKE_BINARY_DIR}/test-data/M4-${case}")
    set_tests_properties(M4-${case} PROPERTIES TIMEOUT 120)
  endforeach()
  foreach(case IN ITEMS JOIN-01 JOIN-02 JOIN-03 JOIN-04 NAME-01 NAME-02 TAIL-01 EMPTY-01
      ERROR-01 RESOURCE-01 OWNERSHIP-01 CLI-01 REGRESSION-01)
    add_test(NAME M5-${case} COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/m5_cli.py"
      --exe "$<TARGET_FILE:minidb>" --fault-exe "$<TARGET_FILE:minidb_fault>"
      --unit-exe "$<TARGET_FILE:unit_join>"
      --case "M5-${case}" --work "${CMAKE_BINARY_DIR}/test-data/M5-${case}")
    set_tests_properties(M5-${case} PROPERTIES TIMEOUT 120)
  endforeach()
  foreach(case IN ITEMS COUNT-01 GROUP-01 GROUP-02 HAVING-01 HAVING-02 JOIN-01 TAIL-01 EMPTY-01
      ERROR-01 OVERFLOW-01 RESOURCE-01 OWNERSHIP-01 CLI-01 REGRESSION-01)
    add_test(NAME M6-${case} COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/m6_cli.py"
      --exe "$<TARGET_FILE:minidb>" --fault-exe "$<TARGET_FILE:minidb_fault>"
      --unit-exe "$<TARGET_FILE:unit_join>"
      --case "M6-${case}" --work "${CMAKE_BINARY_DIR}/test-data/M6-${case}")
    set_tests_properties(M6-${case} PROPERTIES TIMEOUT 120)
  endforeach()
  foreach(case IN ITEMS UNION-01 ALL-01 MIXED-01 TYPE-01 EMPTY-01 SELECT-01 TAIL-01 ERROR-01
      OVERFLOW-01 RESOURCE-01 OWNERSHIP-01 CLI-01 REGRESSION-01)
    add_test(NAME M7-${case} COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/m7_cli.py"
      --exe "$<TARGET_FILE:minidb>" --fault-exe "$<TARGET_FILE:minidb_fault>"
      --unit-exe "$<TARGET_FILE:unit_join>"
      --case "M7-${case}" --work "${CMAKE_BINARY_DIR}/test-data/M7-${case}")
    set_tests_properties(M7-${case} PROPERTIES TIMEOUT 120)
  endforeach()
  foreach(case IN ITEMS PLAN-01 PLAN-02 PLAN-03 SOURCE-01 MAP-01 MAP-02 MAP-03 COPY-01
      TYPE-01 TYPE-02 TYPE-03 PATH-01 LEASE-01 FAILURE-01 FAILURE-02 RESOURCE-01 CLI-01
      OWNERSHIP-01 INIT-01 REGRESSION-01)
    add_test(NAME M8-${case} COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/m8_cli.py"
      --exe "$<TARGET_FILE:minidb>" --fault-exe "$<TARGET_FILE:minidb_fault>"
      --unit-exe "$<TARGET_FILE:unit_join>"
      --case "M8-${case}" --work "${CMAKE_BINARY_DIR}/test-data/M8-${case}")
    set_tests_properties(M8-${case} PROPERTIES TIMEOUT 120)
  endforeach()
endif()

if(BUILD_TESTING)
  add_executable(unit_m12 tests/unit_m12.c)
  target_include_directories(unit_m12 PRIVATE include src vendor/yyjson)
  target_link_libraries(unit_m12 PRIVATE engine)
  add_test(NAME M12-HEALTH-01 COMMAND unit_m12)
  add_executable(unit_m13 tests/unit_m13.c)
  file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/test-data")
  target_include_directories(unit_m13 PRIVATE include src vendor/yyjson)
  target_link_libraries(unit_m13 PRIVATE engine_fault minidb_client)
  target_compile_definitions(unit_m13 PRIVATE MINIDB_TEST_FAULTS=1)
  foreach(case IN ITEMS READ-01 RW-01 WW-01 ADMIT-01 WAIT-01 CLASSIFY-01 HEALTH-01 TIME-01 LOOP-01 PREP-01 COMMIT-01)
    add_test(NAME M13-${case} COMMAND unit_m13 M13-${case} "${CMAKE_BINARY_DIR}/test-data/M13-${case}")
    set_tests_properties(M13-${case} PROPERTIES TIMEOUT 30)
  endforeach()
endif()

add_library(minidb_client STATIC src/client.c src/protocol.c src/result.c src/format.c)
target_include_directories(minidb_client PUBLIC include PRIVATE src vendor/yyjson)
target_link_libraries(minidb_client PRIVATE yyjson ws2_32)
target_compile_options(minidb_client PRIVATE -Wall -Wextra -Wpedantic)

if(BUILD_TESTING)
  target_link_libraries(unit_m12 PRIVATE minidb_client)
  # 구 SDK의 원래 strict stage whitelist를 별도 실행 파일로 재현한다.
  add_library(minidb_client_legacy STATIC src/client.c src/protocol.c src/result.c src/format.c)
  target_include_directories(minidb_client_legacy PUBLIC include PRIVATE src vendor/yyjson)
  target_compile_definitions(minidb_client_legacy PRIVATE MINIDB_LEGACY_STAGE=1)
  target_link_libraries(minidb_client_legacy PRIVATE yyjson ws2_32)
  add_executable(unit_m13_legacy tests/unit_m13.c)
  target_include_directories(unit_m13_legacy PRIVATE include src vendor/yyjson)
  target_compile_definitions(unit_m13_legacy PRIVATE MINIDB_TEST_FAULTS=1 MINIDB_LEGACY_STAGE=1)
  target_link_libraries(unit_m13_legacy PRIVATE engine_fault minidb_client_legacy)
  add_test(NAME M12-SDK-01 COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/m12_remote.py"
    --unit "$<TARGET_FILE:unit_m12>" --case M12-SDK-01)
endif()

add_executable(minidbd src/server.c)
target_include_directories(minidbd PRIVATE src vendor/yyjson)
target_link_libraries(minidbd PRIVATE engine minidb_client ws2_32)
target_link_options(minidbd PRIVATE -municode)
target_compile_options(minidbd PRIVATE -Wall -Wextra -Wpedantic)
if(BUILD_TESTING)
  add_executable(minidbd_fault src/server.c)
  target_compile_definitions(minidbd_fault PRIVATE MINIDB_TEST_FAULTS=1)
  target_include_directories(minidbd_fault PRIVATE src vendor/yyjson)
  target_link_libraries(minidbd_fault PRIVATE engine_fault minidb_client ws2_32)
  target_link_options(minidbd_fault PRIVATE -municode)
endif()
add_executable(minidb-remote src/remote_cli.c)
target_include_directories(minidb-remote PRIVATE src vendor/yyjson)
target_link_libraries(minidb-remote PRIVATE minidb_client)
target_link_options(minidb-remote PRIVATE -municode)
target_compile_options(minidb-remote PRIVATE -Wall -Wextra -Wpedantic)

add_executable(bench_m14 tools/bench_m14.c)
target_link_libraries(bench_m14 PRIVATE minidb_client psapi)
target_link_options(bench_m14 PRIVATE -municode)
target_compile_options(bench_m14 PRIVATE -Wall -Wextra -Wpedantic)
if(BUILD_TESTING)
  foreach(case IN ITEMS SDK-01 QUEUE-01 STATISTICS-01 MIXED-01 FAILURE-01 MEMORY-01 OUTPUT-01 REGRESSION-01)
    add_test(NAME M14-${case} COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/m14_benchmark.py"
      --exe "$<TARGET_FILE:minidb>" --server "$<TARGET_FILE:minidbd>" --fault-server "$<TARGET_FILE:minidbd_fault>"
      --remote "$<TARGET_FILE:minidb-remote>" --bench "$<TARGET_FILE:bench_m14>"
      --case "M14-${case}" --work "${CMAKE_BINARY_DIR}/test-data/M14-${case}")
    set_tests_properties(M14-${case} PROPERTIES TIMEOUT 120)
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(case IN ITEMS LIFE-01 WIRE-01 REGRESSION-01)
    add_test(NAME M13-${case} COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/m13_remote.py"
      --unit "$<TARGET_FILE:unit_m13>" --legacy "$<TARGET_FILE:unit_m13_legacy>"
      --exe "$<TARGET_FILE:minidb>" --server "$<TARGET_FILE:minidbd>"
      --fault-server "$<TARGET_FILE:minidbd_fault>" --remote "$<TARGET_FILE:minidb-remote>"
      --case "M13-${case}" --work "${CMAKE_BINARY_DIR}/test-data/M13-${case}")
    set_tests_properties(M13-${case} PROPERTIES TIMEOUT 60)
  endforeach()
  foreach(case IN ITEMS WIRE-01 WIRE-02 CHUNK-01 CHUNK-02 SQL-01 POOL-01 QUEUE-01 OWN-01 DISCONNECT-01
      STATE-01 LIMIT-01 CONFIG-01 TIME-01 SHUTDOWN-01 CLI-01 REGRESSION-01)
    add_test(NAME M12-${case} COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/m12_remote.py"
      --unit "$<TARGET_FILE:unit_m12>" --exe "$<TARGET_FILE:minidb>" --server "$<TARGET_FILE:minidbd>"
      --fault-server "$<TARGET_FILE:minidbd_fault>" --remote "$<TARGET_FILE:minidb-remote>"
      --case "M12-${case}" --work "${CMAKE_BINARY_DIR}/test-data/M12-${case}")
    set_tests_properties(M12-${case} PROPERTIES TIMEOUT 120)
  endforeach()
endif()
