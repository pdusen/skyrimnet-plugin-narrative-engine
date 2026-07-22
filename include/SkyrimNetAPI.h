#pragma once

#include <PluginThread.h>

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

    // Result of the synchronous SendCustomPromptToLLM overload below.
    // On success, `response` holds the LLM's raw response body. On
    // failure, `response` holds the error string SkyrimNet reported
    // (or a wrapper-supplied one for the queue-full case).
    struct LLMResult
    {
        bool ok = false;
        std::string response;
    };

    // Synchronous LLM call. Blocks the plugin thread until SkyrimNet
    // delivers its callback. Same underlying operation as the async
    // overload below; caller sees a linear result instead of a nested
    // callback lambda.
    //
    // Internally uses a foreign-thread callback that pokes a
    // std::promise directly (bypassing AsyncDispatch — routing that
    // step through AsyncDispatch would deadlock the plugin thread
    // that's waiting on the future).
    //
    // Blocking cost: this holds the plugin thread for the full
    // duration of the LLM call (typically several seconds). Only use
    // from single-flighted contexts where the plugin thread has
    // nothing useful to do concurrently — the tick's Director
    // evaluation (guarded by EvaluationPipeline::g_inFlight) is the
    // canonical example. In particular do NOT use from contexts where
    // other plugin-thread work would meaningfully suffer from the
    // stall.
    //
    // Shutdown note: if AsyncDispatch::Stop() is called while a caller
    // is blocked here, shutdown waits for SkyrimNet to deliver its
    // callback (success or failure) before proceeding. Acceptable in
    // practice — SkyrimNet's own shutdown path fires pending callbacks
    // with success=false — but noted so future callers understand the
    // tradeoff.
    LLMResult SendCustomPromptToLLM(const PluginThread::Token&,
                                    const std::string& promptName,
                                    const std::string& variant,
                                    const std::string& contextJson);

    // Queues an async LLM call against the named SkyrimNet prompt template.
    //
    // SkyrimNet fires its callback on one of its own foreign worker
    // threads. This wrapper does NOT invoke the caller-supplied callback
    // there — instead, it captures the response into locals and
    // enqueues the invocation onto the plugin thread via
    // AsyncDispatch::EnqueueWork. By the time `callback` runs, it holds
    // a PluginThread::Token and can freely call MainThread::Run for any
    // engine work.
    //
    // The compile-time proof is worth naming explicitly: the callback's
    // first parameter is a PluginThread::Token, so it is impossible to
    // invoke `callback` from a thread that does not hold one. The only
    // syntactically-legal call site is inside an EnqueueWork job — which
    // is exactly where this wrapper puts it.
    //
    // The const char* response from SkyrimNet is copied into the
    // std::string passed to our callback (the original is only valid for
    // the duration of the SkyrimNet-side call).
    //
    // Returns false if the underlying function pointer is null or the task
    // failed to queue.
    bool SendCustomPromptToLLM(
        const std::string& promptName,
        const std::string& variant,
        const std::string& contextJson,
        std::function<void(const PluginThread::Token&, std::string response, bool success)> callback);

    // Lower-level variant whose callback fires on whatever foreign
    // thread SkyrimNet delivers on — NOT bridged onto the plugin
    // thread via AsyncDispatch::EnqueueWork.
    //
    // Used internally by the synchronous overload above (the promise
    // wait would deadlock if the promise-set went through
    // AsyncDispatch — the plugin thread waiting on the future can't
    // dequeue the very job that would unblock it). Exposed publicly
    // for any future primitive that needs the same "poke a promise /
    // condvar from foreign thread" pattern.
    //
    // The callback body MUST be trivial and thread-agnostic — do NOT
    // call any NarrativeEngine plugin function from it, do NOT touch
    // engine state, do NOT hold locks. The only sanctioned use is
    // poking a std::promise / std::condition_variable that a plugin-
    // thread waiter is watching. Callers that need to run real logic
    // in response to the LLM should use one of the two SendCustomPromptToLLM
    // overloads above.
    //
    // Returns false if the underlying function pointer is null or
    // the task failed to queue. The response C-string is copied
    // into a std::string before being handed to the callback (the
    // original is only valid for the duration of the SkyrimNet-side
    // call).
    bool SendCustomPromptToLLMForeign(const std::string& promptName,
                                      const std::string& variant,
                                      const std::string& contextJson,
                                      std::function<void(std::string response, bool success)> foreignCallback);

    // Returns SkyrimNet's recent-events JSON (an array of event objects with
    // fields: type, text, gameTime, originatingActorName, targetActorName).
    // formId=0 returns the global event stream; an empty filter accepts every
    // event type. Returns "[]" if SkyrimNet is unavailable.
    std::string GetRecentEvents(std::uint32_t formId, int maxCount, const std::string& eventTypeFilter);

    // Registers a custom Inja-template / eligibility-rule decorator with
    // SkyrimNet. The callback must be thread-safe — SkyrimNet calls it from
    // its prompt-rendering thread, which may not be the main thread.
    // Returns false if SkyrimNet is unavailable or registration fails (e.g.
    // name collision with a built-in).
    bool RegisterDecorator(const std::string& name,
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
    // by SkyrimNet's internal scoring. Used by NPCLetterBeat to pick a
    // sender pool. Empty array if SkyrimNet or its memory system isn't
    // ready.
    //
    // maxCount=0 returns every actor with any activity.
    // shortWindow / mediumWindow are in *game* seconds (86400=1 day,
    // 604800=7 days are sensible defaults).
    std::string GetActorEngagement(int maxCount,
                                   bool excludePlayer,
                                   bool playerEventsOnly,
                                   double shortWindowSeconds,
                                   double mediumWindowSeconds);

    // Returns a JSON array of memory entries for the given actor,
    // optionally biased toward entries matching the contextQuery via
    // SkyrimNet's vector search. Empty contextQuery falls back to
    // recency. Returns "[]" if SkyrimNet or its memory system isn't
    // ready.
    std::string GetMemoriesForActor(std::uint32_t formId, int maxCount, const std::string& contextQuery);

    // Returns a JSON array of the most recent dialogue exchanges
    // between the player and the given NPC (chronological, oldest
    // first). Each entry is `{speaker, text, gameTime}`. maxExchanges
    // <= 0 defaults to 10 on the SkyrimNet side. Returns "[]" if
    // SkyrimNet or its memory system isn't ready.
    std::string GetRecentDialogue(std::uint32_t formId, int maxExchanges);

    // Write a memory to SkyrimNet's per-actor store. Returns the
    // memory's id on success, -1 if SkyrimNet / memory system are
    // unavailable. importance is 0.0..1.0; memoryType is one of
    // SkyrimNet's enums ("EXPERIENCE", "OBSERVATION", ...); emotion is
    // a free-form short tag; location is a player-visible cell / loc
    // name (empty for "unknown").
    //
    // tagsJson and relatedActorsJson forward directly to the
    // underlying PublicAddMemory call. Both accept an empty string
    // (interpreted as "no tags" / "no related actors") — the wrapper
    // translates empty → nullptr so SkyrimNet takes its documented
    // null path rather than trying to parse "" as JSON. When non-
    // empty, both MUST be valid JSON: tagsJson a string array
    // (e.g. `["debt","market"]`), relatedActorsJson a number array of
    // FormIDs (e.g. `[20]` for the player).
    int AddMemory(std::uint32_t formId,
                  const std::string& contentText,
                  float importance,
                  const std::string& memoryType,
                  const std::string& emotion,
                  const std::string& location,
                  const std::string& tagsJson = {},
                  const std::string& relatedActorsJson = {});
} // namespace NarrativeEngine::SkyrimNetAPI
