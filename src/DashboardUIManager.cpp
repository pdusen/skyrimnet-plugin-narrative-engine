#include <DashboardUIManager.h>

#include <ActionDispatcher.h>
#include <ActionRegistry.h>
#include <AlphaCanon.h>
#include <AsyncDispatch.h>
#include <BeatRegistry.h>
#include <BeatSystem.h>
#include <CombatEventLog.h>
#include <DecisionLog.h>
#include <LetterPool.h>
#include <PhaseTracker.h>
#include <PrismaUI.h>
#include <Settings.h>
#include <SkyrimNetAPI.h>
#include <SkyrimNetEvents.h>
#include <Tick.h>
#include <VisitConclusionPoll.h>
#include <VisitState.h>
#include <logger.h>

#include <nlohmann/json.hpp>

#include <Windows.h>

#include <atomic>
#include <string>

namespace NarrativeEngine::DashboardUIManager
{
    namespace
    {
        PrismaUI_API::ViewHandle g_view = PrismaUI_API::kInvalidView;
        std::atomic<bool>        g_visible = false;

        // -- Hotkey input sink --------------------------------------------
        //
        // PrismaUI doesn't expose a hotkey-registration API, so we hook
        // SKSE's input device manager and dispatch on matching button-down
        // events. The dispatch marshals back to the main thread before
        // touching PrismaUI / engine state — the input sink may fire on a
        // non-main thread.

