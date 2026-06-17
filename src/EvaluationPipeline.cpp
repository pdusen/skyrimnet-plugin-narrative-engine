#include <EvaluationPipeline.h>

#include <AlphaCanon.h>
#include <AsyncDispatch.h>
#include <DecisionLog.h>
#include <PhaseTracker.h>
#include <Settings.h>
#include <SkyrimNetAPI.h>
#include <logger.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <utility>

namespace NarrativeEngine::EvaluationPipeline
{
    namespace
    {
        std::atomic<bool> g_inFlight = false;

        // Wall-clock anchor for Snapshot::realTimeSec — monotonic seconds
        // since the first BuildSnapshot() call.
        const std::chrono::steady_clock::time_point &StartTime()
        {
            static const auto start = std::chrono::steady_clock::now();
            return start;
        }

        double SecondsSinceStart()
        {
            const auto now = std::chrono::steady_clock::now();
            return std::chrono::duration<double>(now - StartTime()).count();
        }

        // Render the human-readable per-event `text` field that the prompt
        // template expects. SkyrimNet's events all carry a `type` string
        // and a `data` object whose shape varies by type; we collapse each
        // known type to a single readable line. Unknown types fall back to
        // dumping the raw `data` JSON so the LLM at least sees the content.
        //
        // Operates on the event array in place — each object gains a `text`
        // key. Safe against missing fields (treats them as empty strings).
        void FormatEventsText(nlohmann::json &events)
        {
            for (auto &evt : events) {
                if (!evt.is_object()) continue;

                const std::string type = evt.value("type", std::string{});

                const nlohmann::json *data = nullptr;
                if (auto it = evt.find("data"); it != evt.end() && it->is_object()) {
                    data = &(*it);
                }
                const auto str = [&](const char *key) -> std::string {
                    return data ? data->value(key, std::string{}) : std::string{};
                };

                std::string text;
                if (type == "dialogue" || type == "dialogue_background") {
                    const std::string speaker  = str("speaker");
                    const std::string listener = str("listener");
                    const std::string dialogue = str("dialogue");
                    if (!listener.empty()) {
                        text = speaker + " -> " + listener + ": \"" + dialogue + "\"";
                    } else {
                        text = speaker + ": \"" + dialogue + "\"";
                    }
                }
                else if (type == "dialogue_player_text") {
                    const std::string speaker  = str("speaker");
                    const std::string listener = str("listener");
                    const std::string dialogue = str("dialogue");
                    if (!listener.empty()) {
                        text = "(player) " + speaker + " -> " + listener + ": \"" + dialogue + "\"";
                    } else {
                        text = "(player) " + speaker + ": \"" + dialogue + "\"";
                    }
                }
                else if (type == "gamemaster_dialogue") {
                    const std::string speaker  = str("speaker");
                    const std::string target   = str("target");
                    const std::string topic    = str("topic");
                    const std::string dialogue = str("dialogue");
                    text = speaker + " -> " + target + " (topic: " + topic + "): " + dialogue;
                }
                else if (type == "npc_thoughts") {
                    text = str("npc_name") + " (thinking): \"" + str("thoughts") + "\"";
                }
                else if (type == "death") {
                    text = str("killer") + " killed " + str("victim");
                }
                else if (type == "persistent_generic") {
                    text = str("line");
                }
                else {
                    text = data ? data->dump() : std::string{"(no data)"};
                }

                // Prepend the in-game timestamp (SkyrimNet's pre-formatted
                // `gameTimeStr`, e.g. "11:22 AM, Sundas, 31st of Last Seed,
                // 4E 201"). Putting game time on every line lets the LLM
                // reason about pacing without us having to teach it the
                // gameTime float epoch. Falls back to no prefix if missing.
                const std::string timestamp = evt.value("gameTimeStr", std::string{});
                if (!timestamp.empty()) {
                    text = "[" + timestamp + "] " + text;
                }

                evt["text"] = std::move(text);
            }
        }

