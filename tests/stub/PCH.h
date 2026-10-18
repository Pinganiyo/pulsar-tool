#pragma once

// Test-build stand-in for include/PCH.h. The real one pulls in CommonLibSF and
// the game headers; the sources compiled into pulsar_tests must not need them.
// Logging compiles to nothing here.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#define PLOG_INFO(...) ((void)0)
#define PLOG_WARN(...) ((void)0)
#define PLOG_DIAG(...) ((void)0)
