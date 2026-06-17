#pragma once

namespace NarrativeEngine
{
    // 4-byte identifier registered with SKSE's serialization interface for all
    // of NarrativeEngine's co-save records. Frozen at MVP time — changing it
    // would orphan every previously-saved NarrativeEngine payload. Spells
    // "NRTV" in little-endian ASCII.
    inline constexpr std::uint32_t kCoSaveUniqueID = 'NRTV';

    // Plugin entry point. Called once from the root plugin.cpp's SKSEPluginLoad
    // shim. Returns true on success; returning false aborts plugin load.
    bool Startup(const SKSE::LoadInterface* skse);
}