        std::string JoinCSV(const std::vector<std::string> &parts)
        {
            std::string out;
            for (const auto &s : parts)
            {
                if (!out.empty())
                    out += ',';
                out += s;
            }
            return out;
        }

        // Single multi-line debug dump of a snapshot. Gated on debugMode by
        // the caller.
        void LogSnapshot(const Snapshot &s)
        {
            logger::debug("Snapshot: realTimeSec={:.2f} phase={} timeInPhase={:.2f}s",
                          s.realTimeSec, s.currentPhase, s.timeInPhaseSeconds);
            logger::debug(
                "Snapshot: player formID=0x{:08X} location='{}' (0x{:08X}) cell='{}' (0x{:08X}) interior={} "
                "gameDays={:.3f} hour={:.2f}",
                s.player.formID, s.player.locationName, s.player.locationFormID,
                s.player.cellName, s.player.cellFormID, s.player.cellIsInterior,
                s.player.gameDaysPassed, s.player.timeOfDayHours);
            logger::debug(
                "Snapshot: alphaCanon mask=0x{:08X} signals=[{}] decisionTail={} eventsJSON.size={}B",
                s.alphaCanonSignalBitmask, JoinCSV(s.alphaCanonSignals),
                s.decisionLogTail.size(), s.skyrimNetEventsJSON.size());
        }
    }

    bool IsEvaluationInFlight()
    {
        return g_inFlight.load();
    }

    Snapshot BuildSnapshot()
    {
        // Ensure the static anchor is initialized on the first call. This
        // also implicitly pins the start time to the first tick rather than
        // to plugin load — close enough that the difference doesn't matter.
        (void)StartTime();

        const bool debug = Settings::Get().debugMode;
        if (debug)
            logger::debug("BuildSnapshot: begin");

        Snapshot s;
        s.realTimeSec = SecondsSinceStart();

        s.currentPhase = PhaseTracker::PhaseName(PhaseTracker::Get());
        s.timeInPhaseSeconds = PhaseTracker::TimeInPhaseSeconds();
        if (debug)
            logger::debug("BuildSnapshot: phase OK");

        const int eventTail = std::max(0, Settings::Get().skyrimNetEventTailSizeForPrompt);
        s.skyrimNetEventsJSON = SkyrimNetAPI::GetRecentEvents(0, eventTail, "");
        if (debug)
            logger::debug("BuildSnapshot: events OK ({}B)", s.skyrimNetEventsJSON.size());

        const int decisionTail = std::max(0, Settings::Get().decisionLogTailSizeForPrompt);
        s.decisionLogTail = DecisionLog::Tail(static_cast<std::size_t>(decisionTail));
        if (debug)
            logger::debug("BuildSnapshot: decisionTail OK");

        if (auto *pc = RE::PlayerCharacter::GetSingleton())
        {
            s.player.formID = pc->GetFormID();
            if (debug)
                logger::debug("BuildSnapshot: pc OK");

            if (auto *loc = pc->GetCurrentLocation())
            {
                s.player.locationFormID = loc->GetFormID();
                if (const char *name = loc->GetFullName(); name && *name)
                {
                    s.player.locationName = name;
                }
            }
            if (debug)
                logger::debug("BuildSnapshot: location OK");

            if (auto *cell = pc->GetParentCell())
            {
                s.player.cellFormID = cell->GetFormID();
                s.player.cellIsInterior = cell->IsInteriorCell();
                if (const char *name = cell->GetFullName(); name && *name)
                {
                    s.player.cellName = name;
                }
            }
            if (debug)
                logger::debug("BuildSnapshot: cell OK");
        }

        if (auto *cal = RE::Calendar::GetSingleton())
        {
            s.player.gameDaysPassed = cal->GetDaysPassed();
            s.player.timeOfDayHours = cal->GetHour();
        }
        if (debug)
            logger::debug("BuildSnapshot: calendar OK");

        const auto mask = AlphaCanon::EvaluateAll();
        s.alphaCanonSignals = AlphaCanon::Names(mask);
        s.alphaCanonSignalBitmask = static_cast<std::uint32_t>(mask);
        if (debug)
            logger::debug("BuildSnapshot: alphaCanon OK; returning");

        return s;
    }

