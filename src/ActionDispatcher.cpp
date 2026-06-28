#include <ActionDispatcher.h>

#include <ActionRegistry.h>
#include <AlphaCanon.h>
#include <AsyncDispatch.h>
#include <CombatEventLog.h>
#include <DashboardUIManager.h>
#include <DecisionLog.h>
#include <EvaluationPipeline.h>
#include <LLMTextSanitizer.h>
#include <PhaseTracker.h>
#include <Settings.h>
#include <SkyrimNetAPI.h>
#include <SkyrimNetEvents.h>
#include <Snapshot.h>
#include <logger.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

namespace NarrativeEngine::ActionDispatcher
{
    namespace
    {
        constexpr std::uint32_t kRecordVersion = 1;

        // Anti-repetition ring buffer cap. Small; only big enough to hold
        // entries that haven't aged past iActionRepetitionWindowSeconds.
        constexpr std::size_t kRecentlyFiredCap = 32;

        // -----------------------------------------------------------------
        // State
        // -----------------------------------------------------------------

        std::mutex  g_mutex;

        std::string g_actionInFlight;
        double      g_actionStartedAt       = 0.0;  // Unix-epoch seconds
        double      g_lastActionCompletedAt = 0.0;  // Unix-epoch seconds

        struct RecentlyFired
        {
            std::string name;
            double      completedAt = 0.0;
        };
        std::deque<RecentlyFired> g_recentlyFired;

        // Sink-registration idempotency.
        bool g_sinkRegistered = false;

        // -----------------------------------------------------------------
        // Helpers
        // -----------------------------------------------------------------

        double NowUnixSeconds()
        {
            return std::chrono::duration<double>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        }

        int IdealDurationFor(PhaseTracker::Phase p)
        {
            const auto& cfg = Settings::Get();
            switch (p) {
                case PhaseTracker::Phase::Exposition:    return cfg.idealDurationExposition;
                case PhaseTracker::Phase::RisingAction:  return cfg.idealDurationRisingAction;
                case PhaseTracker::Phase::Climax:        return cfg.idealDurationClimax;
                case PhaseTracker::Phase::FallingAction: return cfg.idealDurationFallingAction;
                case PhaseTracker::Phase::Resolution:    return cfg.idealDurationResolution;
                default:                                  return 0;
            }
        }

        // Drop entries older than iActionRepetitionWindowSeconds from the
        // front of the deque. Caller must hold g_mutex.
        void TrimRecentlyFiredLocked(double now)
        {
            const double window = static_cast<double>(
                Settings::Get().actionRepetitionWindowSeconds);
            while (!g_recentlyFired.empty() &&
                   (now - g_recentlyFired.front().completedAt) > window) {
                g_recentlyFired.pop_front();
            }
            // Cap protection — should never hit in practice.
            while (g_recentlyFired.size() > kRecentlyFiredCap) {
                g_recentlyFired.pop_front();
            }
        }

        bool WasFiredRecentlyLocked(const std::string& name, double now)
        {
            const double window = static_cast<double>(
                Settings::Get().actionRepetitionWindowSeconds);
            for (const auto& r : g_recentlyFired) {
                if (r.name == name && (now - r.completedAt) <= window) {
                    return true;
                }
            }
            return false;
        }

        // -----------------------------------------------------------------
        // ModEvent sink
        // -----------------------------------------------------------------
        //
        // CompleteAction (public, defined below) owns the shared body;
        // this sink just unwraps the BSFixedString and marshals.

        struct CompletionSink : public RE::BSTEventSink<SKSE::ModCallbackEvent>
        {
            RE::BSEventNotifyControl ProcessEvent(
                const SKSE::ModCallbackEvent*                a_event,
                RE::BSTEventSource<SKSE::ModCallbackEvent>*  /*a_source*/) override
            {
                if (!a_event) return RE::BSEventNotifyControl::kContinue;
                if (a_event->eventName != "_ne_ActionCompleted") {
                    return RE::BSEventNotifyControl::kContinue;
                }
                // Copy out of BSFixedString into std::string for the marshal.
                std::string actionName{a_event->strArg.c_str()};
                AsyncDispatch::MarshalToMainThread(
                    [actionName = std::move(actionName)]() mutable {
                        CompleteAction(actionName);
                    });
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        CompletionSink* GetCompletionSink()
        {
            static CompletionSink instance;
            return &instance;
        }

        // -----------------------------------------------------------------
        // Co-save string helpers (same shape as CombatEventLog's)
        // -----------------------------------------------------------------

        void WriteString(SKSE::SerializationInterface* intfc, const std::string& s)
        {
            const auto len = static_cast<std::uint32_t>(s.size());
            intfc->WriteRecordData(len);
            if (len > 0) intfc->WriteRecordData(s.data(), len);
        }

        bool ReadString(SKSE::SerializationInterface* intfc, std::string& out)
        {
            std::uint32_t len = 0;
            if (intfc->ReadRecordData(len) != sizeof(len)) return false;
            out.resize(len);
            if (len > 0 && intfc->ReadRecordData(out.data(), len) != len) return false;
            return true;
        }
    }

