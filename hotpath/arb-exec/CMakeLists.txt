cmake_minimum_required(VERSION 3.16)
project(arb-exec VERSION 0 LANGUAGES C)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "arb-exec targets Linux")
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_compile_definitions(_GNU_SOURCE)
add_compile_options(-Wall -Wextra -Wpedantic -Werror)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O3 -march=native -mtune=native)
endif()

include_directories(${CMAKE_SOURCE_DIR}/include)

add_library(exec OBJECT src/route0.c src/tx_template.c src/route_fam.c src/nonce.c src/leader.c src/ctrl.c)

add_executable(exec001 tests/exec001.c $<TARGET_OBJECTS:exec>)
add_executable(exec002 tests/exec002.c $<TARGET_OBJECTS:exec>)
add_executable(exec_live002 tests/exec_live002.c $<TARGET_OBJECTS:exec>)
add_executable(exec_v0 tests/exec_v0.c src/route0_v0.c src/sign.c $<TARGET_OBJECTS:exec>)
target_link_libraries(exec_v0
    /usr/lib/x86_64-linux-gnu/libsodium.so.23
    crypto)

add_executable(exec003 tests/exec003.c src/sign.c $<TARGET_OBJECTS:exec>)
target_link_libraries(exec003
    /usr/lib/x86_64-linux-gnu/libsodium.so.23
    crypto)

add_executable(exec004 tests/exec004.c $<TARGET_OBJECTS:exec>)
add_executable(exec_ctrl tests/exec_ctrl.c $<TARGET_OBJECTS:exec>)

add_executable(exec005 tests/exec005.c src/sign.c $<TARGET_OBJECTS:exec>)
target_link_libraries(exec005
    /usr/lib/x86_64-linux-gnu/libsodium.so.23
    crypto)

add_executable(exec005a tests/exec005a.c src/sign.c src/b58.c src/smoke_tx.c
    $<TARGET_OBJECTS:exec>)
target_link_libraries(exec005a
    /usr/lib/x86_64-linux-gnu/libsodium.so.23
    crypto)

set(SWQOS_LIB "${CMAKE_SOURCE_DIR}/swqos/target/release/libswqos_quic.a")
add_custom_command(
    OUTPUT "${SWQOS_LIB}"
    COMMAND cargo build --release --manifest-path "${CMAKE_SOURCE_DIR}/swqos/Cargo.toml"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/swqos"
    DEPENDS
        "${CMAKE_SOURCE_DIR}/swqos/src/lib.rs"
        "${CMAKE_SOURCE_DIR}/swqos/Cargo.toml"
    COMMENT "swqos QUIC staticlib")
add_custom_target(swqos_quic DEPENDS "${SWQOS_LIB}")
add_executable(exec_swqos tests/exec_swqos.c)
add_dependencies(exec_swqos swqos_quic)
target_link_libraries(exec_swqos
    "${SWQOS_LIB}"
    pthread
    dl
    m)

add_executable(exec_swqos_oneshot tests/exec_swqos_oneshot.c)
add_dependencies(exec_swqos_oneshot swqos_quic)
target_link_libraries(exec_swqos_oneshot
    "${SWQOS_LIB}"
    pthread
    dl
    m)

add_executable(exec_swqos_racer tests/exec_swqos_racer.c)
add_dependencies(exec_swqos_racer swqos_quic)
target_link_libraries(exec_swqos_racer
    "${SWQOS_LIB}"
    pthread
    dl
    m)

add_executable(exec_swqos_bench tests/exec_swqos_bench.c src/sign.c src/b58.c src/smoke_tx.c)
add_dependencies(exec_swqos_bench swqos_quic)
target_link_libraries(exec_swqos_bench
    "${SWQOS_LIB}"
    /usr/lib/x86_64-linux-gnu/libsodium.so.23
    crypto
    pthread
    dl
    m)

add_executable(exec_swqos_v1 tests/exec_swqos_v1.c src/sign.c src/b58.c src/smoke_tx.c)
add_dependencies(exec_swqos_v1 swqos_quic)
target_link_libraries(exec_swqos_v1
    "${SWQOS_LIB}"
    /usr/lib/x86_64-linux-gnu/libsodium.so.23
    crypto
    pthread
    dl
    m)
