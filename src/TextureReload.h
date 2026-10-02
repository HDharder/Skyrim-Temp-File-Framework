// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 HDharder, Temp File Framework, https://github.com/HDharder/Skyrim-Temp-File-Framework

#pragma once

// Reloading textures the engine has already loaded.
//
// The engine keeps one NiSourceTexture per loaded texture path, and every object using that
// texture points at it. NiSourceTexture (+0x48) owns a BSGraphics::Texture holding the D3D texture
// and its view. Reloading = read the file again through the engine's resource system (which sees
// the temp file or the original, as the index says), create a new D3D texture through the normal
// CreateTexture2D (so other plugins hooking it, like texture downscalers, still decide the size),
// and on the main thread, between frames, swap texture and view inside that BSGraphics::Texture.
//
// Live textures are tracked by two hooks: BSShaderResourceManager's texture loader (to know which
// NiSourceTexture is loading) and ID3D11Device::CreateTexture2D (to pair it with its D3D texture).
// Both are vtable slots shared with other plugins (Engine Fixes, texture downscalers); whatever is
// there is chained into.
namespace TextureReload {
    // kDataLoaded: the renderer exists by then.
    bool Install();

    // Queues a reload of every live texture loaded from this path (relative to Data, any case,
    // '/' or '\'). Returns how many live textures were queued (0: the path is not loaded right now).
    // Does nothing (returns 0) for non-.dds paths, before Install, or until the layout check passed.
    std::int32_t Request(std::wstring_view a_rel);

    // Stops the reload queue (the game is closing). Safe to call more than once.
    void Shutdown();

    // The NiSourceTexture being re-created on the calling thread right now (only inside the
    // CreateTexture2D call a reload makes), else nullptr. Lets plugins that size textures by file
    // name find the name when the engine's loader is not on the stack.
    void* ReloadingTexture();
}
