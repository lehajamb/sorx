// Test-only process policy. Windows Error Reporting and the Debug CRT can
// otherwise display an invisible modal assertion dialog under headless CTest,
// turning a useful failure into an apparent hang.

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#if defined(_MSC_VER)
#include <crtdbg.h>
#include <stdlib.h>
#endif

namespace {

class TestProcessPolicy final {
public:
    TestProcessPolicy() noexcept {
#if defined(_WIN32)
        const UINT current = GetErrorMode();
        static_cast<void>(SetErrorMode(
            current | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
            SEM_NOOPENFILEERRORBOX));
#endif
#if defined(_MSC_VER)
        static_cast<void>(_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE));
        static_cast<void>(_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR));
        static_cast<void>(_set_abort_behavior(
            0U, _WRITE_ABORT_MSG | _CALL_REPORTFAULT));
#endif
    }
};

[[maybe_unused]] const TestProcessPolicy test_process_policy{};

}  // namespace