    std::string BuildPromptContext(const Snapshot &snapshot)
    {
        nlohmann::json ctx;
        ctx["current_phase"] = snapshot.currentPhase;
        ctx["time_in_phase_seconds"] = snapshot.timeInPhaseSeconds;

        // next_phase: name of the immediate cyclical successor. The loop
        // wraps — NextPhase(Resolution) = Exposition — so this is always a
        // valid phase name; there is no terminal case. The prompt template
        // uses this to present the LLM with a binary stay/advance choice
        // rather than the full five-phase enumeration.
        if (auto cur = PhaseTracker::PhaseFromName(snapshot.currentPhase); cur)
        {
            ctx["next_phase"] = PhaseTracker::PhaseName(PhaseTracker::NextPhase(*cur));
        }
        else
        {
            // Defensive fallback for a malformed snapshot.
            ctx["next_phase"] = PhaseTracker::PhaseName(PhaseTracker::Phase::Exposition);
        }

        // recent_events: pass SkyrimNet's event array through, but REVERSE
        // it. SkyrimNet returns events newest-first; the prompt template
        // renders them in array order and labels the section "(newest last)".
        // Reversing here makes the data order match the label, and puts the
        // most recent events at the end of the rendered list — where LLMs
        // typically attend more. On parse failure or non-array, fall back to
        // an empty array and warn.
        {
            auto parsed = nlohmann::json::parse(
                snapshot.skyrimNetEventsJSON, /*cb=*/nullptr,
                /*allow_exceptions=*/false);
            if (parsed.is_array())
            {
                std::reverse(parsed.begin(), parsed.end());

                // SkyrimNet events have a `type` discriminator and an
                // arbitrary `data` payload that varies by type. The prompt
                // template can't reasonably branch on every shape, so we
                // synthesize a human-readable `evt.text` here. The template
                // then just renders `{{ evt.text }}` per event.
                FormatEventsText(parsed);

                ctx["recent_events"] = std::move(parsed);
            }
            else
            {
                if (parsed.is_discarded() && !snapshot.skyrimNetEventsJSON.empty())
                {
                    logger::warn("BuildPromptContext: recent_events JSON failed to parse; using []");
                }
                else if (!parsed.is_discarded())
                {
                    logger::warn("BuildPromptContext: recent_events JSON wasn't an array; using []");
                }
                ctx["recent_events"] = nlohmann::json::array();
            }
        }

        // decision_log_tail: oldest-first per Tail() semantics.
        {
            nlohmann::json tail = nlohmann::json::array();
            for (const auto &r : snapshot.decisionLogTail)
            {
                nlohmann::json entry = {
                    {"t", r.realTimeSec},
                    {"tension_score", r.tensionScore},
                    {"phase", PhaseTracker::PhaseName(r.currentPhase)},
                    {"action", r.actionSelected},
                    {"narrative_note", r.narrativeNote},
                };
                if (r.advancedToPhase)
                {
                    entry["advanced_to"] = PhaseTracker::PhaseName(*r.advancedToPhase);
                }
                tail.push_back(std::move(entry));
            }
            ctx["decision_log_tail"] = std::move(tail);
        }

        // player_context. Form IDs go out as "0x........" strings — the
        // LLM treats them as opaque identifiers, and hex matches how the
        // rest of the modding tool chain renders them.
        {
            char formIdHex[16];
            std::snprintf(formIdHex, sizeof(formIdHex), "0x%08X", snapshot.player.formID);
            ctx["player_context"] = {
                {"player_form_id", std::string(formIdHex)},
                {"location_name", snapshot.player.locationName},
                {"cell_name", snapshot.player.cellName},
                {"cell_is_interior", snapshot.player.cellIsInterior},
                {"game_days_passed", snapshot.player.gameDaysPassed},
                {"time_of_day_hours", snapshot.player.timeOfDayHours},
            };
        }

        ctx["alpha_canon_signals"] = snapshot.alphaCanonSignals;

        return ctx.dump();
    }