    // ---------------------------------------------------------------------
    // Public API
    // ---------------------------------------------------------------------

    void Initialize()
    {
        if (g_sinkRegistered) {
            return;
        }
        if (auto* src = SKSE::GetModCallbackEventSource()) {
            src->AddEventSink<SKSE::ModCallbackEvent>(GetCompletionSink());
            g_sinkRegistered = true;
            logger::info("ActionDispatcher: completion ModEvent sink registered");
        } else {
            logger::error("ActionDispatcher: SKSE::GetModCallbackEventSource() returned null; completion sink NOT registered");
        }
    }

    void CompleteAction(std::string_view actionName)
    {
        const double now = NowUnixSeconds();

        {
            std::scoped_lock lock(g_mutex);
            if (g_actionInFlight.empty()) {
                logger::warn(
                    "ActionDispatcher::CompleteAction: action='{}' but no action is in flight; ignoring",
                    actionName);
                return;
            }
            if (g_actionInFlight != actionName) {
                logger::warn(
                    "ActionDispatcher::CompleteAction: action='{}' but in-flight action is '{}'; ignoring",
                    actionName, g_actionInFlight);
                return;
            }
            logger::info(
                "ActionDispatcher: action '{}' completed (started={:.1f}, duration={:.1f}s)",
                g_actionInFlight, g_actionStartedAt, now - g_actionStartedAt);
            g_recentlyFired.push_back({g_actionInFlight, now});
            TrimRecentlyFiredLocked(now);
            g_actionInFlight.clear();
            g_actionStartedAt       = 0.0;
            g_lastActionCompletedAt = now;
        }

        // Push a fresh dashboard state so any in-flight indicator clears.
        DashboardUIManager::PushFullState();
    }

    namespace
    {
        // ---------- ActionContext rebuild on main thread ----------

        ActionContext BuildActionContextFromSnapshot(
            const Snapshot&              snapshot,
            PhaseTracker::Direction      desiredDirection = PhaseTracker::Direction::Raise,
            int                          tensionDelta     = 0)
        {
            ActionContext ctx;
            ctx.player = RE::PlayerCharacter::GetSingleton();
            if (ctx.player) {
                ctx.playerInCombat = ctx.player->IsInCombat();
            }
            ctx.playerInInterior = snapshot.player.cellIsInterior;
            ctx.locationName     = snapshot.player.locationName;
            ctx.cellName         = snapshot.player.cellName;
            if (auto* ui = RE::UI::GetSingleton()) {
                ctx.playerInDialogue = ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME);
            }
            ctx.desiredDirection = desiredDirection;
            ctx.tensionDelta     = tensionDelta;
            return ctx;
        }

        // ---------- global action preconditions ----------
        // Conditions under which NO action should fire, regardless of which
        // action the LLM might pick. Checked twice per action-firing tick:
        //
        //   1. Before sending the action-select prompt — saves an LLM round
        //      trip when we know up-front no action would be allowed to run
        //      anyway.
        //   2. After receiving the LLM response, just before the chosen
        //      action's Start — guards against the state changing during
        //      the round trip (player walked into combat, opened a dialogue
        //      menu, etc.).
        //
        // Individual actions' IsAvailable still owns *action-specific*
        // preconditions (e.g. "ambush requires exterior cell"). The split
        // is: globally-disqualifying world state lives here; per-action
        // situational fit lives on the action.
        //
        // Returns nullptr when all preconditions hold; otherwise returns a
        // short literal naming the gate that blocked. Caller logs the
        // reason; the literal lifetime is the program (string literal).
        const char* CheckGlobalActionPreconditions(const ActionContext& ctx)
        {
            if (ctx.playerInCombat)                  return "playerInCombat";
            if (ctx.playerInDialogue)                return "playerInDialogue";
            if (AlphaCanon::IsInScriptedScene())     return "scriptedScene";
            if (AlphaCanon::IsInDoNotDisturbCell())  return "doNotDisturbCell";
            return nullptr;
        }

