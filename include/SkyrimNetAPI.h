#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace RE
{
    class Actor;
}

// Typed, defensive wrapper over SkyrimNet's soft-loaded public C API.
//
// `PublicAPI.h` defines its function pointers at file scope, so it must be
// included in exactly one translation unit — src/SkyrimNetAPI.cpp. Every other
// module in NarrativeEngine talks to SkyrimNet through this namespace.
//
// Every wrapper is defensive: if SkyrimNet is unavailable or the underlying
// function pointer didn't resolve (e.g. older SkyrimNet than we compiled
// against), the wrapper returns a sensible default rather than crashing.
namespace NarrativeEngine::SkyrimNetAPI
{
    // Loads SkyrimNet.dll and resolves all exported function pointers we use.
    // Call once during SKSE's kDataLoaded message. Idempotent — subsequent
    // calls return the cached availability. Returns true iff SkyrimNet was
    // located and at least PublicGetVersion resolved.
    bool Initialize();

    // True after a successful Initialize(). False if SkyrimNet is uninstalled
    // or Initialize hasn't been called yet.
    bool IsAvailable();

    // The runtime API version SkyrimNet reports (PublicGetVersion). Returns
    // -1 if SkyrimNet is unavailable. Used by the dashboard's status pill.
    int GetVersion();

    // Queues an async LLM call against the named SkyrimNet prompt template.
    // The callback fires on a SkyrimNet worker thread — do NOT call RE::*
    // functions from it. The const char* response from SkyrimNet is copied
    // into the std::string passed to our callback (the original is only valid
    // for the duration of the SkyrimNet-side call).
    //
    // Returns false if the underlying function pointer is null or the task
    // failed to queue.
    bool SendCustomPromptToLLM(
        const std::string& promptName,
        const std::string& variant,
        const std::string& contextJson,
        std::function<void(std::string response, bool success)> callback);

    // Returns SkyrimNet's recent-events JSON (an array of event objects with
    // fields: type, text, gameTime, originatingActorName, targetActorName).
    // formId=0 returns the global event stream; an empty filter accepts every
    // event type. Returns "[]" if SkyrimNet is unavailable.
    std::string GetRecentEvents(
        std::uint32_t formId,
        int maxCount,
        const std::string& eventTypeFilter);

    // Registers a custom Inja-template / eligibility-rule decorator with
    // SkyrimNet. The callback must be thread-safe — SkyrimNet calls it from
    // its prompt-rendering thread, which may not be the main thread.
    // Returns false if SkyrimNet is unavailable or registration fails (e.g.
    // name collision with a built-in).
    bool RegisterDecorator(
        const std::string& name,
        const std::string& description,
        std::function<std::string(RE::Actor*)> callback);

    // True if a decorator by this name is already registered (built-in or
    // from another plugin). Use before RegisterDecorator if you want to log
    // collisions.
    bool HasDecorator(const std::string& name);

    // True iff SkyrimNet's memory database is loaded and ready. Memory
    // calls before this returns true crash inside SkyrimNet (per the
    // PublicAPI.h header doc). Returns false if SkyrimNet is
    // unavailable.
    bool IsMemorySystemReady();

    // Convert a Skyrim FormID to SkyrimNet's internal 64-bit UUID.
    // Needed to seed the `npc.UUID` context variable that SkyrimNet's
    // character-profile / system_head submodules key on. Returns 0 if
    // SkyrimNet is unavailable or the actor is unknown to SkyrimNet.
    std::uint64_t FormIDToUUID(std::uint32_t formId);

    // Returns per-actor engagement statistics as a JSON array, ranked
    // by SkyrimNet's internal scoring. Used by NPCLetterAction to pick
    // a sender pool. Empty array if SkyrimNet or its memory system
    // isn't ready.
    //
    // maxCount=0 returns every actor with any activity.
    // shortWindow / mediumWindow are in *game* seconds (86400=1 day,
    // 604800=7 days are sensible defaults).
    std::string GetActorEngagement(int    maxCount,
                                   bool   excludePlayer,
                                   bool   playerEventsOnly,
                                   double shortWindowSeconds,
                                   double mediumWindowSeconds);

    // Returns a JSON array of memory entries for the given actor,
    // optionally biased toward entries matching the contextQuery via
    // SkyrimNet's vector search. Empty contextQuery falls back to
    // recency. Returns "[]" if SkyrimNet or its memory system isn't
    // ready.
    std::string GetMemoriesForActor(std::uint32_t      formId,
                                    int                maxCount,
                                    const std::string& contextQuery);

    // Write a memory to SkyrimNet's per-actor store. Returns the
    // memory's id on success, -1 if SkyrimNet / memory system are
    // unavailable. importance is 0.0..1.0; memoryType is one of
    // SkyrimNet's enums ("EXPERIENCE", "OBSERVATION", ...); emotion is
    // a free-form short tag; location is a player-visible cell / loc
    // name (empty for "unknown").
    int AddMemory(std::uint32_t      formId,
                  const std::string& contentText,
                  float              importance,
                  const std::string& memoryType,
                  const std::string& emotion,
                  const std::string& location);
}
