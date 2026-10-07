cmake_minimum_required(VERSION 3.16)
project(arb-feed VERSION 0 LANGUAGES C)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "arb-feed targets Linux")
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

set(CORE_CLASSIFY "${CMAKE_SOURCE_DIR}/../arb-core/src/classify.c")
if(NOT EXISTS "${CORE_CLASSIFY}")
    message(FATAL_ERROR "arb-core classify.c not next to arb-feed")
endif()

add_library(feed OBJECT
    src/udp_source.c
    src/recorder.c
    src/observe.c
    ${CORE_CLASSIFY})
set_property(TARGET feed PROPERTY C_EXTENSIONS ON)
target_compile_options(feed PRIVATE -Wno-pedantic)

add_executable(feed001 tests/feed001.c $<TARGET_OBJECTS:feed>)
target_link_libraries(feed001 pthread)

add_executable(feed_load tests/feed_load.c $<TARGET_OBJECTS:feed>)
target_link_libraries(feed_load pthread)

add_executable(feed_live src/live.c $<TARGET_OBJECTS:feed>)
target_link_libraries(feed_live pthread)

add_executable(feed_replay src/replay.c $<TARGET_OBJECTS:feed>)
target_link_libraries(feed_replay pthread)

add_executable(cap001 src/cap001.c $<TARGET_OBJECTS:feed>)
target_link_libraries(cap001 pthread)

add_executable(cap002 src/cap002.c $<TARGET_OBJECTS:feed>)
target_link_libraries(cap002 pthread)

add_executable(entry_truth src/entry_truth.c $<TARGET_OBJECTS:feed>)
target_link_libraries(entry_truth pthread)

add_executable(frame_truth src/frame_truth.c
    ${CMAKE_SOURCE_DIR}/../arb-core/src/frame.c
    $<TARGET_OBJECTS:feed>)
target_include_directories(frame_truth PRIVATE
    ${CMAKE_SOURCE_DIR}/../arb-core/include)
target_link_libraries(frame_truth pthread)

set(CORE_TX "${CMAKE_SOURCE_DIR}/../arb-core/src/tx.c")
set(CORE_POOL "${CMAKE_SOURCE_DIR}/../arb-core/src/pool.c")
add_executable(cap003 src/cap003.c ${CORE_TX} ${CORE_POOL} $<TARGET_OBJECTS:feed>)
target_include_directories(cap003 PRIVATE ${CMAKE_SOURCE_DIR}/../arb-core/include)
target_link_libraries(cap003 pthread)

add_executable(cap004 src/cap004.c $<TARGET_OBJECTS:feed>)
target_link_libraries(cap004 pthread)

set(CORE_ROOT "${CMAKE_SOURCE_DIR}/../arb-core")
set(EXEC_ROOT "${CMAKE_SOURCE_DIR}/../arb-exec")
add_executable(cap009 src/cap009.c
    ${CORE_ROOT}/src/classify_n.c
    ${CORE_ROOT}/src/tx.c
    ${CORE_ROOT}/src/tx_n.c
    ${CORE_ROOT}/src/adapter.c
    ${CORE_ROOT}/src/universe.c
    ${CORE_ROOT}/src/hot.c
    ${EXEC_ROOT}/src/route0.c
    ${EXEC_ROOT}/src/tx_template.c
    ${EXEC_ROOT}/src/route_fam.c
    ${EXEC_ROOT}/src/nonce.c
    ${EXEC_ROOT}/src/sign.c
    $<TARGET_OBJECTS:feed>)
target_include_directories(cap009 PRIVATE
    ${CORE_ROOT}/include
    ${EXEC_ROOT}/include)
set_property(TARGET cap009 PROPERTY C_EXTENSIONS ON)
target_compile_options(cap009 PRIVATE -Wno-pedantic)
target_link_libraries(cap009
    pthread
    /usr/lib/x86_64-linux-gnu/libsodium.so.23
    crypto)

add_executable(decode_feed src/decode_feed.c
    ${CORE_ROOT}/src/tx.c
    ${CORE_ROOT}/src/swapix.c
    $<TARGET_OBJECTS:feed>)
target_include_directories(decode_feed PRIVATE
    ${CORE_ROOT}/include)
set_property(TARGET decode_feed PROPERTY C_EXTENSIONS ON)
target_compile_options(decode_feed PRIVATE -Wno-pedantic)
target_link_libraries(decode_feed
    pthread
    crypto)

