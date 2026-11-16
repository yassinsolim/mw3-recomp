#pragma once
// The thread label kernel objects record for their diagnostics, for tests that
// register no threads. Include it from exactly one translation unit per test
// executable, and not where the test names its own threads.
#include "runtime/crash.h"

namespace crash
{
    const char* CurrentThreadName() { return nullptr; }
}