        // ---------- tension delta ----------

        int ComputeTensionDelta(PhaseTracker::Phase phase, std::uint32_t currentTension)
        {
            const auto& cfg = Settings::Get();
            const int current = static_cast<int>(currentTension);
            switch (phase) {
                case PhaseTracker::Phase::Exposition:
                    return std::max(0, cfg.advanceThresholdExposition - current);
                case PhaseTracker::Phase::RisingAction:
                    return std::max(0, cfg.advanceThresholdRisingAction - current);
                case PhaseTracker::Phase::Climax:
                    return std::max(0, current - cfg.advanceThresholdClimax);
                case PhaseTracker::Phase::FallingAction:
                    return std::max(0, current - cfg.advanceThresholdFallingAction);
                case PhaseTracker::Phase::Resolution:
                    return std::max(0, cfg.advanceThresholdResolution - current);
                default:
                    return 0;
            }
        }

        // ---------- prompt context ----------

        // Number of events to feed the action-select prompt. Deliberately
        // smaller than the tension-eval prompt's tail — the LLM is making
        // a discrete choice from a small candidate set and doesn't need
        // the full history.
        constexpr std::size_t kActionSelectEventTailSize = 10;

        std::string BuildActionPromptContext(const Snapshot&              snapshot,
                                             const std::vector<IAction*>& candidates,
                                             PhaseTracker::Direction       direction,
                                             int                           tensionDelta)
        {
            nlohmann::json ctx;
            ctx["desired_direction"] =
                (direction == PhaseTracker::Direction::Raise) ? "raise" : "lower";
            ctx["tension_delta"] = tensionDelta;

            // candidates
            nlohmann::json candArr = nlohmann::json::array();
            for (auto* a : candidates) {
                if (!a) continue;
                candArr.push_back({
                    {"name",        a->Name()},
                    {"description", a->Description()},
                });
            }
            ctx["candidates"] = std::move(candArr);

            // player_context — thin block. No formID; the LLM doesn't need it.
            ctx["player_context"] = {
                {"location_name",    snapshot.player.locationName},
                {"cell_name",        snapshot.player.cellName},
                {"cell_is_interior", snapshot.player.cellIsInterior},
            };

            // recent_events — same processing chain as the tension-eval
            // prompt (phase filter + reverse + FormatEventsText +
            // BuildMergedTimeline), then sliced to a short tail.
            {
                auto parsed = nlohmann::json::parse(
                    snapshot.skyrimNetEventsJSON, nullptr, false);
                nlohmann::json skyrimSide = nlohmann::json::array();
                if (parsed.is_array()) {
                    const double cutoff = snapshot.phaseEnteredAtRealTime;
                    nlohmann::json filtered = nlohmann::json::array();
                    for (auto& evt : parsed) {
                        if (!evt.is_object()) continue;
                        if (cutoff > 0.0) {
                            const double et = evt.value("localTime", 0.0);
                            if (et < cutoff) continue;
                        }
                        filtered.push_back(std::move(evt));
                    }
                    std::reverse(filtered.begin(), filtered.end());
                    SkyrimNetEvents::FormatEventsText(
                        filtered, snapshot.player.gameTimeSeconds);
                    skyrimSide = std::move(filtered);
                }
                auto merged = SkyrimNetEvents::BuildMergedTimeline(
                    std::move(skyrimSide),
                    CombatEventLog::GetRenderedTail(snapshot.player.gameTimeSeconds),
                    snapshot.player.gameTimeSeconds);

                // Slice to last N. Keep newest-last ordering.
                if (merged.is_array() && merged.size() > kActionSelectEventTailSize) {
                    const std::size_t skipFront =
                        merged.size() - kActionSelectEventTailSize;
                    merged.erase(merged.begin(), merged.begin() + skipFront);
                }
                ctx["recent_events"] = std::move(merged);
            }

            return ctx.dump();
        }

        // ---------- finalize paths ----------

        // Called on the main thread when no LLM call will fire (gate blocked,
        // empty candidate list, etc.). Apply the unmodified record and call
        // the finalizer. Does NOT mark anything in dispatcher state — the
        // tick simply produced no action.
        void FinalizeWithoutAction(DecisionLog::DecisionRecord    rec,
                                   FinalizedCallback              onFinalized)
        {
            EvaluationPipeline::ApplyDecision(rec);
            if (onFinalized) onFinalized();
        }

