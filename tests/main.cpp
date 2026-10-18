#include "TestHarness.h"

#include <cstring>
#include <exception>

int main(int argc, char** argv)
{
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int run = 0, failed = 0;
    for (const auto& t : pt::Registry()) {
        if (filter && !std::strstr(t.name, filter)) continue;
        ++run;
        const int before = pt::Current().failures;
        std::printf("[ RUN  ] %s\n", t.name);
        try {
            t.fn();
        } catch (const pt::RequireFailed&) {
            // already reported
        } catch (const std::exception& e) {
            pt::Fail(__FILE__, __LINE__, std::string("unexpected exception: ") + e.what());
        } catch (...) {
            pt::Fail(__FILE__, __LINE__, "unexpected non-std exception");
        }
        const bool ok = pt::Current().failures == before;
        if (!ok) ++failed;
        std::printf("[ %s ] %s\n", ok ? " OK " : "FAIL", t.name);
    }
    std::printf("\n%d test(s) run, %d failed.\n", run, failed);
    return failed == 0 && run > 0 ? 0 : 1;
}
