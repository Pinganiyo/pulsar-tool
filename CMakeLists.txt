cmake_minimum_required(VERSION 3.30)
message("Using toolchain file ${CMAKE_TOOLCHAIN_FILE}.")

# ---- Project ----
project(
  PULSAR
  VERSION 0.7.2   # keep in sync with include/Plugin.h Version (drives version.rc file properties)
  DESCRIPTION "Co-op Mod for Starfield"
  LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OPTIMIZE_DEPENDENCIES ON)

# ---- Dependencies ----
find_package(spdlog CONFIG REQUIRED)
find_package(unofficial-enet CONFIG REQUIRED)
find_package(minhook CONFIG REQUIRED)
find_package(ZLIB REQUIRED)
find_package(imgui CONFIG REQUIRED)

# CommonLibSF from local submodule (libxse/CommonLibSF, supports Starfield 1.16.236)
include(cmake/CommonLibSF.cmake)

# ---- Version resource ----
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.rc.in
  ${CMAKE_CURRENT_BINARY_DIR}/version.rc
  @ONLY
)

# ---- Sources ----
file(
  GLOB_RECURSE
  sources
  CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h
  ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/version.rc
)

source_group(
  TREE ${CMAKE_CURRENT_SOURCE_DIR}
  FILES ${sources}
)

# ---- Plugin DLL ----
add_commonlibsf_plugin(
  ${PROJECT_NAME}
  AUTHOR PULSAR-Team
  SOURCES ${sources}
)

add_library(${PROJECT_NAME}::${PROJECT_NAME} ALIAS ${PROJECT_NAME})

target_link_libraries(
  ${PROJECT_NAME}
  PRIVATE
  unofficial::enet::enet
  minhook::minhook
  ZLIB::ZLIB
  imgui::imgui
  d3d12.lib
  dxgi.lib
)

target_include_directories(
  ${PROJECT_NAME}
  BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_precompile_headers(
  ${PROJECT_NAME}
  PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include/PCH.h
)

# ---- Compiler options ----
target_compile_options(
  ${PROJECT_NAME}
  PRIVATE
  /cgthreads8 /diagnostics:caret /MP /W4
  /Zc:__cplusplus /Zc:enumTypes /Zc:inline /Zc:templateScope
)

if(CMAKE_BUILD_TYPE STREQUAL RelWithDebInfo)
  target_compile_options(
    ${PROJECT_NAME}
    PRIVATE
    /fp:fast /Ob3 /GL /GR- /Gw /Qpar
  )
  target_link_options(
    ${PROJECT_NAME}
    PRIVATE
    /CGTHREADS:8 /INCREMENTAL:NO /OPT:REF,ICF=4
  )
endif()

# ---- Release = HARDENED (IP-theft protection) ----
# RelWithDebInfo stays the DEV build (full PLOG logging + PDB) for the crash-dump
# workflow. The Release config is the ONLY thing shipped: it compiles INFO/WARN logs
# OUT (PULSAR_HARDENED, see include/Log.h), emits NO PDB, and folds/strips unused code.
# Genex-gated so it applies in the multi-config VS generator without a separate config.
target_compile_definitions(
  ${PROJECT_NAME}
  PRIVATE
  $<$<CONFIG:Release>:PULSAR_HARDENED=1>
)

# __FILE__ otherwise bakes the ABSOLUTE dev path (<repo>\src\...) into the
# shipped DLL via CommonLibSF helpers (report_and_fail, BSTArray/REL asserts use
# std::source_location). /d1trimfile strips the repo-root prefix so only short relative
# paths remain - no dev-machine layout leaks. Native (backslash) form to match __FILE__.
file(TO_NATIVE_PATH "${CMAKE_SOURCE_DIR}/" PULSAR_SRC_ROOT_NATIVE)

target_compile_options(
  ${PROJECT_NAME}
  PRIVATE
  $<$<CONFIG:Release>:/Gy>
  $<$<CONFIG:Release>:/Gw>
  $<$<CONFIG:Release>:/GL>
  $<$<CONFIG:Release>:/GR->
  $<$<CONFIG:Release>:/Zi>           # emit debug info -> a PDB kept PRIVATE for crash symbolication
  "$<$<CONFIG:Release>:/d1trimfile:${PULSAR_SRC_ROOT_NATIVE}>"  # strip dev path from __FILE__
)
target_link_options(
  ${PROJECT_NAME}
  PRIVATE
  # /DEBUG generates PULSAR.pdb. It is kept PRIVATE (crash-dump symbolication) and NEVER
  # shipped - package_release.ps1 excludes it. /OPT:REF,ICF AFTER /DEBUG keep the DLL
  # folded/stripped; /PDBALTPATH embeds only the filename, not the build path.
  $<$<CONFIG:Release>:/DEBUG>
  $<$<CONFIG:Release>:/PDBALTPATH:%_PDB%>
  $<$<CONFIG:Release>:/INCREMENTAL:NO>
  $<$<CONFIG:Release>:/OPT:REF>
  $<$<CONFIG:Release>:/OPT:ICF>
  $<$<CONFIG:Release>:/LTCG>
)

# ---- Unskippable hardening gate (runs on EVERY build) ----
# Scans the freshly built DLL for leak / reverse-engineering strings. The script no-ops
# for dev configs and FAILS the build (non-zero exit) on Release if ANY identifying
# string is present (FUN_/RVAs/dev paths, or an [PULSAR] string outside the curated
# PLOG_DIAG allow-list). This makes the check impossible to forget or skip: an un-vetted
# hardened DLL can never even be produced, however the build is invoked.
add_custom_command(
  TARGET ${PROJECT_NAME} POST_BUILD
  COMMAND powershell -NoProfile -ExecutionPolicy Bypass
          -File "${CMAKE_SOURCE_DIR}/tools/verify_hardening.ps1"
          -DllPath "$<TARGET_FILE:${PROJECT_NAME}>"
          -Config "$<CONFIG>"
  VERBATIM
  COMMENT "Verifying release hardening (no leak strings)"
)

# ---- Tests (game-free, see tests/ and docs/TESTING.md) ----
option(PULSAR_BUILD_TESTS "Build the pulsar_tests executable" ON)
if(PULSAR_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
