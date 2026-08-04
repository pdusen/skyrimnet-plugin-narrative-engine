#include <EvaluationPipeline.h>

#include <AlphaCanon.h>
#include <BeatSystem.h>
#include <CombatEventLog.h>
#include <DashboardUIManager.h>
#include <DecisionLog.h>
#include <LLMTextSanitizer.h>
#include <logger.h>
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

        // Unix-epoch seconds. The canonical DecisionRecord timestamp so
        // dashboard ordering survives save/load (a per-process steady-
        // clock anchor would reset every session).
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

        // Shared by both BuildSnapshot overloads.
        struct EngineSnapshotFields
        {
            PlayerContext player;
            AlphaCanon::Signal alphaCanonMask = AlphaCanon::Signal::None;
        };

        // Off-main-safe: stable-singleton pointer walks + plain
        // accessors, no engine mutation.
        EngineSnapshotFields ReadEngineSnapshotFields()
        {
            EngineSnapshotFields e;

            if (auto* pc = RE::PlayerCharacter::GetSingleton()) {
                e.player.formID = pc->GetFormID();

                if (const char* dn = pc->GetDisplayFullName(); dn && *dn) {
                    e.player.name = dn;
                }

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
                // Seconds-since-epoch matches SkyrimNet's per-event
                // gameTime field; FormatEventsText uses it for "N ago".
                e.player.gameTimeSeconds = static_cast<double>(cal->GetDaysPassed()) * 86400.0;
            }

            e.alphaCanonMask = AlphaCanon::EvaluateAll();
            return e;
        }

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

        void MergeEngineFieldsInto(Snapshot& s, const EngineSnapshotFields& engine)
        {
            s.player.formID = engine.player.formID;
            s.player.name = engine.player.name;
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
        const bool debug = Settings::Get().debugMode;
        if (debug)
            logger::debug("BuildSnapshot: begin (main-thread overload)");

        Snapshot s;
        FillNonEngineSnapshotFields(s);
        MergeEngineFieldsInto(s, ReadEngineSnapshotFields());

        if (debug)
            logger::debug("BuildSnapshot: main-thread overload complete");
        return s;
    }

    Snapshot BuildSnapshot(const PluginThread::Token& pt)
    {
        (void)pt;
        const bool debug = Settings::Get().debugMode;
        if (debug)
            logger::debug("BuildSnapshot: begin (plugin-thread overload)");

        Snapshot s;
        FillNonEngineSnapshotFields(s);
        MergeEngineFieldsInto(s, ReadEngineSnapshotFields());

        if (debug)
            logger::debug("BuildSnapshot: plugin-thread overload complete");
        return s;
    }

    std::string BuildPromptContext(const Snapshot& snapshot)
    {
        nlohmann::json ctx;
        ctx["current_phase"] = snapshot.currentPhase;
        ctx["time_in_phase_seconds"] = snapshot.timeInPhaseSeconds;

        // SkyrimNet returns events newest-first; the prompt template
        // renders them in array order and labels the section "(newest
        // last)", so we reverse to match the label and put the newest
        // events where LLMs attend more.
        {
            auto parsed = nlohmann::json::parse(snapshot.skyrimNetEventsJSON,
                                                /*cb=*/nullptr,
                                                /*allow_exceptions=*/false);
            if (parsed.is_array()) {
                // Drop events from prior phases so idle-time doesn't
                // re-justify the same advance tick after tick.
                //
                // Filter on `evt.localTime` (Unix-epoch real seconds) not
                // `evt.gameTime`: the latter is time-of-day [0..86400)
                // and can't be compared against a cumulative cutoff
                // once a session crosses a day.
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

                // Synthesize `evt.text` per event so the template just
                // renders `{{ evt.text }}`; each line carries a "N ago"
                // relative timestamp from gameTimeSeconds.
                //
                // Book bodies are dropped: the Director scores tension, and
                // what a book said has never moved that needle — but one
                // illustrated book is tens of kilobytes and has blown a
                // request on its own.
                SkyrimNetEvents::FormatEventsText(parsed,
                                                  snapshot.player.gameTimeSeconds,
                                                  SkyrimNetEvents::BookTextPolicy::Omit,
                                                  snapshot.player.name);

                // Drop unrecognized event types (FormatEventsText's
                // "(no data)" last-resort case). Log each so the type
                // surfaces and we can add a renderer if needed.
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

                // Merge in NarrativeEngine's internal event tails
                // (combat, weather, travel). Already phase-pruned in
                // memory; BuildMergedTimeline sorts by localTime and
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
                // Internal event tails still flow through even with no
                // SkyrimNet events.
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

        // Form IDs go out as "0x........" strings to match the rest of
        // the modding tool chain. No game-time field — the template
        // uses SkyrimNet's built-in `{{ gameTime }}` decorator.
        {
            char formIdHex[16];
            std::snprintf(formIdHex, sizeof(formIdHex), "0x%08X", snapshot.player.formID);
            ctx["player_context"] = {
                {"player_form_id", std::string(formIdHex)},
                {"location_name", snapshot.player.locationName},
                {"cell_name", snapshot.player.cellName},
                {"cell_is_interior", snapshot.player.cellIsInterior},
            };
        }

        ctx["alpha_canon_signals"] = snapshot.alphaCanonSignals;

        return ctx.dump();
    }

    DecisionLog::DecisionRecord ParseDecision(const std::string& jsonResponse, const Snapshot& snapshot)
    {
        // Pre-seed from the snapshot so a parse failure still produces
        // a dashboard-displayable record. beatSelected is set later by
        // ConsiderBeat's beat-select callback.
        DecisionLog::DecisionRecord r;
        r.realTimeSec = snapshot.realTimeSec;
        r.gameDaysPassed = snapshot.player.gameDaysPassed;
        r.currentPhase = PhaseTracker::PhaseFromName(snapshot.currentPhase).value_or(PhaseTracker::Phase::Exposition);
        r.alphaCanonActiveSignals = snapshot.alphaCanonSignalBitmask;

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

        // Advancement is system-side: per-phase tension threshold gated
        // by min dwell floor. The dwell floor prevents a borderline
        // score early in a phase from immediately advancing.
        r.advancedToPhase = PhaseTracker::EvaluateAdvance(r.currentPhase, r.tensionScore, snapshot.timeInPhaseSeconds);

        // Sanitize BEFORE the clamp so truncation lands on a well-
        // defined byte boundary (see docs/LLM_RESPONSE_HANDLING.md).
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
        // Append first so the next tick's snapshot sees this decision.
        DecisionLog::Append(record);

        if (record.advancedToPhase) {
            PhaseTracker::AdvanceTo(*record.advancedToPhase);
        }

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
        // Drop this tick if the previous evaluation is still running;
        // the flag releases in ApplyDecision or the failure path.
        bool expected = false;
        if (!g_inFlight.compare_exchange_strong(expected, true)) {
            if (Settings::Get().debugMode) {
                logger::debug("EvaluationPipeline: evaluation already in flight; skipping tick");
            }
            return;
        }

        Snapshot snapshot = BuildSnapshot(pt);
        if (Settings::Get().debugMode) {
            LogSnapshot(snapshot);
        }

        const std::string ctx = BuildPromptContext(snapshot);
        if (Settings::Get().debugMode) {
            logger::debug("BuildPromptContext: produced {}B", ctx.size());
            logger::debug("BuildPromptContext: {}", ctx);
        }

        // The variant "narrative_engine_director" selects an LLM
        // config profile from the NarrativeEngine manifest; without it
        // SkyrimNet falls back to its default Dialogue LLM, which is
        // tuned for creative writing rather than per-tick classification.
        const auto result =
            SkyrimNetAPI::SendCustomPromptToLLM(pt, "narrative_engine_story_eval", "narrative_engine_director", ctx);

        if (Settings::Get().debugMode) {
            logger::debug("LLM callback: success={} body={}B", result.ok, result.response.size());
            if (!result.response.empty()) {
                logger::debug("LLM response: {}", result.response);
            }
        }

        if (!result.ok) {
            logger::warn("EvaluationPipeline: LLM call failed: {}", result.response);
            g_inFlight.store(false);
            return;
        }

        DecisionLog::DecisionRecord rec = ParseDecision(result.response, snapshot);

        // ConsiderBeat takes ownership and is responsible for calling
        // ApplyDecision and the finalizer exactly once.
        BeatSystem::ConsiderBeat(pt, std::move(snapshot), std::move(rec), [] { g_inFlight.store(false); });
    }
} // namespace NarrativeEngine::EvaluationPipeline
