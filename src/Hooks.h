#pragma once

namespace Hooks {
    // Installs the hooks on the file API (kernelbase) of the WHOLE process - the game, SKSE and
    // every other plugin. With MO2 our hook sits IN FRONT of usvfs: we redirect the Data path to
    // the temp file and usvfs lets it through (it is not a Data path).
    bool Install();
}
