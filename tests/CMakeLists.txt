# pulsar_tests: game-free unit and loopback tests. Builds next to PULSAR.dll and
# runs without Starfield. See docs/TESTING.md.
# pulsar_fakepeer: headless PULSAR client for one-PC testing against the game
# (tests/fakepeer, docs/TESTING.md "Fake peer"). Same game-free build setup.

# Shared settings for the game-free executables.
function(pulsar_game_free_target a_target)
  # tests/stub comes FIRST so `#include "PCH.h"` in the production sources picks
  # up the game-free stand-in instead of include/PCH.h.
  target_include_directories(
    ${a_target}
    BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stub
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include
  )
  target_compile_features(${a_target} PRIVATE cxx_std_23)
  target_compile_options(${a_target} PRIVATE /W4 /utf-8 /EHsc /permissive- /Zc:__cplusplus)
  target_compile_definitions(
    ${a_target}
    PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    "PULSAR_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\""
    PULSAR_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    PULSAR_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    PULSAR_VERSION_PATCH=${PROJECT_VERSION_PATCH}
  )
  target_link_libraries(${a_target} PRIVATE unofficial::enet::enet bcrypt ws2_32 winmm)
endfunction()

add_executable(
  pulsar_tests
  main.cpp
  test_profile_codec.cpp
  test_crypto.cpp
  test_relay_and_packets.cpp
  test_loopback.cpp
  test_fakepeer.cpp
  test_version_compat.cpp
  test_divergence_watch.cpp
  fakepeer/FakePeer.cpp
  # Production sources under test (must stay free of game headers):
  ${PROJECT_SOURCE_DIR}/src/Profile/ProfileCodec.cpp
  ${PROJECT_SOURCE_DIR}/src/Network/Crypto.cpp
)
pulsar_game_free_target(pulsar_tests)

add_executable(
  pulsar_fakepeer
  fakepeer/main.cpp
  fakepeer/FakePeer.cpp
  ${PROJECT_SOURCE_DIR}/src/Network/Crypto.cpp
)
pulsar_game_free_target(pulsar_fakepeer)

add_test(NAME pulsar_tests COMMAND pulsar_tests)