        struct HotkeySink : public RE::BSTEventSink<RE::InputEvent*>
        {
            RE::BSEventNotifyControl ProcessEvent(
                RE::InputEvent* const* a_event,
                RE::BSTEventSource<RE::InputEvent*>* /*a_source*/) override
            {
                if (!a_event || !*a_event) {
                    return RE::BSEventNotifyControl::kContinue;
                }

                bool fire = false;
                for (auto* e = *a_event; e; e = e->next) {
                    if (e->GetEventType() != RE::INPUT_EVENT_TYPE::kButton) continue;
                    auto* btn = e->AsButtonEvent();
                    if (!btn || !btn->IsDown()) continue;

                    const std::uint32_t scanCode = btn->GetIDCode();
                    // SKSE input events use DirectInput scan codes; the user
                    // configures the hotkey as a Windows VK code. Translate
                    // via MapVirtualKeyW so the comparison is in VK space.
                    const std::uint32_t vk = ::MapVirtualKeyW(scanCode, MAPVK_VSC_TO_VK);
                    if (vk == 0) continue;

                    // ESC closes the dashboard when it's open and no
                    // modifier keys are held. Doesn't open it when closed
                    // — matches the standard "Escape dismisses overlays"
                    // affordance without stealing the keybind in normal
                    // play (where ESC opens the pause menu). Modifier-
                    // combo ESC bindings are left alone so the player can
                    // still use them for other mods.
                    if (vk == VK_ESCAPE) {
                        if (!g_visible.load()) continue;
                        const bool anyMod =
                            (::GetAsyncKeyState(VK_CONTROL) & 0x8000) ||
                            (::GetAsyncKeyState(VK_SHIFT)   & 0x8000) ||
                            (::GetAsyncKeyState(VK_MENU)    & 0x8000);
                        if (anyMod) continue;
                        fire = true;
                        break;
                    }

                    if (static_cast<int>(vk) != Settings::Get().dashboardHotkeyVK) continue;

                    // Modifier match is exact, not a superset. (F7+Shift is
                    // a different binding than plain F7.)
                    std::uint8_t actualMods = 0;
                    if (::GetAsyncKeyState(VK_CONTROL) & 0x8000) actualMods |= Settings::kModCtrl;
                    if (::GetAsyncKeyState(VK_SHIFT)   & 0x8000) actualMods |= Settings::kModShift;
                    if (::GetAsyncKeyState(VK_MENU)    & 0x8000) actualMods |= Settings::kModAlt;
                    if (actualMods != Settings::Get().dashboardHotkeyModifiers) continue;

                    fire = true;
                    break;
                }

                if (fire) {
                    AsyncDispatch::MarshalToMainThread([] {
                        ToggleVisibility();
                    });
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        HotkeySink* GetHotkeySink()
        {
            static HotkeySink instance;
            return &instance;
        }

        // PrismaUI resolves `htmlPath` internally as a path under
        // Data/PrismaUI/views/. Both absolute filesystem paths and CWD-
        // relative paths fail because Mod Organizer 2's VFS hides the
        // file from the real filesystem. Pass the views-root-relative
        // path and let PrismaUI handle resolution.
        constexpr const char* kHtmlPath = "NarrativeEngine/dashboard/index.html";

        // Every input-listener handler below ends its marshaled main-
        // thread lambda with a call to PushFullState so the dashboard
        // reflects the state change within a frame — no more "the
        // checkbox lied until you reopened the dashboard" surprises.
        // The push is inside the marshal, not around it, so the state
        // is guaranteed to reflect the mutation the listener just
        // applied.

        // Parse `"true"` / `"1"` / everything else. Consistent with
        // the tick-toggle argument shape SkyrimNet's JS bridge sends.
        bool ParseBoolArg(const char* argument)
        {
            const std::string arg = argument ? argument : "";
            return arg == "true" || arg == "1";
        }

        // JS -> C++ listener for the dashboard debug tick killswitch.
        // Fires on PrismaUI's worker thread when the dashboard checkbox
        // is toggled; marshal to the main thread before touching engine
        // state. Payload is `"true"` or `"false"` (JSON booleans stringified).
        void OnSetTickEnabled(const char* argument)
        {
            const bool enabled = ParseBoolArg(argument);
            logger::info("DashboardUIManager: ne_setTickEnabled({}) received",
                         enabled ? "true" : "false");
            AsyncDispatch::MarshalToMainThread([enabled] {
                Tick::SetEnabled(enabled);
                PushFullState();
            });
        }

        // JS -> C++ listener for per-action enabled toggles on the
        // Dispatch tab. Payload is a JSON object
        // `{"name":"npc_letter","enabled":true}`.
        void OnSetActionEnabled(const char* argument)
        {
            const std::string arg = argument ? argument : "";
            auto parsed = nlohmann::json::parse(arg, nullptr, false);
            if (parsed.is_discarded() || !parsed.is_object()) {
                logger::warn(
                    "DashboardUIManager: ne_setActionEnabled: malformed payload '{}'",
                    arg);
                return;
            }
            std::string name;
            bool enabled = false;
            if (auto it = parsed.find("name"); it != parsed.end() && it->is_string()) {
                name = it->get<std::string>();
            }
            if (auto it = parsed.find("enabled"); it != parsed.end() && it->is_boolean()) {
                enabled = it->get<bool>();
            }
            if (name.empty()) {
                logger::warn(
                    "DashboardUIManager: ne_setActionEnabled: missing/empty name in '{}'",
                    arg);
                return;
            }
            logger::info(
                "DashboardUIManager: ne_setActionEnabled(name='{}', enabled={}) received",
                name, enabled);
            AsyncDispatch::MarshalToMainThread(
                [name = std::move(name), enabled]() mutable {
                    BeatRegistry::SetEnabled(name, enabled);
                    PushFullState();
                });
        }

        // Backs the "Enable All" / "Disable All" bulk buttons under
        // the beats table. Payload is `"true"` or `"false"`.
        void OnSetAllActionsEnabled(const char* argument)
        {
            const bool enabled = ParseBoolArg(argument);
            logger::info(
                "DashboardUIManager: ne_setAllActionsEnabled({}) received",
                enabled ? "true" : "false");
            AsyncDispatch::MarshalToMainThread([enabled] {
                for (const auto& entry : BeatRegistry::All()) {
                    BeatRegistry::SetEnabled(entry.name, enabled);
                }
                PushFullState();
            });
        }

        // Backs the per-row "Dispatch" button. Payload is the bare
        // beat name (no JSON wrapping — one field). Routes through
        // BeatSystem::ForceDispatchBeat so the beat-select LLM still
        // runs against a one-element candidate list — parameter
        // validation flows through the same code path as the normal
        // Director-cadence dispatch.
        void OnDispatchAction(const char* argument)
        {
            const std::string name = argument ? argument : "";
            if (name.empty()) {
                logger::warn("DashboardUIManager: ne_dispatchAction: empty name");
                return;
            }
            logger::info(
                "DashboardUIManager: ne_dispatchAction('{}') received", name);
            AsyncDispatch::MarshalToMainThread([name]() mutable {
                BeatSystem::ForceDispatchBeat(name);
                // Push once now so the dashboard reflects the "in-
                // flight" state within a frame.
                PushFullState();
            });
        }
    }

    void Initialize()
    {
        if (!PrismaUI_API::IsAvailable()) {
            logger::info("DashboardUIManager: PrismaUI absent; dashboard disabled");
            return;
        }

        logger::info("DashboardUIManager: creating view from {}", kHtmlPath);
        g_view = PrismaUI_API::CreateView(kHtmlPath);
        if (g_view == PrismaUI_API::kInvalidView) {
            logger::error("DashboardUIManager: CreateView failed for path {}", kHtmlPath);
            return;
        }

        // Start hidden; the player toggles with the configured hotkey.
        PrismaUI_API::Hide(g_view);
        g_visible = false;

        // JS -> C++ listeners for dashboard input controls. Registered
        // before the view goes live so the first Show()/user
        // interaction can immediately route into the handlers.
        PrismaUI_API::RegisterJSListener(g_view, "ne_setTickEnabled",       &OnSetTickEnabled);
        PrismaUI_API::RegisterJSListener(g_view, "ne_setActionEnabled",     &OnSetActionEnabled);
        PrismaUI_API::RegisterJSListener(g_view, "ne_setAllActionsEnabled", &OnSetAllActionsEnabled);
        PrismaUI_API::RegisterJSListener(g_view, "ne_dispatchAction",       &OnDispatchAction);

        // Hook input events for the hotkey.
        if (auto* inputManager = RE::BSInputDeviceManager::GetSingleton()) {
            inputManager->AddEventSink<RE::InputEvent*>(GetHotkeySink());
        } else {
            logger::warn("DashboardUIManager: BSInputDeviceManager unavailable; hotkey disabled");
        }

        const auto& cfg = Settings::Get();
        logger::info("DashboardUIManager: initialized (view created, hotkey VK={} mods={})",
                     cfg.dashboardHotkeyVK, static_cast<int>(cfg.dashboardHotkeyModifiers));
    }

    void Shutdown()
    {
        if (g_view != PrismaUI_API::kInvalidView) {
            PrismaUI_API::Destroy(g_view);
            g_view = PrismaUI_API::kInvalidView;
            g_visible = false;
        }
    }

    std::string ComposeFullStateJSON()
    {
        nlohmann::json j;

        // status
        j["status"] = {
            {"skyrim_net_available", SkyrimNetAPI::IsAvailable()},
            {"skyrim_net_version",   SkyrimNetAPI::GetVersion()},
            // MVP has no enable/disable toggle. Future settings could expose
            // one; for now the Director is unconditionally on whenever the
            // plugin loaded.
            {"director_enabled",     true},
            {"prisma_ui_available",  PrismaUI_API::IsAvailable()},
            // Runtime debug killswitch. The dashboard renders a checkbox
            // bound to this and calls back via `window.ne_setTickEnabled`.
            {"tick_enabled",         Tick::IsEnabled()},
        };

        // phase
        j["current_phase"]         = PhaseTracker::PhaseName(PhaseTracker::Get());
        j["time_in_phase_seconds"] = PhaseTracker::TimeInPhaseSeconds();

        // recent_decisions + last_evaluation (the latter is just the newest
        // record on the tail, surfaced as a separate panel so the dashboard
        // can highlight the alpha_canon_signals bitmask via the names
        // helper; the list entries don't carry that field).
        const auto tail = DecisionLog::Tail(10);
        nlohmann::json decisions = nlohmann::json::array();
        for (const auto& r : tail) {
            decisions.push_back({
                {"timestamp",      r.realTimeSec},
                {"tension_score",  r.tensionScore},
                {"phase",          PhaseTracker::PhaseName(r.currentPhase)},
                {"action",         r.beatSelected.empty()
                                       ? nlohmann::json(nullptr)
                                       : nlohmann::json(r.beatSelected)},
                {"narrative_note", r.narrativeNote},
            });
        }
        j["recent_decisions"] = std::move(decisions);

        if (!tail.empty()) {
            const auto& latest = tail.back();
            const auto mask = static_cast<AlphaCanon::Signal>(latest.alphaCanonActiveSignals);
            j["last_evaluation"] = {
                {"timestamp",           latest.realTimeSec},
                {"tension_score",       latest.tensionScore},
                {"narrative_note",      latest.narrativeNote},
                {"advanced_to",         latest.advancedToPhase
                                            ? nlohmann::json(PhaseTracker::PhaseName(*latest.advancedToPhase))
                                            : nlohmann::json(nullptr)},
                {"alpha_canon_signals", AlphaCanon::Names(mask)},
                {"action",              latest.beatSelected.empty()
                                            ? nlohmann::json(nullptr)
                                            : nlohmann::json(latest.beatSelected)},
            };
        } else {
            j["last_evaluation"] = nullptr;
        }

        // action_in_flight — populated from BeatSystem's live top-
        // level state, not the decision log. (JSON key retained as
        // "action_in_flight" because the dashboard's JS side still
        // reads that key; the client-side rename to "beat_in_flight"
        // is a follow-up.)
        if (auto info = BeatSystem::GetInFlightInfo()) {
            j["action_in_flight"] = {
                {"name",       info->name},
                {"started_at", info->startedAtRealSeconds},
                {"state",      static_cast<int>(info->state)},
            };
        } else {
            j["action_in_flight"] = nullptr;
        }

        // recent_events — SkyrimNet's tail merged with NarrativeEngine's
        // internal combat tail, formatted via the shared helper (same
        // `text` synthesis + condensation the LLM prompt uses, so the
        // dashboard reads identically to what the Director sees).
        double currentGameTimeSeconds = 0.0;
        if (auto* cal = RE::Calendar::GetSingleton()) {
            currentGameTimeSeconds = static_cast<double>(cal->GetDaysPassed()) * 86400.0;
        }

        const auto eventsJson = SkyrimNetAPI::GetRecentEvents(0, 20, "");
        auto parsed = nlohmann::json::parse(eventsJson, /*cb=*/nullptr, /*allow_exceptions=*/false);
        nlohmann::json skyrimSide = nlohmann::json::array();
        if (parsed.is_array()) {
            std::reverse(parsed.begin(), parsed.end());
            SkyrimNetEvents::FormatEventsText(parsed, currentGameTimeSeconds);
            skyrimSide = std::move(parsed);
        }
        j["recent_events"] = SkyrimNetEvents::BuildMergedTimeline(
            std::move(skyrimSide),
            CombatEventLog::GetRenderedTail(currentGameTimeSeconds),
            currentGameTimeSeconds);

        // letter_pool — full per-slot snapshot for the Letters tab, plus
        // the most-recent-dispatch index so the client doesn't have to
        // scan for it. `most_recent_dispatch_slot` is the slot with the
        // largest deliveredAt among non-Free slots, or null when every
        // slot is Free.
        {
            const auto snapshots = LetterPool::GetSlotSnapshots();
            nlohmann::json slots = nlohmann::json::array();
            int    mostRecentIdx = -1;
            double mostRecentTs  = 0.0;
            for (const auto& s : snapshots) {
                const bool isFree = (s.state == LetterPool::State::Free);
                const char* stateStr = "free";
                switch (s.state) {
                    case LetterPool::State::Free:            stateStr = "free"; break;
                    case LetterPool::State::PendingDelivery: stateStr = "pending_delivery"; break;
                    case LetterPool::State::InInventory:     stateStr = "in_inventory"; break;
                    case LetterPool::State::Read:            stateStr = "read"; break;
                    case LetterPool::State::Discarded:       stateStr = "free"; break;
                }
                // Body preview: strip any <font …>…</font> wrappers the ESP
                // rendering path might add later, then truncate to ~200
                // chars. Done here so the payload stays small even when
                // the LLM wrote a long letter.
                std::string preview;
                if (!isFree && !s.body.empty()) {
                    preview = s.body;
                    // Strip any <font ...>...</font> wrapper anywhere in the
                    // preview by removing every <font …> open tag and every
                    // closing </font> tag. Cheap, defensive, tolerant of
                    // nested or truncated tags.
                    for (;;) {
                        const auto open = preview.find("<font");
                        if (open == std::string::npos) break;
                        const auto close = preview.find('>', open);
                        if (close == std::string::npos) break;
                        preview.erase(open, close - open + 1);
                    }
                    for (;;) {
                        const auto pos = preview.find("</font>");
                        if (pos == std::string::npos) break;
                        preview.erase(pos, std::string_view{"</font>"}.size());
                    }
                    if (preview.size() > 200) {
                        preview.resize(200);
                    }
                }
                slots.push_back({
                    {"index",        s.index},
                    {"state",        stateStr},
                    {"letter_label", isFree ? std::string{} : s.senderLabel},
                    {"topic_tag",    isFree ? std::string{} : s.topicTag},
                    {"mood",         isFree ? std::string{} : s.mood},
                    {"body_preview", preview},
                    {"delivered_at", isFree ? 0.0 : s.deliveredAt},
                    {"read_at",      s.readAt},
                });
                if (!isFree && s.deliveredAt > mostRecentTs) {
                    mostRecentTs  = s.deliveredAt;
                    mostRecentIdx = static_cast<int>(s.index);
                }
            }
            j["letter_pool"] = {
                {"slots", std::move(slots)},
                {"most_recent_dispatch_slot",
                    mostRecentIdx >= 0
                        ? nlohmann::json(mostRecentIdx)
                        : nlohmann::json(nullptr)},
            };
        }

        // actions — for the Dispatch tab. Every registered beat, in
        // registration order, with its runtime toggle state, the wall-
        // clock of its most recent successful dispatch (session only),
        // and any in-game-hours cooldown remaining. (JSON key retained
        // as "actions" pending the JS side's rename.)
        {
            nlohmann::json actions = nlohmann::json::array();
            for (const auto& entry : BeatRegistry::All()) {
                if (!entry.beat) continue;
                actions.push_back({
                    {"name",                     entry.name},
                    {"enabled",                  entry.enabled},
                    {"last_dispatched_at",       entry.lastDispatchedRealTime},
                    {"remaining_cooldown_hours", entry.beat->RemainingCooldownGameHours()},
                });
            }
            j["actions"] = std::move(actions);
        }

        // visit — Phase 05 Step 16. The Visit tab renders three
        // sections: the current conversation (when a visit is in
        // flight), recent poll verdicts (up to 5), and recent
        // dispatch history (up to 10). All state is per-process
        // except the snapshot fields, which co-save-persist so the
        // dashboard survives save/load.
        {
            const auto snap    = VisitState::GetSnapshot();
            const auto mode    = VisitState::DerivePhase();
            const auto history = VisitState::GetHistory();
            const auto verdicts = VisitConclusionPoll::GetRecentVerdicts();

            const auto modeStr = [](VisitState::Mode m) -> const char* {
                switch (m) {
                    case VisitState::Mode::Idle:        return "idle";
                    case VisitState::Mode::Composing:   return "composing";
                    case VisitState::Mode::Salutation:  return "salutation";
                    case VisitState::Mode::Discuss:     return "discuss";
                    case VisitState::Mode::OnHold:      return "on_hold";
                    case VisitState::Mode::ReEngage:    return "reengage";
                    case VisitState::Mode::Valediction: return "valediction";
                    case VisitState::Mode::ReturnHome:  return "return_home";
                }
                return "idle";
            };
            const auto outcomeStr = [](VisitState::Outcome o) -> const char* {
                switch (o) {
                    case VisitState::Outcome::Completed:   return "completed";
                    case VisitState::Outcome::Unsatisfied: return "unsatisfied";
                    case VisitState::Outcome::RolledBack:  return "rolled_back";
                    case VisitState::Outcome::Aborted:     return "aborted";
                }
                return "completed";
            };

            nlohmann::json current = nullptr;
            if (mode != VisitState::Mode::Idle) {
                std::string briefingPreview = snap.briefingText;
                if (briefingPreview.size() > 200) briefingPreview.resize(200);
                current = {
                    {"mode",              modeStr(mode)},
                    {"sender_form_id",    snap.senderFormID},
                    {"topic_tag",         snap.topicTag},
                    {"mood",              snap.mood},
                    {"briefing_preview",  briefingPreview},
                    {"dispatched_at",     snap.dispatchedAtRealSeconds},
                    {"ignore_nudge_count", snap.ignoreNudgeCount},
                };
            }

            nlohmann::json verdictsJson = nlohmann::json::array();
            for (const auto& v : verdicts) {
                verdictsJson.push_back({
                    {"fired_at",        v.firedAtRealSeconds},
                    {"should_conclude", v.shouldConclude},
                    {"rationale",       v.rationale},
                });
            }

            nlohmann::json historyJson = nlohmann::json::array();
            for (const auto& h : history) {
                historyJson.push_back({
                    {"dispatched_at",     h.dispatchedAt},
                    {"sender_name",       h.senderName},
                    {"topic_tag",         h.topicTag},
                    {"outcome",           outcomeStr(h.outcome)},
                    {"duration_seconds",  h.durationSeconds},
                });
            }

            j["visit"] = {
                {"current",         current},
                {"recent_verdicts", std::move(verdictsJson)},
                {"history",         std::move(historyJson)},
            };
        }

        return j.dump();
    }

    void PushFullState()
    {
        if (!PrismaUI_API::IsAvailable() || g_view == PrismaUI_API::kInvalidView) {
            return;
        }
        const std::string json = ComposeFullStateJSON();
        PrismaUI_API::InvokeJS(g_view, "updateFullState", json);
    }

    void ToggleVisibility()
    {
        if (!PrismaUI_API::IsAvailable() || g_view == PrismaUI_API::kInvalidView) {
            return;
        }
        if (g_visible.load()) {
            // Unfocus before Hide — paired teardown matches IntelEngine's
            // working sequence. Show without Focus, Hide without Unfocus,
            // either step alone leaves the view in a bad state.
            PrismaUI_API::Unfocus(g_view);
            PrismaUI_API::Hide(g_view);
            g_visible = false;
            logger::debug("DashboardUIManager: hidden");
        } else {
            // Push fresh state before showing so the view doesn't render the
            // last stale snapshot.
            PushFullState();
            PrismaUI_API::Show(g_view);
            // Focus pauses the game while the dashboard is on screen. The
            // dashboard is a read-only observability surface; pausing makes
            // it easier for the player to actually read the state instead
            // of dodging arrows while squinting at the panel.
            PrismaUI_API::Focus(g_view, /*pauseGame=*/true, /*disableFocusMenu=*/false);
            g_visible = true;
            logger::debug("DashboardUIManager: shown");
        }
    }
}
