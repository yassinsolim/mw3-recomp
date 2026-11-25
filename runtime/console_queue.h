#pragma once
// The commands waiting for the console's pump (console.cpp): the timed ones of
// MW2_CONSOLE, those of console::RunNow and the one of console::Place. It
// knows nothing of the title, so what only queues a command -- the input
// script, a key -- is built and tested without one.
#include <string>

namespace console
{
    // The next command that is due, a queued one before the placed one; false
    // when none is. `placed` says it came from console::Place.
    bool Take(std::string& text, bool& placed);
    // Seconds on MW2_CONSOLE's clock.
    double Elapsed();
}
