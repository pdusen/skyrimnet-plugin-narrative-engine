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
}