        // Called on the main thread when an action-select LLM call has
        // already failed (network error, validation error, etc.). Marks the
        // record as failed, applies the global cooldown so we don't retry
        // immediately, then applies + finalizes.
        void FinalizeWithFailure(DecisionLog::DecisionRecord    rec,
                                 const std::string&             reason,
                                 FinalizedCallback              onFinalized)
        {
            rec.actionSelected = "(failed: " + reason + ")";
            {
                std::scoped_lock lock(g_mutex);
                g_lastActionCompletedAt = NowUnixSeconds();
            }
            logger::warn("ActionDispatcher: action-select failed: {}", reason);
            EvaluationPipeline::ApplyDecision(rec);
            if (onFinalized) onFinalized();
        }

        // Called on the main thread when the LLM response has been parsed
        // and is structurally valid. Validates the action name against the
        // candidate set, looks up the action, calls Start, updates state.
        void FinalizeWithLLMResponse(Snapshot                      snapshot,
                                     DecisionLog::DecisionRecord   rec,
                                     std::vector<std::string>      candidateNames,
                                     std::string                   chosenAction,
                                     nlohmann::json                parameters,
                                     std::string                   narrativeNote,
                                     PhaseTracker::Direction       direction,
                                     int                           tensionDelta,
                                     FinalizedCallback             onFinalized)
        {
            // Validate name is in the candidate set.
            const bool isValidCandidate =
                std::find(candidateNames.begin(), candidateNames.end(), chosenAction)
                != candidateNames.end();
            if (!isValidCandidate) {
                FinalizeWithFailure(
                    std::move(rec),
                    "LLM returned unknown action '" + chosenAction + "'",
                    std::move(onFinalized));
                return;
            }

            // Look up the action in the registry.
            IAction* action = ActionRegistry::Find(chosenAction);
            if (!action) {
                // Should be impossible — candidate names were sourced from
                // the registry — but defensive logging in case the registry
                // was somehow mutated mid-tick.
                FinalizeWithFailure(
                    std::move(rec),
                    "LLM-chosen action '" + chosenAction + "' missing from registry",
                    std::move(onFinalized));
                return;
            }

            // Rebuild ActionContext on the main thread (snapshot data may
            // be slightly stale; the live player / UI state is what Start
            // needs to act on). Direction and tensionDelta were captured
            // before the LLM round trip — they don't change as a function
            // of the LLM response, and Either-polarity actions consume
            // them inside Start.
            const ActionContext ctx =
                BuildActionContextFromSnapshot(snapshot, direction, tensionDelta);

            // Re-check global preconditions: the LLM round trip may have
            // taken seconds, during which the player could have entered
            // combat, opened a dialogue, walked into a scripted scene, or
            // crossed into a DND cell. Bailing here cleanly drops the
            // action with no cooldown — the same gate would block the
            // next tick's pre-call check too, but if the situation has
            // already cleared by then we want immediate re-eligibility.
            if (const char* blockedBy = CheckGlobalActionPreconditions(ctx)) {
                if (Settings::Get().debugMode) {
                    logger::debug(
                        "ActionDispatcher: global preconditions changed during LLM round trip (blocked: {}); dropping chosen action '{}'",
                        blockedBy, chosenAction);
                }
                FinalizeWithoutAction(std::move(rec), std::move(onFinalized));
                return;
            }

            // Populate the record fields up-front. actionSelected is
            // tentatively the action name; the failure path below will
            // overwrite it with the "(failed: ...)" form if Start fails.
            rec.actionSelected       = chosenAction;
            rec.actionParametersJSON = parameters.dump();
            if (!narrativeNote.empty()) {
                rec.narrativeNote = std::move(narrativeNote);
            }

            // Start the action. The action owns parameter validation and
            // reports started=true/false.
            const StartResult result = action->Start(ctx, parameters);
            if (!result.started) {
                FinalizeWithFailure(
                    std::move(rec),
                    result.detail.empty() ? std::string("action Start returned false")
                                          : ("Start: " + result.detail),
                    std::move(onFinalized));
                return;
            }

            // Started successfully — record in-flight state.
            {
                std::scoped_lock lock(g_mutex);
                g_actionInFlight  = chosenAction;
                g_actionStartedAt = NowUnixSeconds();
            }
            logger::info("ActionDispatcher: action '{}' started ({})",
                         chosenAction, result.detail);

            EvaluationPipeline::ApplyDecision(rec);
            if (onFinalized) onFinalized();
        }
    }

