cmake_minimum_required(VERSION 3.16)
project(the-firm VERSION 0 LANGUAGES C)

enable_testing()

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wpedantic -Werror -Wshadow)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O3)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        add_compile_options(-march=native -mtune=native)
    endif()
endif()

option(FIRM_SANITIZE "Build with ASan+UBSan (not for production benches)" OFF)
if(FIRM_SANITIZE)
    add_compile_options(-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined)
    add_link_options(-fsanitize=address,undefined)
endif()

include_directories(${CMAKE_SOURCE_DIR}/include)

set(NET_CORE_SRC
    src/net/ring.c
    src/net/stats.c
    src/net/sink.c
    src/net/race.c
    src/net/capture.c
    src/net/capio.c)

add_library(net_core STATIC ${NET_CORE_SRC})
target_link_libraries(net_core PUBLIC pthread)

add_library(shred STATIC src/shred/race.c src/shred/ingest.c src/shred/prefix.c)
target_link_libraries(shred PUBLIC net_core)

add_executable(test_ring tests/net/test_ring.c)
target_link_libraries(test_ring PRIVATE net_core)
add_test(NAME test_ring COMMAND test_ring)

add_executable(test_capture tests/net/test_capture.c)
target_link_libraries(test_capture PRIVATE net_core)
add_test(NAME test_capture COMMAND test_capture)

add_executable(test_race tests/net/test_race.c)
target_link_libraries(test_race PRIVATE net_core)
add_test(NAME test_race COMMAND test_race)

add_executable(test_shred_parse tests/shred/test_parse.c)
add_test(NAME test_shred_parse COMMAND test_shred_parse)

add_executable(test_shred_race tests/shred/test_race.c)
target_link_libraries(test_shred_race PRIVATE shred)
add_test(NAME test_shred_race COMMAND test_shred_race)

add_executable(rx_bench bench/rx/main.c)
target_link_libraries(rx_bench PRIVATE net_core)

add_executable(shred_bench bench/shred/main.c)
target_link_libraries(shred_bench PRIVATE shred)

add_library(pump STATIC src/protocols/pump/pump.c)
set_property(TARGET pump PROPERTY C_EXTENSIONS ON)
target_compile_options(pump PRIVATE -Wno-pedantic)

add_library(state STATIC
    src/ingress/tx.c
    src/ingress/decode.c
    src/ingress/resolve.c
    src/shred/complete.c
    src/shred/sha256.c
    src/shred/rs.c
    src/shred/merkle.c
    src/shred/fec.c
    src/shred/assemble.c
    src/wire/pipeline.c
    src/deps/class.c
    src/state/compact.c
    src/state/cert.c
    src/transitions/overlay.c
    src/transitions/token.c
    src/transitions/apply.c
    src/publish/publish.c
    src/oracle/oracle.c
    src/oracle/case.c
    src/oracle/soak.c
    src/shred/txframe.c
    src/shred/entry.c
    src/tx/view.c
    src/tx/resolve.c
    src/tx/bind.c
    src/tx/pumpstate.c
    src/state/boot.c
    src/state/authvec.c
    src/state/live.c
    src/wire/classify.c
    src/wire/lut_file.c
    src/wire/lut_parse.c
    src/wire/lut_cache.c)
target_link_libraries(state PUBLIC pump)

