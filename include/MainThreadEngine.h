#pragma once

#include <MainThread.h>

#include <cstdint>
#include <optional>
#include <string>

namespace RE
{
    class TESForm;
}

// Token-gated engine-read wrappers. Every function here demands a
// `MainThread::Token` as its first parameter, which is unforgeable
// outside a `MainThread::Run` / `MainThread::FireAndForget` lambda.
// Off-main callers cannot construct a token, so they cannot invoke
// these functions — a compile error catches every attempt.
//
// The wrappers return plain, self-contained data (no `RE::*` pointers,
// no references into engine-owned memory). This is the return-by-value
// discipline that keeps engine-owned state from leaking back to a
// worker thread that would race against the main thread's mutations.
//
// This is the deliberately-small starter set the phase 10 substrate
// ships with. Additional wrappers land in later phases as those phases
// touch new engine surface. Do NOT add wrappers speculatively.
namespace NarrativeEngine::MainThreadEngine
{
    struct Vec3
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    struct PlayerSnapshot
    {
        std::uint32_t formID = 0;

        // Current location — 0 / empty when the player is in a cell
        // with no BGSLocation assignment (unmarked wilderness).
        std::uint32_t locationFormID = 0;
        std::string locationName;

        // Current cell — always populated when the player is loaded.
        std::uint32_t cellFormID = 0;
        std::string cellName;
        bool cellIsInterior = false;

        Vec3 position;
    };

    struct ActorSnapshot
    {
        std::uint32_t formID = 0;
        std::string displayName;
        bool isDead = false;
        bool isDisabled = false;
        bool isInCombat = false;
        bool isPlayerTeammate = false;
        bool isBleedingOut = false;
        Vec3 position;
    };

    enum class SkyMode : std::uint8_t
    {
        Interior = 0,
        SkyDomeOnly,
        Full,
    };

    struct SkySnapshot
    {
        SkyMode mode = SkyMode::Interior;

        // 0 when no current weather is resolved (sky mode is Interior,
        // or the pointer transiently races through a save/load).
        std::uint32_t currentWeatherFormID = 0;

        // Raw flag byte off `TESWeather::data.flags`, so callers can
        // apply their own precedence (snow > rain > pleasant > cloudy).
        // Untouched by the wrapper — 0 when no current weather.
        std::uint8_t weatherFlags = 0;

        std::uint8_t windSpeed = 0;
        std::int8_t thunderLightningFrequency = 0;
    };

    // Read the player's identity / location / cell / position from the
    // engine, all in one main-thread hop. Returns a zero-initialized
    // struct if the player singleton is unavailable (pre-load / mid-
    // teardown).
    PlayerSnapshot ReadPlayerSnapshot(const MainThread::Token&);

    // Look up an actor by FormID and return a snapshot of its
    // narratively-relevant state. Returns nullopt if the form doesn't
    // resolve, isn't an Actor, or has no usable display name.
    std::optional<ActorSnapshot> LookupActor(const MainThread::Token&, std::uint32_t formID);

    // Snapshot of the current sky state. Returns nullopt if the Sky
    // singleton is unavailable.
    std::optional<SkySnapshot> ReadCurrentSky(const MainThread::Token&);
} // namespace NarrativeEngine::MainThreadEngine
