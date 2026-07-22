#include <EvaluationPipeline.h>

#include <AlphaCanon.h>
#include <BeatSystem.h>
#include <CombatEventLog.h>
#include <DashboardUIManager.h>
#include <DecisionLog.h>
#include <LLMTextSanitizer.h>
#include <logger.h>
#include <MainThread.h>
#include <PhaseTracker.h>
#include <Settings.h>
#include <SkyrimNetAPI.h>
#include <SkyrimNetEvents.h>
#include <TravelEventLog.h>
#include <WeatherEventLog.h>

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

        // Unix-epoch seconds. Used as the canonical timestamp on every
        // DecisionRecord so the dashboard can order entries correctly
        // across save/load boundaries. (A per-process steady-clock anchor
        // resets every session, which made loaded records from a prior
        // session bury a fresh decision whose anchor was just initialized.)
        double NowUnixSeconds()
        {
            return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
        }

        std::string JoinCSV(const std::vector<std::string>& parts)
        {
            std::string out;
            for (const auto& s : parts) {
                if (!out.empty())
                    out += ',';
                out += s;
            }
            return out;
        }

        // Engine-derived fields the snapshot needs from the main
        // thread. Split out so both BuildSnapshot overloads (main-
        // thread caller and plugin-thread caller) can share one
        // implementation of the actual engine reads.
        struct EngineSnapshotFields
        {
            PlayerContext player; // engine-derived subset of PlayerContext
            AlphaCanon::Signal alphaCanonMask = AlphaCanon::Signal::None;
        };

        // Read every engine-touching field the snapshot needs.
        // Callers must be on the main thread. The plugin-thread
        // BuildSnapshot overload calls this via MainThread::Run; the
        // main-thread overload calls it directly.
        EngineSnapshotFields ReadEngineSnapshotFieldsOnMain()
        {
            EngineSnapshotFields e;

            if (auto* pc = RE::PlayerCharacter::GetSingleton()) {
                e.player.formID = pc->GetFormID();

                if (auto* loc = pc->GetCurrentLocation()) {
                    e.player.locationFormID = loc->GetFormID();
                    if (const char* name = loc->GetFullName(); name && *name) {
                        e.player.locationName = name;
                    }
                }

                if (auto* cell = pc->GetParentCell()) {
                    e.player.cellFormID = cell->GetFormID();
                    e.player.cellIsInterior = cell->IsInteriorCell();
                    if (const char* name = cell->GetFullName(); name && *name) {
                        e.player.cellName = name;
                    }
                }
            }

            if (auto* cal = RE::Calendar::GetSingleton()) {
                e.player.gameDaysPassed = cal->GetDaysPassed();
                e.player.timeOfDayHours = cal->GetHour();
                // Convert days-since-epoch → seconds-since-epoch to
                // match SkyrimNet's per-event gameTime field units.
                // Used by FormatEventsText for "N units ago" relative
                // timestamps.
                e.player.gameTimeSeconds = static_cast<double>(cal->GetDaysPassed()) * 86400.0;
            }

            e.alphaCanonMask = AlphaCanon::EvaluateAll();
            return e;
        }

        // Non-engine snapshot fields — the pieces that are thread-safe
        // to read from either main or plugin thread. Shared between
        // the two BuildSnapshot overloads.
        void FillNonEngineSnapshotFields(Snapshot& s)
        {
            s.realTimeSec = NowUnixSeconds();

            s.currentPhase = PhaseTracker::PhaseName(PhaseTracker::Get());
            s.timeInPhaseSeconds = PhaseTracker::TimeInPhaseSeconds();
            s.phaseEnteredAtRealTime = PhaseTracker::PhaseEnteredAtRealTime();

            const int eventTail = std::max(0, Settings::Get().skyrimNetEventTailSizeForPrompt);
            s.skyrimNetEventsJSON = SkyrimNetAPI::GetRecentEvents(0, eventTail, "");

            const int decisionTail = std::max(0, Settings::Get().decisionLogTailSizeForPrompt);
            s.decisionLogTail = DecisionLog::Tail(static_cast<std::size_t>(decisionTail));
        }

        // Merge the engine-derived fields into the target snapshot.
        // Called after FillNonEngineSnapshotFields.
        void MergeEngineFieldsInto(Snapshot& s, const EngineSnapshotFields& engine)
        {
            s.player.formID = engine.player.formID;
            s.player.locationFormID = engine.player.locationFormID;
            s.player.locationName = engine.player.locationName;
            s.player.cellFormID = engine.player.cellFormID;
            s.player.cellName = engine.player.cellName;
            s.player.cellIsInterior = engine.player.cellIsInterior;
            s.player.gameDaysPassed = engine.player.gameDaysPassed;
            s.player.timeOfDayHours = engine.player.timeOfDayHours;
            s.player.gameTimeSeconds = engine.player.gameTimeSeconds;

            s.alphaCanonSignals = AlphaCanon::Names(engine.alphaCanonMask);
            s.alphaCanonSignalBitmask = static_cast<std::uint32_t>(engine.alphaCanonMask);
        }

        // Single multi-line debug dump of a snapshot. Gated on debugMode by
        // the caller.
        void LogSnapshot(const Snapshot& s)
        {
            logger::debug("Snapshot: realTimeSec={:.2f} phase={} timeInPhase={:.2f}s",
                          s.realTimeSec,
                          s.currentPhase,
                          s.timeInPhaseSeconds);
            logger::debug("Snapshot: player formID=0x{:08X} location='{}' (0x{:08X}) cell='{}' (0x{:08X}) interior={} "
                          "gameDays={:.3f} hour={:.2f}",
                          s.player.formID,
                          s.player.locationName,
                          s.player.locationFormID,
                          s.player.cellName,
                          s.player.cellFormID,
                          s.player.cellIsInterior,
                          s.player.gameDaysPassed,
                          s.player.timeOfDayHours);
            logger::debug("Snapshot: alphaCanon mask=0x{:08X} signals=[{}] decisionTail={} eventsJSON.size={}B",
                          s.alphaCanonSignalBitmask,
                          JoinCSV(s.alphaCanonSignals),
                          s.decisionLogTail.size(),
                          s.skyrimNetEventsJSON.size());
        }
    } // namespace

    bool IsEvaluationInFlight()
    {
        return g_inFlight.load();
    }

    std::string StripMarkdownFences(const std::string& input)
    {
        // Trim leading/trailing whitespace. If the result begins with ```
        // (optionally followed by a language tag and newline), skip past
        // that opening fence and strip the closing fence too.
        auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
        std::size_t start = 0;
        std::size_t end = input.size();
        while (start < end && isSpace(input[start]))
            ++start;
        while (end > start && isSpace(input[end - 1]))
            --end;
        if (start == end) {
            return {};
        }

        std::string trimmed = input.substr(start, end - start);
        if (trimmed.size() < 6 || trimmed.substr(0, 3) != "```") {
            return trimmed;
        }

        const std::size_t firstNewline = trimmed.find('\n');
        if (firstNewline == std::string::npos) {
            return trimmed;
        }
        std::string body = trimmed.substr(firstNewline + 1);

        const std::size_t closing = body.rfind("```");
        if (closing != std::string::npos) {
            body = body.substr(0, closing);
        }

        std::size_t bodyEnd = body.size();
        while (bodyEnd > 0 && isSpace(body[bodyEnd - 1]))
            --bodyEnd;
        body.resize(bodyEnd);
        return body;
    }

    Snapshot BuildSnapshot()
    {
        // Main-thread overload — used by BeatSystem::StartBeat and
        // ForceDispatchBeat, which run on the main thread. Reads
        // every field inline, no marshaling.
        const bool debug = Settings::Get().debugMode;
        if (debug)
            logger::debug("BuildSnapshot: begin (main-thread overload)");

        Snapshot s;
        FillNonEngineSnapshotFields(s);
        MergeEngineFieldsInto(s, ReadEngineSnapshotFieldsOnMain());

        if (debug)
            logger::debug("BuildSnapshot: main-thread overload complete");
        return s;
    }

    Snapshot BuildSnapshot(const PluginThread::Token& pt)
    {
        // Plugin-thread overload — used by BeginEvaluation. The non-
        // engine reads (PhaseTracker, DecisionLog, SkyrimNetAPI DLL
        // fetch) run inline on the plugin thread since they're all
        // thread-safe; the engine reads (player + calendar + AlphaCanon)
        // bundle into a single MainThread::Run hop.
        const bool debug = Settings::Get().debugMode;
        if (debug)
            logger::debug("BuildSnapshot: begin (plugin-thread overload)");

        Snapshot s;
        FillNonEngineSnapshotFields(s);

        const auto engine =
            MainThread::Run(pt, [](const MainThread::Token&) { return ReadEngineSnapshotFieldsOnMain(); });
        MergeEngineFieldsInto(s, engine);

        if (debug)
            logger::debug("BuildSnapshot: plugin-thread overload complete");
        return s;
    }

    std::string BuildPromptContext(const Snapshot& snapshot)
    {
        nlohmann::json ctx;
        ctx["current_phase"] = snapshot.currentPhase;
        ctx["time_in_phase_seconds"] = snapshot.timeInPhaseSeconds;
        // No `next_phase` in the prompt context — phase advancement is no
        // longer the LLM's call. The system applies per-phase thresholds
        // (PhaseTracker::EvaluateAdvance) to the returned tension score.

        // recent_events: pass SkyrimNet's event array through, but REVERSE
        // it. SkyrimNet returns events newest-first; the prompt template
        // renders them in array order and labels the section "(newest last)".
        // Reversing here makes the data order match the label, and puts the
        // most recent events at the end of the rendered list — where LLMs
        // typically attend more. On parse failure or non-array, fall back to
        // an empty array and warn.
        {
            auto parsed = nlohmann::json::parse(snapshot.skyrimNetEventsJSON,
                                                /*cb=*/nullptr,
                                                /*allow_exceptions=*/false);
            if (parsed.is_array()) {
                // Filter to events that occurred during the current phase.
                // Events from prior phases have already been "consumed" by
                // whichever past decision drove the previous advance — if
                // we leave them in, the LLM keeps re-justifying advances
                // against the same set of events tick after tick, walking
                // the phase cycle on idle time alone.
                //
                // Filter on `evt.localTime` (Unix-epoch real seconds), not
                // `evt.gameTime`: SkyrimNet's gameTime field is time-of-day
                // in seconds [0..86400), which can't be compared against a
                // cumulative cutoff once a session crosses a day. localTime
                // is monotonic real seconds, immune to that and to the
                // stale-Calendar race we hit at kNewGame (the engine's
                // GetDaysPassed can briefly return the previous session's
                // value before the new world finishes initializing).
                const double cutoff = snapshot.phaseEnteredAtRealTime;
                if (cutoff > 0.0) {
                    nlohmann::json filtered = nlohmann::json::array();
                    for (auto& evt : parsed) {
                        if (!evt.is_object())
                            continue;
                        const double et = evt.value("localTime", 0.0);
                        if (et >= cutoff) {
                            filtered.push_back(std::move(evt));
                        }
                    }
                    parsed = std::move(filtered);
                }

                std::reverse(parsed.begin(), parsed.end());

                // SkyrimNet events have a `type` discriminator and an
                // arbitrary `data` payload that varies by type. The prompt
                // template can't reasonably branch on every shape, so we
                // synthesize a human-readable `evt.text` here. The template
                // then just renders `{{ evt.text }}` per event. Passing the
                // snapshot's current game time lets each event line carry a
                // "N units ago" relative timestamp.
                SkyrimNetEvents::FormatEventsText(parsed, snapshot.player.gameTimeSeconds);

                // Drop events with no usable text (FormatEventsText's
                // "(no data)" last-resort case — typically third-party
                // event types whose `data` field is missing entirely).
                // Sending them to the LLM is pure noise. We log each
                // dropped event so the unrecognized `type` surfaces in
                // logs and we can add a renderer for it if it turns out
                // to be narratively meaningful.
                {
                    static constexpr std::string_view kNoData = "(no data)";
                    nlohmann::json kept = nlohmann::json::array();
                    for (auto& evt : parsed) {
                        if (!evt.is_object())
                            continue;
                        auto it = evt.find("text");
                        if (it != evt.end() && it->is_string()) {
                            const auto& s = it->get_ref<const std::string&>();
                            if (s.size() >= kNoData.size()
                                && s.compare(s.size() - kNoData.size(), kNoData.size(), kNoData) == 0) {
                                logger::warn("BuildPromptContext: dropping event with no usable text: {}", evt.dump());
                                continue;
                            }
                        }
                        kept.push_back(std::move(evt));
                    }
                    parsed = std::move(kept);
                }

                // Merge in NarrativeEngine's internal event tails (combat
                // from Phase 02, weather from Phase 09). Both tails are
                // already phase-pruned in memory, so no extra filter pass
                // is needed. BuildMergedTimeline sorts by localTime and
                // condenses runs of hit events.
                ctx["recent_events"] = SkyrimNetEvents::BuildMergedTimeline(
                    std::move(parsed),
                    CombatEventLog::GetRenderedTail(snapshot.player.gameTimeSeconds),
                    WeatherEventLog::GetRenderedTail(snapshot.player.gameTimeSeconds),
                    TravelEventLog::GetRenderedTail(snapshot.player.gameTimeSeconds),
                    snapshot.player.gameTimeSeconds);
            } else {
                if (parsed.is_discarded() && !snapshot.skyrimNetEventsJSON.empty()) {
                    logger::warn("BuildPromptContext: recent_events JSON failed to parse; using internal-only tail");
                } else if (!parsed.is_discarded()) {
                    logger::warn("BuildPromptContext: recent_events JSON wasn't an array; using internal-only tail");
                }
                // Even with no SkyrimNet events, we still want our
                // internal event tails to reach the prompt.
                ctx["recent_events"] = SkyrimNetEvents::BuildMergedTimeline(
                    nlohmann::json::array(),
                    CombatEventLog::GetRenderedTail(snapshot.player.gameTimeSeconds),
                    WeatherEventLog::GetRenderedTail(snapshot.player.gameTimeSeconds),
                    TravelEventLog::GetRenderedTail(snapshot.player.gameTimeSeconds),
                    snapshot.player.gameTimeSeconds);
            }
        }

        // decision_log_tail: oldest-first per Tail() semantics.
        {
            nlohmann::json tail = nlohmann::json::array();
            for (const auto& r : snapshot.decisionLogTail) {
                nlohmann::json entry = {
                    {"t", r.realTimeSec},
                    {"tension_score", r.tensionScore},
                    {"phase", PhaseTracker::PhaseName(r.currentPhase)},
                    {"action", r.beatSelected},
                    {"narrative_note", r.narrativeNote},
                };
                if (r.advancedToPhase) {
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
                // No game-time field — the prompt template renders the
                // current time via SkyrimNet's built-in `{{ gameTime }}`
                // decorator, so we don't need to push it through the
                // context JSON ourselves.
            };
        }

        ctx["alpha_canon_signals"] = snapshot.alphaCanonSignals;

        return ctx.dump();
    }

    DecisionLog::DecisionRecord ParseDecision(const std::string& jsonResponse, const Snapshot& snapshot)
    {
        // Pre-seed the record from the snapshot so even a total parse
        // failure produces a valid, dashboard-displayable record (we still
        // know the time, phase, and alpha-canon snapshot regardless of
        // what the LLM said).
        DecisionLog::DecisionRecord r;
        r.realTimeSec = snapshot.realTimeSec;
        r.gameDaysPassed = snapshot.player.gameDaysPassed;
        r.currentPhase = PhaseTracker::PhaseFromName(snapshot.currentPhase).value_or(PhaseTracker::Phase::Exposition);
        r.alphaCanonActiveSignals = snapshot.alphaCanonSignalBitmask;
        // beatSelected stays empty at this point — the Director's
        // beat-select LLM callback populates it later in ConsiderBeat.

        const std::string body = StripMarkdownFences(jsonResponse);
        const auto parsed = nlohmann::json::parse(body,
                                                  /*cb=*/nullptr,
                                                  /*allow_exceptions=*/false);
        if (parsed.is_discarded()) {
            r.narrativeNote = "parse_failure: invalid JSON";
            logger::warn("ParseDecision: invalid JSON response: {}", jsonResponse);
            return r;
        }
        if (!parsed.is_object()) {
            r.narrativeNote = "parse_failure: response was not a JSON object";
            logger::warn("ParseDecision: response wasn't an object: {}", jsonResponse);
            return r;
        }

        // tension_score — accept any number, clamp to 0..100.
        if (auto it = parsed.find("tension_score"); it != parsed.end() && it->is_number()) {
            const double raw = it->get<double>();
            const int clamped = std::clamp(static_cast<int>(raw), 0, 100);
            r.tensionScore = static_cast<std::uint32_t>(clamped);
        }

        // System-side phase advancement: compare the LLM's tension score
        // against the per-current-phase threshold, gated by the minimum
        // dwell floor. The LLM no longer votes on this directly — it just
        // scores tension, and the thresholds capture the dramatic shape of
        // each transition (rises into Exposition/Climax, drops out of
        // Climax/FallingAction). The dwell floor prevents a single
        // borderline tension score early in a phase from immediately
        // advancing on the next tick.
        r.advancedToPhase = PhaseTracker::EvaluateAdvance(r.currentPhase, r.tensionScore, snapshot.timeInPhaseSeconds);

        // narrative_note — sanitize LLM-returned Unicode noise (smart quotes,
        // em-dashes, ellipsis, NBSPs, etc.) per docs/LLM_RESPONSE_HANDLING.md,
        // then clamp to 200 chars. Sanitization happens BEFORE the clamp so
        // the truncation lands on a well-defined byte boundary.
        if (auto it = parsed.find("narrative_note"); it != parsed.end() && it->is_string()) {
            std::string note = LLMTextSanitizer::Sanitize(it->get<std::string>());
            if (note.size() > 200) {
                note.resize(200);
            }
            r.narrativeNote = std::move(note);
        }

        return r;
    }

    void ApplyDecision(const PluginThread::Token&, const DecisionLog::DecisionRecord& record)
    {
        // Append first so the next tick's snapshot sees this decision in
        // its `decisionLogTail`. The `ne_narrative_tension` decorator
        // (Step 12) will read this record's tensionScore on the very next
        // NPC bio render. DecisionLog::Append is mutex-guarded — safe
        // from the plugin thread.
        DecisionLog::Append(record);

        // Phase advance, if any. AdvanceTo zeroes time-in-phase and
        // notifies event logs; all of that is mutex-guarded. The
        // `ne_narrative_phase` decorator will see the new phase on the
        // next bio render.
        if (record.advancedToPhase) {
            PhaseTracker::AdvanceTo(*record.advancedToPhase);
        }

        // Push the fresh DirectorState to the PrismaUI dashboard view.
        // PushFullState is safe from any thread — it early-outs when
        // hidden and moves the compose off the caller's thread via
        // AsyncDispatch, so we pay no compose cost here.
        DashboardUIManager::PushFullState();

        if (Settings::Get().debugMode) {
            logger::debug("ApplyDecision: tension={} advance={} note=\"{}\"",
                          record.tensionScore,
                          record.advancedToPhase ? PhaseTracker::PhaseName(*record.advancedToPhase) : "(no)",
                          record.narrativeNote);
        }
    }

    void BeginEvaluation(const PluginThread::Token& pt)
    {
        // Atomic guard: only one evaluation may be in flight at a time.
        // If a previous one is still being processed when the next tick
        // fires (LLM slow, worker backed up), this tick is silently
        // dropped. The flag is released only after ApplyDecision (or an
        // early-exit failure path) runs, so two ticks can't see
        // overlapping in-flight state.
        bool expected = false;
        if (!g_inFlight.compare_exchange_strong(expected, true)) {
            if (Settings::Get().debugMode) {
                logger::debug("EvaluationPipeline: evaluation already in flight; skipping tick");
            }
            return;
        }

        // Phase A — snapshot on plugin thread, with a single main-
        // thread hop for the engine reads.
        Snapshot snapshot = BuildSnapshot(pt);
        if (Settings::Get().debugMode) {
            LogSnapshot(snapshot);
        }

        // Phase B — prompt context assembly. Pure JSON work on the
        // plugin thread; no engine touches.
        const std::string ctx = BuildPromptContext(snapshot);
        if (Settings::Get().debugMode) {
            logger::debug("BuildPromptContext: produced {}B", ctx.size());
            logger::debug("BuildPromptContext: {}", ctx);
        }

        // Phase C — fire the LLM call synchronously. Blocks the
        // plugin thread until SkyrimNet delivers its callback. See
        // SkyrimNetAPI.h for the rationale on sync-blocking:
        // BeginEvaluation is single-flighted via g_inFlight, so
        // nothing useful runs on the plugin thread concurrently that
        // would suffer from the wait.
        //
        // 2nd arg is the *variant* — a named LLM-config profile
        // declared in statics/.../SkyrimNet/config/plugins/
        // NarrativeEngine/manifest.yaml. Without a variant the call
        // falls back to SkyrimNet's default Dialogue LLM, which is
        // tuned for creative writing, not per-tick classification.
        const auto result =
            SkyrimNetAPI::SendCustomPromptToLLM(pt, "narrative_engine_story_eval", "narrative_engine_director", ctx);

        if (Settings::Get().debugMode) {
            logger::debug("LLM callback: success={} body={}B", result.ok, result.response.size());
            if (!result.response.empty()) {
                logger::debug("LLM response: {}", result.response);
            }
        }

        if (!result.ok) {
            // SkyrimNet's failure path (or the sync wrapper's queue-
            // full path) puts an error string in `result.response`.
            logger::warn("EvaluationPipeline: LLM call failed: {}", result.response);
            g_inFlight.store(false);
            return;
        }

        // Phase D parser — plugin thread, touches no engine state.
        DecisionLog::DecisionRecord rec = ParseDecision(result.response, snapshot);

        // Phase D applier hand-off. ConsiderBeat now runs on the
        // plugin thread — its own gate walk, LLM round-trip, and JSON
        // parse are all thread-safe; only the finalize step (which
        // calls beat->OnStart per IBeat's main-thread contract) hops
        // back to main via MainThread::FireAndForget. ConsiderBeat
        // takes ownership of snapshot + rec and is responsible for
        // calling ApplyDecision and the finalizer exactly once.
        BeatSystem::ConsiderBeat(pt, std::move(snapshot), std::move(rec), [] { g_inFlight.store(false); });
    }
} // namespace NarrativeEngine::EvaluationPipeline