    void ConsiderAction(Snapshot                    snapshot,
                        DecisionLog::DecisionRecord rec,
                        FinalizedCallback           onFinalized)
    {
        const bool debug = Settings::Get().debugMode;

        // Snapshot the dispatcher state we need under the lock, then evaluate
        // gates without holding the lock so that logging and PhaseTracker
        // lookups don't serialize against the completion sink.
        std::string inFlightCopy;
        double      lastCompletedCopy = 0.0;
        {
            std::scoped_lock lock(g_mutex);
            inFlightCopy      = g_actionInFlight;
            lastCompletedCopy = g_lastActionCompletedAt;
        }

        // Gate 1: action already in flight.
        if (!inFlightCopy.empty()) {
            if (debug) {
                logger::debug("ActionDispatcher: gate in_flight blocked: '{}' is still running",
                              inFlightCopy);
            }
            FinalizeWithoutAction(std::move(rec), std::move(onFinalized));
            return;
        }

        // Gate 2: tension eval already advanced the phase this tick.
        if (rec.advancedToPhase) {
            if (debug) {
                logger::debug("ActionDispatcher: gate just_advanced blocked: phase advanced this tick");
            }
            FinalizeWithoutAction(std::move(rec), std::move(onFinalized));
            return;
        }

        // Gate 3: global cooldown.
        const double now      = NowUnixSeconds();
        const double cooldown = static_cast<double>(Settings::Get().actionCooldownSeconds);
        if (lastCompletedCopy > 0.0 && (now - lastCompletedCopy) < cooldown) {
            if (debug) {
                const double remaining = cooldown - (now - lastCompletedCopy);
                logger::debug("ActionDispatcher: gate cooldown blocked: {:.1f}s remaining",
                              remaining);
            }
            FinalizeWithoutAction(std::move(rec), std::move(onFinalized));
            return;
        }

        // Gate 4: phase dwell time vs. ideal duration.
        const auto currentPhaseOpt = PhaseTracker::PhaseFromName(snapshot.currentPhase);
        if (!currentPhaseOpt) {
            if (debug) {
                logger::debug("ActionDispatcher: gate dwell blocked: unknown phase '{}'",
                              snapshot.currentPhase);
            }
            FinalizeWithoutAction(std::move(rec), std::move(onFinalized));
            return;
        }
        const int ideal = IdealDurationFor(*currentPhaseOpt);
        if (snapshot.timeInPhaseSeconds < static_cast<float>(ideal)) {
            if (debug) {
                logger::debug("ActionDispatcher: gate dwell blocked: {:.1f}s / {}s ideal",
                              snapshot.timeInPhaseSeconds, ideal);
            }
            FinalizeWithoutAction(std::move(rec), std::move(onFinalized));
            return;
        }

        // Compute direction + tension delta now so they can flow into the
        // ActionContext (consumed by Either-polarity actions like
        // NPCLetterAction) AND into the action-select prompt.
        const auto direction = PhaseTracker::OutgoingDirection(*currentPhaseOpt);
        const int  tensionDelta = ComputeTensionDelta(*currentPhaseOpt, rec.tensionScore);

        // Gate 5: global action preconditions (player not in combat /
        // dialogue / scripted scene / DND cell). Checked before candidate
        // filtering so we don't pay the per-action IsAvailable cost — and
        // more importantly, before the LLM round trip below.
        const ActionContext ctx =
            BuildActionContextFromSnapshot(snapshot, direction, tensionDelta);
        if (const char* blockedBy = CheckGlobalActionPreconditions(ctx)) {
            if (debug) {
                logger::debug("ActionDispatcher: gate global_preconditions blocked: {}", blockedBy);
            }
            FinalizeWithoutAction(std::move(rec), std::move(onFinalized));
            return;
        }

        // Gate 6: at least one candidate must survive IsAvailable + recency.
        const ActionPolarity desired =
            (direction == PhaseTracker::Direction::Raise)
                ? ActionPolarity::Raise
                : ActionPolarity::Lower;
        auto candidates = ActionRegistry::AvailableMatching(ctx, desired);

        if (!candidates.empty()) {
            std::scoped_lock lock(g_mutex);
            TrimRecentlyFiredLocked(now);
            candidates.erase(
                std::remove_if(candidates.begin(), candidates.end(),
                               [now](IAction* a) {
                                   return WasFiredRecentlyLocked(a->Name(), now);
                               }),
                candidates.end());
        }

        if (candidates.empty()) {
            if (debug) {
                logger::debug("ActionDispatcher: gate candidates blocked: 0 candidates after filtering");
            }
            FinalizeWithoutAction(std::move(rec), std::move(onFinalized));
            return;
        }

        // All gates passed — fire the action-select LLM call.
        const char* dirName = (direction == PhaseTracker::Direction::Raise) ? "raise" : "lower";
        logger::info(
            "ActionDispatcher: firing action-select (direction={}, tension_delta={}, candidates={}, dwell={:.1f}/{}s)",
            dirName, tensionDelta, candidates.size(), snapshot.timeInPhaseSeconds, ideal);

        const std::string promptCtx =
            BuildActionPromptContext(snapshot, candidates, direction, tensionDelta);
        if (debug) {
            logger::debug("ActionDispatcher: action-select prompt context: {}", promptCtx);
        }

        // Capture candidate names by value so we can validate the LLM's
        // choice without re-touching the registry across the async boundary.
        std::vector<std::string> candidateNames;
        candidateNames.reserve(candidates.size());
        for (auto* a : candidates) {
            if (a) candidateNames.push_back(a->Name());
        }

        // Backup rec + finalizer so we can take the failure path on the rare
        // !queued case. SkyrimNetAPI::SendCustomPromptToLLM returning false
        // means the callback won't fire; without a backup, the moved-into-
        // lambda captures (including onFinalized) would silently leak and
        // EvaluationPipeline's g_inFlight would stay stuck.
        DecisionLog::DecisionRecord recBackup       = rec;
        FinalizedCallback           finalizedBackup = onFinalized;

        const bool queued = SkyrimNetAPI::SendCustomPromptToLLM(
            "narrative_engine_action_select", "narrative_engine_director", promptCtx,
            [snapshot       = std::move(snapshot),
             rec            = std::move(rec),
             candidateNames = std::move(candidateNames),
             onFinalized    = std::move(onFinalized),
             direction,
             tensionDelta]
            (std::string response, bool success) mutable
            {
                if (!success) {
                    // LLM call failed — marshal back and finalize as failure.
                    AsyncDispatch::MarshalToMainThread(
                        [rec = std::move(rec), onFinalized = std::move(onFinalized),
                         response = std::move(response)]() mutable {
                            FinalizeWithFailure(
                                std::move(rec),
                                std::string("LLM error: ") + response,
                                std::move(onFinalized));
                        });
                    return;
                }

                if (Settings::Get().debugMode) {
                    logger::debug("ActionDispatcher: action-select LLM callback: body={}B",
                                  response.size());
                    if (!response.empty()) {
                        logger::debug("ActionDispatcher: action-select LLM response: {}", response);
                    }
                }

                // Parse on the worker thread — no engine API touched.
                const std::string body = EvaluationPipeline::StripMarkdownFences(response);
                auto parsed = nlohmann::json::parse(body, nullptr, false);

                if (parsed.is_discarded() || !parsed.is_object()) {
                    AsyncDispatch::MarshalToMainThread(
                        [rec = std::move(rec), onFinalized = std::move(onFinalized),
                         response = std::move(response)]() mutable {
                            FinalizeWithFailure(
                                std::move(rec),
                                std::string("LLM response was not a JSON object: ") + response,
                                std::move(onFinalized));
                        });
                    return;
                }

                // Extract the fields we care about. Missing-field handling
                // is done in FinalizeWithLLMResponse / the action's Start.
                std::string chosenAction;
                if (auto it = parsed.find("action"); it != parsed.end() && it->is_string()) {
                    chosenAction = it->get<std::string>();
                }
                nlohmann::json parameters = nlohmann::json::object();
                if (auto it = parsed.find("parameters"); it != parsed.end() && it->is_object()) {
                    parameters = *it;
                }
                // narrative_note — sanitize first (smart quotes, em-dash,
                // ellipsis, NBSP, accented Latin → ASCII per
                // docs/LLM_RESPONSE_HANDLING.md), then clamp to 200 chars.
                std::string narrativeNote;
                if (auto it = parsed.find("narrative_note"); it != parsed.end() && it->is_string()) {
                    narrativeNote = LLMTextSanitizer::Sanitize(it->get<std::string>());
                    if (narrativeNote.size() > 200) {
                        narrativeNote.resize(200);
                    }
                }

                AsyncDispatch::MarshalToMainThread(
                    [snapshot       = std::move(snapshot),
                     rec            = std::move(rec),
                     candidateNames = std::move(candidateNames),
                     chosenAction   = std::move(chosenAction),
                     parameters     = std::move(parameters),
                     narrativeNote  = std::move(narrativeNote),
                     direction,
                     tensionDelta,
                     onFinalized    = std::move(onFinalized)]() mutable {
                        FinalizeWithLLMResponse(
                            std::move(snapshot), std::move(rec),
                            std::move(candidateNames), std::move(chosenAction),
                            std::move(parameters), std::move(narrativeNote),
                            direction, tensionDelta,
                            std::move(onFinalized));
                    });
            });

        if (!queued) {
            logger::warn(
                "ActionDispatcher: SendCustomPromptToLLM returned false; treating as failure");
            FinalizeWithFailure(
                std::move(recBackup),
                "SendCustomPromptToLLM returned false (queue full?)",
                std::move(finalizedBackup));
        }
    }

