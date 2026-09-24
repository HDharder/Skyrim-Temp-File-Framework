#pragma once

namespace Diagnostics {
    // Logs the caller's stack (diagnostic trace only). Frames inside SkyrimSE.exe are shown as the
    // RVA, the start of the enclosing function and its Address Library ID, so they can be looked
    // up in a disassembly. Capped per trace session to keep the log readable.
    void LogCallerStack();

    // Re-arms the cap (called whenever the trace is turned on).
    void ResetCallerStackBudget();
}