add_executable(test_pump_vectors tests/state/test_pump_vectors.c)
target_link_libraries(test_pump_vectors PRIVATE pump)
add_test(NAME test_pump_vectors COMMAND test_pump_vectors)
set_tests_properties(test_pump_vectors PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(test_overlay tests/state/test_overlay.c)
target_link_libraries(test_overlay PRIVATE state)
add_test(NAME test_overlay COMMAND test_overlay)

add_executable(test_apply tests/state/test_apply.c)
target_link_libraries(test_apply PRIVATE state)
add_test(NAME test_apply COMMAND test_apply)

add_executable(test_pump_corpus tests/state/test_pump_corpus.c)
target_link_libraries(test_pump_corpus PRIVATE pump)
add_test(NAME test_pump_corpus COMMAND test_pump_corpus)
set_tests_properties(test_pump_corpus PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(test_token tests/state/test_token.c)
target_link_libraries(test_token PRIVATE state)
add_test(NAME test_token COMMAND test_token)

add_executable(test_oracle tests/state/test_oracle.c)
target_link_libraries(test_oracle PRIVATE state)
add_test(NAME test_oracle COMMAND test_oracle)

add_executable(test_pump_oracle tests/state/test_pump_oracle.c)
target_link_libraries(test_pump_oracle PRIVATE state)
add_test(NAME test_pump_oracle COMMAND test_pump_oracle)
set_tests_properties(test_pump_oracle PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(test_pump_oracle002 tests/state/test_pump_oracle002.c)
target_link_libraries(test_pump_oracle002 PRIVATE state)
add_test(NAME test_pump_oracle002 COMMAND test_pump_oracle002)
set_tests_properties(test_pump_oracle002 PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(state_bench bench/state/main.c)
target_link_libraries(state_bench PRIVATE state net_core)

add_executable(state_pipeline_bench bench/state_pipeline/main.c)
target_link_libraries(state_pipeline_bench PRIVATE state net_core)

add_executable(test_decode tests/ingress/test_decode.c)
target_link_libraries(test_decode PRIVATE state)
add_test(NAME test_decode COMMAND test_decode)

add_executable(test_wire tests/wire/test_wire.c)
target_link_libraries(test_wire PRIVATE state)
add_test(NAME test_wire COMMAND test_wire)

add_executable(wire_to_state_bench bench/wire_to_state/main.c)
target_link_libraries(wire_to_state_bench PRIVATE state net_core)

add_executable(test_assemble tests/shred/test_assemble.c)
target_link_libraries(test_assemble PRIVATE state)
add_test(NAME test_assemble COMMAND test_assemble)

add_executable(test_capio tests/net/test_capio.c)
target_link_libraries(test_capio PRIVATE net_core)
add_test(NAME test_capio COMMAND test_capio)

add_executable(wire_to_state002 bench/wire_to_state002/main.c)
target_link_libraries(wire_to_state002 PRIVATE state net_core)

add_executable(test_rs tests/shred/test_rs.c)
target_link_libraries(test_rs PRIVATE state)
add_test(NAME test_rs COMMAND test_rs)

add_executable(test_merkle tests/shred/test_merkle.c)
target_link_libraries(test_merkle PRIVATE state)
add_test(NAME test_merkle COMMAND test_merkle)

add_executable(test_fec tests/shred/test_fec.c)
target_link_libraries(test_fec PRIVATE state)
add_test(NAME test_fec COMMAND test_fec)

add_executable(wire_to_state003 bench/wire_to_state003/main.c)
target_link_libraries(wire_to_state003 PRIVATE state net_core)

add_executable(test_authstate tests/state/test_authstate.c)
target_link_libraries(test_authstate PRIVATE state)
add_test(NAME test_authstate COMMAND test_authstate)
set_tests_properties(test_authstate PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(test_catchup tests/state/test_catchup.c)
target_link_libraries(test_catchup PRIVATE state)
add_test(NAME test_catchup COMMAND test_catchup)

add_executable(test_lut_cache tests/wire/test_lut_cache.c)
target_link_libraries(test_lut_cache PRIVATE state)
add_test(NAME test_lut_cache COMMAND test_lut_cache)
set_tests_properties(test_lut_cache PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(test_livestate tests/state/test_livestate.c)
target_link_libraries(test_livestate PRIVATE state)
add_test(NAME test_livestate COMMAND test_livestate)
set_tests_properties(test_livestate PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(authstate_001 bench/authstate/main.c)
target_link_libraries(authstate_001 PRIVATE state net_core shred)

add_executable(live_state_001 apps/live_state/main.c)
target_link_libraries(live_state_001 PRIVATE state net_core shred)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_compile_definitions(_GNU_SOURCE)
    add_library(net_linux STATIC src/net/udp.c src/net/feed.c)
    set_property(TARGET net_linux PROPERTY C_EXTENSIONS ON)
    target_link_libraries(net_linux PUBLIC net_core)

    add_executable(test_rx tests/net/test_rx.c)
    target_link_libraries(test_rx PRIVATE net_linux shred)
    add_test(NAME test_rx COMMAND test_rx)

    add_executable(rx_sink apps/rx_sink/main.c)
    target_link_libraries(rx_sink PRIVATE net_linux shred)
    target_link_libraries(live_state_001 PRIVATE net_linux)
    set_property(TARGET live_state_001 PROPERTY C_EXTENSIONS ON)
    target_link_libraries(rx_bench PRIVATE net_linux)
endif()