add_executable(paper_live001 src/paper_live001.c
    ${CORE_ROOT}/src/classify_n.c
    ${CORE_ROOT}/src/tx.c
    ${CORE_ROOT}/src/tx_n.c
    ${CORE_ROOT}/src/pool.c
    ${CORE_ROOT}/src/adapter.c
    ${CORE_ROOT}/src/universe.c
    ${CORE_ROOT}/src/hot.c
    ${CORE_ROOT}/src/swapix.c
    ${CORE_ROOT}/src/syncrec.c
    ${CORE_ROOT}/src/live.c
    ${CORE_ROOT}/src/paper.c
    ${CORE_ROOT}/src/cycle.c
    ${CORE_ROOT}/src/dex/meteora_dlmm.c
    ${CORE_ROOT}/src/dex/dlmm_cache.c
    ${CORE_ROOT}/src/dex/pump.c
    ${EXEC_ROOT}/src/route0.c
    ${EXEC_ROOT}/src/tx_template.c
    ${EXEC_ROOT}/src/route0_v0.c
    ${EXEC_ROOT}/src/sign.c
    $<TARGET_OBJECTS:feed>)
target_include_directories(paper_live001 PRIVATE
    ${CORE_ROOT}/include
    ${EXEC_ROOT}/include)
set_property(TARGET paper_live001 PROPERTY C_EXTENSIONS ON)
target_compile_options(paper_live001 PRIVATE -Wno-pedantic)
target_link_libraries(paper_live001
    pthread
    /usr/lib/x86_64-linux-gnu/libsodium.so.23
    crypto)

add_executable(paper_orbit src/paper_orbit.c
    src/authpub.c
    src/tx_overlay.c
    ${CORE_ROOT}/src/classify_n.c
    ${CORE_ROOT}/src/tx.c
    ${CORE_ROOT}/src/tx_n.c
    ${CORE_ROOT}/src/pool.c
    ${CORE_ROOT}/src/adapter.c
    ${CORE_ROOT}/src/universe.c
    ${CORE_ROOT}/src/hot.c
    ${CORE_ROOT}/src/state003.c
    ${CORE_ROOT}/src/swapix.c
    ${CORE_ROOT}/src/syncrec.c
    ${CORE_ROOT}/src/live.c
    ${CORE_ROOT}/src/universe_gen.c
    ${CORE_ROOT}/src/cycle.c
    ${CORE_ROOT}/src/paper.c
    ${CORE_ROOT}/src/frame.c
    ${CORE_ROOT}/src/alt_cache.c
    ${CORE_ROOT}/src/watch.c
    ${CORE_ROOT}/src/trigger.c
    ${CORE_ROOT}/src/dex/meteora_dlmm.c
    ${CORE_ROOT}/src/dex/dlmm_cache.c
    ${CORE_ROOT}/src/dex/pump.c
    ${EXEC_ROOT}/src/route0.c
    ${EXEC_ROOT}/src/tx_template.c
    ${EXEC_ROOT}/src/route0_v0.c
    ${EXEC_ROOT}/src/sign.c
    $<TARGET_OBJECTS:feed>)
target_include_directories(paper_orbit PRIVATE
    ${CORE_ROOT}/include
    ${EXEC_ROOT}/include)
set_property(TARGET paper_orbit PROPERTY C_EXTENSIONS ON)
target_compile_options(paper_orbit PRIVATE -Wno-pedantic)
target_link_libraries(paper_orbit
    pthread
    /usr/lib/x86_64-linux-gnu/libsodium.so.23
    crypto)

add_executable(state005_n src/state005_n.c
    ${CORE_ROOT}/src/classify_n.c
    ${CORE_ROOT}/src/tx.c
    ${CORE_ROOT}/src/tx_n.c
    ${CORE_ROOT}/src/swapix.c
    $<TARGET_OBJECTS:feed>)
target_include_directories(state005_n PRIVATE
    ${CORE_ROOT}/include)
set_property(TARGET state005_n PROPERTY C_EXTENSIONS ON)
target_compile_options(state005_n PRIVATE -Wno-pedantic)
target_link_libraries(state005_n
    pthread
    crypto)

add_executable(paper004 src/paper004.c
    ${CORE_ROOT}/src/tx.c
    ${CORE_ROOT}/src/pool.c
    ${CORE_ROOT}/src/adapter.c
    ${CORE_ROOT}/src/universe.c
    ${CORE_ROOT}/src/hot.c
    ${CORE_ROOT}/src/swapix.c
    ${CORE_ROOT}/src/live.c
    ${CORE_ROOT}/src/cycle.c
    ${CORE_ROOT}/src/dex/meteora_dlmm.c
    ${CORE_ROOT}/src/dex/dlmm_cache.c
    ${CORE_ROOT}/src/dex/pump.c)
target_include_directories(paper004 PRIVATE
    ${CORE_ROOT}/include
    ${EXEC_ROOT}/include)
set_property(TARGET paper004 PROPERTY C_EXTENSIONS ON)
target_compile_options(paper004 PRIVATE -Wno-pedantic)
target_link_libraries(paper004
    pthread
    /usr/lib/x86_64-linux-gnu/libsodium.so.23
    crypto)