    void OnTick()
    {
        // Snapshot in-flight state once under the lock so the rest of
        // this function operates on stable values without re-locking
        // for every read.
        std::string inFlightName;
        double      startedAt = 0.0;
        {
            std::scoped_lock lock(g_mutex);
            if (g_actionInFlight.empty() || g_actionStartedAt <= 0.0) {
                return;
            }
            inFlightName = g_actionInFlight;
            startedAt    = g_actionStartedAt;
        }

        const double now = NowUnixSeconds();
        const double age = now - startedAt;

        // ---------- Stale-lock check ----------
        // If an action has been in flight longer than
        // iActionStaleLockTimeoutSeconds, the completion ModEvent likely
        // never came (quest force-stopped externally, Papyrus crashed,
        // etc.). Clear it so the Director can resume firing. We treat
        // the stale-clear as a completion for cooldown purposes —
        // otherwise the next tick would immediately re-fire.
        const double timeout = static_cast<double>(
            Settings::Get().actionStaleLockTimeoutSeconds);
        if (age > timeout) {
            bool didClear = false;
            {
                std::scoped_lock lock(g_mutex);
                if (g_actionInFlight == inFlightName) {
                    g_actionInFlight.clear();
                    g_actionStartedAt       = 0.0;
                    g_lastActionCompletedAt = now;
                    didClear                = true;
                }
            }
            if (didClear) {
                logger::warn(
                    "ActionDispatcher: stale-lock auto-clear: action '{}' was in flight for {:.0f}s (timeout {}s); clearing",
                    inFlightName, age, Settings::Get().actionStaleLockTimeoutSeconds);
                DashboardUIManager::PushFullState();
            }
            return;
        }

        // ---------- Failed-start verification ----------
        // Ask the action whether its start visibly failed (e.g. a quest
        // that we lit up but the engine never actually promoted). If so,
        // the action has already torn down its engine-side state by the
        // time DetectAndRollbackFailedStart returns; the dispatcher just
        // needs to clear its own in-flight bookkeeping so the next tick
        // can pick the action again.
        //
        // Note: NO cooldown is applied on this path (g_lastActionCompletedAt
        // is intentionally NOT updated). The user wants immediate retry —
        // a failed start isn't a successful action, so the global cooldown
        // shouldn't gate the retry attempt.
        auto* action = ActionRegistry::Find(inFlightName);
        if (!action) {
            return;
        }

        ActionContext ctx;
        ctx.player = RE::PlayerCharacter::GetSingleton();
        if (ctx.player) {
            ctx.playerInCombat   = ctx.player->IsInCombat();
            ctx.playerInInterior = ctx.player->GetParentCell() &&
                                   ctx.player->GetParentCell()->IsInteriorCell();
        }
        if (auto* ui = RE::UI::GetSingleton()) {
            ctx.playerInDialogue = ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME);
        }

