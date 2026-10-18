#pragma once

// Tiny self-contained test harness (no dependency). Usage:
//   TEST(Suite, Name) { CHECK(x); CHECK_EQ(a, b); REQUIRE(ptr); }
// Run: pulsar_tests.exe [filter]   - filter is a substring of "Suite.Name".

#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace pt {

    struct TestCase {
        const char*           name;
        std::function<void()> fn;
    };

    inline std::vector<TestCase>& Registry()
    {
        static std::vector<TestCase> r;
        return r;
    }

    struct Registrar {
        Registrar(const char* a_name, std::function<void()> a_fn)
        {
            Registry().push_back({ a_name, std::move(a_fn) });
        }
    };

    struct State {
        int failures = 0;
    };

    inline State& Current()
    {
        static State s;
        return s;
    }

    struct RequireFailed {};

    inline void Fail(const char* a_file, int a_line, const std::string& a_msg)
    {
        ++Current().failures;
        std::printf("    FAIL %s(%d): %s\n", a_file, a_line, a_msg.c_str());
    }

    template <class A, class B>
    std::string Describe(const A& a, const B& b)
    {
        std::ostringstream os;
        if constexpr (requires(std::ostream& o) { o << a; o << b; }) {
            os << "got [" << a << "] expected [" << b << "]";
        } else {
            os << "values differ";
        }
        return os.str();
    }
}

#define PT_CAT2(a, b) a##b
#define PT_CAT(a, b)  PT_CAT2(a, b)

#define TEST(suite, name)                                                        \
    static void PT_CAT(pt_test_, PT_CAT(suite, PT_CAT(_, name)))();             \
    static ::pt::Registrar PT_CAT(pt_reg_, PT_CAT(suite, PT_CAT(_, name)))(      \
        #suite "." #name, &PT_CAT(pt_test_, PT_CAT(suite, PT_CAT(_, name))));    \
    static void PT_CAT(pt_test_, PT_CAT(suite, PT_CAT(_, name)))()

#define CHECK(cond)                                                              \
    do { if (!(cond)) ::pt::Fail(__FILE__, __LINE__, "CHECK(" #cond ")"); } while (0)

#define CHECK_EQ(a, b)                                                           \
    do {                                                                         \
        const auto& pt_a = (a);                                                  \
        const auto& pt_b = (b);                                                  \
        if (!(pt_a == pt_b))                                                     \
            ::pt::Fail(__FILE__, __LINE__,                                       \
                       "CHECK_EQ(" #a ", " #b "): " + ::pt::Describe(pt_a, pt_b)); \
    } while (0)

// Stops the current test on failure (use when the rest would crash).
#define REQUIRE(cond)                                                            \
    do {                                                                         \
        if (!(cond)) {                                                           \
            ::pt::Fail(__FILE__, __LINE__, "REQUIRE(" #cond ")");                \
            throw ::pt::RequireFailed{};                                         \
        }                                                                        \
    } while (0)
