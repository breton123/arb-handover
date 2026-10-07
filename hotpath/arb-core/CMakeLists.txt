cmake_minimum_required(VERSION 3.16)
project(arb-core VERSION 0 LANGUAGES C)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "arb-core targets Linux")
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

add_library(core OBJECT
    src/classify.c
    src/classify_n.c
    src/tx.c
    src/tx_n.c
    src/pool.c
    src/adapter.c
    src/universe.c
    src/hot.c
    src/dex/meteora_dlmm.c
    src/dex/dlmm_cache.c
    src/dex/pump.c
    src/cycle.c
    src/frame.c
    src/live.c
    src/paper.c
    src/state003.c)
set_property(TARGET core PROPERTY C_EXTENSIONS ON)
target_compile_options(core PRIVATE -Wno-pedantic)

add_executable(replay tests/replay.c $<TARGET_OBJECTS:core>)
add_executable(build_pools tests/build_pools.c $<TARGET_OBJECTS:core>)
add_executable(core001 tests/core001.c $<TARGET_OBJECTS:core>)
add_executable(core002 tests/core002.c $<TARGET_OBJECTS:core>)
add_executable(core003 tests/core003.c $<TARGET_OBJECTS:core>)
set_property(TARGET core003 PROPERTY C_EXTENSIONS ON)
target_compile_options(core003 PRIVATE -Wno-pedantic)

add_executable(core004 tests/core004.c $<TARGET_OBJECTS:core>)
set_property(TARGET core004 PROPERTY C_EXTENSIONS ON)
target_compile_options(core004 PRIVATE -Wno-pedantic)

add_executable(core005 tests/core005.c $<TARGET_OBJECTS:core>)
set_property(TARGET core005 PROPERTY C_EXTENSIONS ON)
target_compile_options(core005 PRIVATE -Wno-pedantic)

add_executable(core006 tests/core006.c $<TARGET_OBJECTS:core>)
set_property(TARGET core006 PROPERTY C_EXTENSIONS ON)
target_compile_options(core006 PRIVATE -Wno-pedantic)

add_executable(core007 tests/core007.c $<TARGET_OBJECTS:core>)
set_property(TARGET core007 PROPERTY C_EXTENSIONS ON)
target_compile_options(core007 PRIVATE -Wno-pedantic)

add_executable(core008 tests/core008.c $<TARGET_OBJECTS:core>)
set_property(TARGET core008 PROPERTY C_EXTENSIONS ON)
target_compile_options(core008 PRIVATE -Wno-pedantic)

add_executable(core009 tests/core009.c $<TARGET_OBJECTS:core>)
set_property(TARGET core009 PROPERTY C_EXTENSIONS ON)
target_compile_options(core009 PRIVATE -Wno-pedantic)

add_executable(core010 tests/core010.c $<TARGET_OBJECTS:core>)
set_property(TARGET core010 PROPERTY C_EXTENSIONS ON)
target_compile_options(core010 PRIVATE -Wno-pedantic)

add_executable(live001 tests/live001.c $<TARGET_OBJECTS:core>)
add_executable(live002 tests/live002.c $<TARGET_OBJECTS:core>)
set_property(TARGET live002 PROPERTY C_EXTENSIONS ON)
target_compile_options(live002 PRIVATE -Wno-pedantic)
add_executable(paper002 tests/paper002.c $<TARGET_OBJECTS:core>)
set_property(TARGET paper002 PROPERTY C_EXTENSIONS ON)
target_compile_options(paper002 PRIVATE -Wno-pedantic)
set_property(TARGET live001 PROPERTY C_EXTENSIONS ON)
target_compile_options(live001 PRIVATE -Wno-pedantic)

add_executable(hot001 tests/hot001.c $<TARGET_OBJECTS:core>)
set_property(TARGET hot001 PROPERTY C_EXTENSIONS ON)
target_compile_options(hot001 PRIVATE -Wno-pedantic)

add_executable(hot_n tests/hot_n.c src/swapix.c src/syncrec.c $<TARGET_OBJECTS:core>)
set_property(TARGET hot_n PROPERTY C_EXTENSIONS ON)
target_compile_options(hot_n PRIVATE -Wno-pedantic)
target_link_libraries(hot_n crypto)

add_executable(state002 tests/state002.c $<TARGET_OBJECTS:core>)
set_property(TARGET state002 PROPERTY C_EXTENSIONS ON)
target_compile_options(state002 PRIVATE -Wno-pedantic)

add_executable(state003 tests/state003.c $<TARGET_OBJECTS:core>)
set_property(TARGET state003 PROPERTY C_EXTENSIONS ON)
target_compile_options(state003 PRIVATE -Wno-pedantic)

add_executable(state003_walk tests/state003_walk.c $<TARGET_OBJECTS:core>)
set_property(TARGET state003_walk PROPERTY C_EXTENSIONS ON)
target_compile_options(state003_walk PRIVATE -Wno-pedantic)

add_executable(state004 tests/state004.c $<TARGET_OBJECTS:core>)
set_property(TARGET state004 PROPERTY C_EXTENSIONS ON)
target_compile_options(state004 PRIVATE -Wno-pedantic)

add_executable(state005 tests/state005.c $<TARGET_OBJECTS:core>)
set_property(TARGET state005 PROPERTY C_EXTENSIONS ON)
target_compile_options(state005 PRIVATE -Wno-pedantic)

add_executable(route0_orient tests/route0_orient.c $<TARGET_OBJECTS:core>)
set_property(TARGET route0_orient PROPERTY C_EXTENSIONS ON)
target_compile_options(route0_orient PRIVATE -Wno-pedantic)

add_executable(dump_closure tests/dump_closure.c $<TARGET_OBJECTS:core>)
set_property(TARGET dump_closure PROPERTY C_EXTENSIONS ON)
target_compile_options(dump_closure PRIVATE -Wno-pedantic)

add_executable(univ_gen tests/univ_gen.c src/universe_gen.c src/watch.c
    $<TARGET_OBJECTS:core>)
set_property(TARGET univ_gen PROPERTY C_EXTENSIONS ON)
target_compile_options(univ_gen PRIVATE -Wno-pedantic)

add_executable(trigger011 tests/trigger011.c
    src/swapix.c src/alt_cache.c src/watch.c src/trigger.c
    $<TARGET_OBJECTS:core>)
set_property(TARGET trigger011 PROPERTY C_EXTENSIONS ON)
target_compile_options(trigger011 PRIVATE -Wno-pedantic)
target_link_libraries(trigger011 crypto)

add_executable(frame_v1 tests/frame_v1.c
    src/swapix.c src/alt_cache.c src/watch.c src/trigger.c
    $<TARGET_OBJECTS:core>)
set_property(TARGET frame_v1 PROPERTY C_EXTENSIONS ON)
target_compile_options(frame_v1 PRIVATE -Wno-pedantic)
target_link_libraries(frame_v1 crypto)

add_executable(state_apply src/state_apply.c src/swapix.c $<TARGET_OBJECTS:core>)
set_property(TARGET state_apply PROPERTY C_EXTENSIONS ON)
target_compile_options(state_apply PRIVATE -Wno-pedantic)
target_link_libraries(state_apply crypto)

# Offline STATE-011 kernel. Not part of `core`. Do not ship in paper_orbit until soak ends.
add_executable(dlmm_orders002 tests/dlmm_orders002.c src/dex/dlmm_orders.c)
set_property(TARGET dlmm_orders002 PROPERTY C_EXTENSIONS ON)
target_compile_options(dlmm_orders002 PRIVATE -Wno-pedantic)