        if (action->DetectAndRollbackFailedStart(ctx, age)) {
            bool didClear = false;
            {
                std::scoped_lock lock(g_mutex);
                // Re-check under lock — the completion ModEvent could have
                // arrived between the DetectAndRollbackFailedStart return
                // and our re-lock, in which case in-flight is already
                // cleared and we shouldn't double-handle.
                if (g_actionInFlight == inFlightName) {
                    g_actionInFlight.clear();
                    g_actionStartedAt = 0.0;
                    // Deliberately do NOT touch g_lastActionCompletedAt:
                    // a failed start should be retryable on the very next
                    // tick without a cooldown gate.
                    didClear = true;
                }
            }
            if (didClear) {
                logger::info(
                    "ActionDispatcher: action '{}' rolled back after failed start (age={:.1f}s); cleared in-flight for retry",
                    inFlightName, age);
                DashboardUIManager::PushFullState();
            }
            return;
        }

        // ---------- Completion verification ----------
        // Some actions resolve via engine-observable state (e.g. a quest
        // reaching its "Complete Quest" marked stage) rather than via a
        // Papyrus-sent _ne_ActionCompleted ModEvent. Ask the action
        // whether it considers itself done; on true, the action has
        // already torn down its engine-side state and we just clear our
        // bookkeeping and apply the post-action cooldown — same shape as
        // the ModEvent sink path, just initiated from the poll side.
        if (action->DetectCompletion(ctx, age)) {
            bool didClear = false;
            {
                std::scoped_lock lock(g_mutex);
                if (g_actionInFlight == inFlightName) {
                    g_actionInFlight.clear();
                    g_actionStartedAt       = 0.0;
                    g_lastActionCompletedAt = now;
                    didClear                = true;
                }
            }
            if (didClear) {
                logger::info(
                    "ActionDispatcher: action '{}' completed via poll (age={:.1f}s); cleared in-flight, cooldown applied",
                    inFlightName, age);
                DashboardUIManager::PushFullState();
            }
        }
    }

