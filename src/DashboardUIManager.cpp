#include <DashboardUIManager.h>

#include <AlphaCanon.h>
#include <AsyncDispatch.h>
#include <CombatEventLog.h>
#include <DecisionLog.h>
#include <PhaseTracker.h>
#include <PrismaUI.h>
#include <Settings.h>
#include <SkyrimNetAPI.h>
#include <SkyrimNetEvents.h>
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
                {"action",         r.actionSelected.empty()
                                       ? nlohmann::json(nullptr)
                                       : nlohmann::json(r.actionSelected)},
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
            };
        } else {
            j["last_evaluation"] = nullptr;
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
