# pulsar_tests: game-free unit and loopback tests. Builds next to PULSAR.dll and
# runs without Starfield. See docs/TESTING.md.

add_executable(
  pulsar_tests
  main.cpp
  test_profile_codec.cpp
  test_crypto.cpp
  test_relay_and_packets.cpp
  test_loopback.cpp
  # Production sources under test (must stay free of game headers):
  ${PROJECT_SOURCE_DIR}/src/Profile/ProfileCodec.cpp
  ${PROJECT_SOURCE_DIR}/src/Network/Crypto.cpp
)

# tests/stub comes FIRST so `#include "PCH.h"` in the production sources picks
# up the game-free stand-in instead of include/PCH.h.
target_include_directories(
  pulsar_tests
  BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stub
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${PROJECT_SOURCE_DIR}/include
)

target_compile_features(pulsar_tests PRIVATE cxx_std_23)
target_compile_options(pulsar_tests PRIVATE /W4 /utf-8 /EHsc /permissive- /Zc:__cplusplus)
target_compile_definitions(
  pulsar_tests
  PRIVATE
  NOMINMAX
  WIN32_LEAN_AND_MEAN
  "PULSAR_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\""
)
target_link_libraries(pulsar_tests PRIVATE unofficial::enet::enet bcrypt ws2_32 winmm)

add_test(NAME pulsar_tests COMMAND pulsar_tests)
