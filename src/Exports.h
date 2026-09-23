#pragma once

namespace Exports {
    // TempFile_GetAPI returns nullptr until this is called (hooks installed successfully).
    // Better that a consumer knows the framework is not active than have it "create" temp files
    // the game will never see.
    void Enable();
}