    DecisionLog::DecisionRecord ParseDecision(const std::string & /*jsonResponse*/,
                                              const Snapshot &snapshot)
    {
        // Step 14 — Phase D parser. For now, return a snapshot-seeded
        // record so callers (when wired) get something sensible.
        DecisionLog::DecisionRecord r;
        r.realTimeSec = snapshot.realTimeSec;
        r.gameDaysPassed = snapshot.player.gameDaysPassed;
        r.currentPhase = PhaseTracker::Get();
        r.alphaCanonActiveSignals = snapshot.alphaCanonSignalBitmask;
        return r;
    }

    void ApplyDecision(const DecisionLog::DecisionRecord & /*record*/)
    {
        // Step 14 — Phase D applier. Not yet implemented.
    }

    void BeginEvaluation()
    {
        // Atomic guard: only one evaluation may be in flight at a time. If
        // a previous one is still being processed when the next tick fires
        // (LLM slow, worker backed up), this tick is silently dropped.
        // The flag is released only after ApplyDecision (or an early-exit
        // failure path) runs on the main thread, so two ticks can't see
        // overlapping in-flight state.
        bool expected = false;
        if (!g_inFlight.compare_exchange_strong(expected, true))
        {
            if (Settings::Get().debugMode)
            {
                logger::debug("EvaluationPipeline: evaluation already in flight; skipping tick");
            }
            return;
        }

        Snapshot snapshot = BuildSnapshot();
        if (Settings::Get().debugMode)
        {
            LogSnapshot(snapshot);
        }

        // Phase B + C run on the worker thread. The snapshot is moved in
        // so the worker carries it forward into the LLM callback (Phase D
        // needs it for ParseDecision's pre-fill).
        AsyncDispatch::EnqueueWork([snapshot = std::move(snapshot)]() mutable
                                   {
            const std::string ctx = BuildPromptContext(snapshot);
            if (Settings::Get().debugMode) {
                logger::debug("BuildPromptContext: produced {}B", ctx.size());
                logger::debug("BuildPromptContext: {}", ctx);
            }

            // Phase C — fire the LLM call. The callback fires on a
            // SkyrimNet worker thread (NOT the main thread); it must
            // marshal back to the main thread before touching engine state.
            const bool queued = SkyrimNetAPI::SendCustomPromptToLLM(
                // 2nd arg is the *variant* — a named LLM-config profile
                // declared in statics/.../SkyrimNet/config/plugins/
                // NarrativeEngine/manifest.yaml. Without a variant the call
                // falls back to SkyrimNet's default Dialogue LLM, which is
                // tuned for creative writing, not per-tick classification.
                "narrative_engine_story_eval", "narrative_engine_director", ctx,
                [snapshot = std::move(snapshot)](std::string response, bool success) mutable {
                    if (Settings::Get().debugMode) {
                        logger::debug("LLM callback: success={} body={}B", success, response.size());
                        if (!response.empty()) {
                            logger::debug("LLM response: {}", response);
                        }
                    }

                    if (!success) {
                        // SkyrimNet's failure path puts an error string in
                        // `response`. Log + release inFlight on the main
                        // thread so the next tick can run.
                        logger::warn("EvaluationPipeline: LLM call failed: {}", response);
                        AsyncDispatch::MarshalToMainThread([] {
                            g_inFlight.store(false);
                        });
                        return;
                    }

                    // Phase D parser runs on this (SkyrimNet) thread — it
                    // touches no engine state. Phase D applier marshals
                    // back to the main thread.
                    DecisionLog::DecisionRecord rec = ParseDecision(response, snapshot);
                    AsyncDispatch::MarshalToMainThread([rec = std::move(rec)] {
                        ApplyDecision(rec);
                        g_inFlight.store(false);
                    });
                });

            if (!queued) {
                logger::warn(
                    "EvaluationPipeline: SendCustomPromptToLLM returned false; dropping tick's evaluation");
                AsyncDispatch::MarshalToMainThread([] {
                    g_inFlight.store(false);
                });
            } });
    }
}