    bool IsActionInFlight()
    {
        std::scoped_lock lock(g_mutex);
        return !g_actionInFlight.empty();
    }

    std::optional<InFlightInfo> GetInFlightInfo()
    {
        std::scoped_lock lock(g_mutex);
        if (g_actionInFlight.empty()) {
            return std::nullopt;
        }
        return InFlightInfo{g_actionInFlight, g_actionStartedAt};
    }

    // ---------------------------------------------------------------------
    // Persistence
    // ---------------------------------------------------------------------

    void OnSave(SKSE::SerializationInterface* intfc)
    {
        if (!intfc) return;
        if (!intfc->OpenRecord(kRecordTypeId, kRecordVersion)) {
            logger::error("ActionDispatcher::OnSave: OpenRecord failed");
            return;
        }

        std::string nameCopy;
        double      startedCopy   = 0.0;
        double      completedCopy = 0.0;
        {
            std::scoped_lock lock(g_mutex);
            nameCopy      = g_actionInFlight;
            startedCopy   = g_actionStartedAt;
            completedCopy = g_lastActionCompletedAt;
        }
        WriteString(intfc, nameCopy);
        intfc->WriteRecordData(startedCopy);
        intfc->WriteRecordData(completedCopy);
    }

    void OnLoad(SKSE::SerializationInterface* intfc, std::uint32_t version, std::uint32_t length)
    {
        if (!intfc) return;
        if (version != kRecordVersion) {
            logger::warn(
                "ActionDispatcher::OnLoad: unknown version {} (length={}); clearing dispatcher state",
                version, length);
            OnRevert();
            return;
        }

        std::string nameLoaded;
        double      startedLoaded   = 0.0;
        double      completedLoaded = 0.0;
        if (!ReadString(intfc, nameLoaded)) {
            logger::error("ActionDispatcher::OnLoad: failed to read action-name string; clearing");
            OnRevert();
            return;
        }
        if (intfc->ReadRecordData(startedLoaded) != sizeof(startedLoaded)) {
            logger::error("ActionDispatcher::OnLoad: short read on startedAt; clearing");
            OnRevert();
            return;
        }
        if (intfc->ReadRecordData(completedLoaded) != sizeof(completedLoaded)) {
            logger::error("ActionDispatcher::OnLoad: short read on lastCompletedAt; clearing");
            OnRevert();
            return;
        }

        {
            std::scoped_lock lock(g_mutex);
            g_actionInFlight        = std::move(nameLoaded);
            g_actionStartedAt       = startedLoaded;
            g_lastActionCompletedAt = completedLoaded;
            g_recentlyFired.clear();
        }
        logger::info(
            "ActionDispatcher::OnLoad: restored inFlight='{}' startedAt={:.1f} lastCompletedAt={:.1f}",
            g_actionInFlight, g_actionStartedAt, g_lastActionCompletedAt);
    }

    void OnRevert()
    {
        std::scoped_lock lock(g_mutex);
        g_actionInFlight.clear();
        g_actionStartedAt       = 0.0;
        g_lastActionCompletedAt = 0.0;
        g_recentlyFired.clear();
    }
}
