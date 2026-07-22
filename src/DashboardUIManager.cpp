#include <DashboardUIManager.h>

#include <AlphaCanon.h>
#include <AsyncDispatch.h>
#include <BeatRegistry.h>
#include <BeatSystem.h>
#include <CombatEventLog.h>
#include <DecisionLog.h>
#include <LetterPool.h>
#include <logger.h>
#include <MainThread.h>
#include <PhaseTracker.h>
#include <PrismaUI.h>
#include <Settings.h>
#include <SkyrimNetAPI.h>
#include <SkyrimNetEvents.h>
#include <Tick.h>
#include <TravelEventLog.h>
#include <VisitConclusionPoll.h>
#include <VisitState.h>
#include <WeatherEventLog.h>

#include <nlohmann/json.hpp>

#include <Windows.h>

#include <atomic>
#include <charconv>
#include <string>

namespace NarrativeEngine::DashboardUIManager
{
    namespace
    {
        PrismaUI_API::ViewHandle g_view = PrismaUI_API::kInvalidView;
        std::atomic<bool> g_visible = false;

        // Set true by ne_beginHotkeyRebind; consumed by HotkeySink's
        // per-button-down loop on the next non-modifier keypress. While
        // true, the Settings tab's rebind modal is displayed (a pure
        // function of the state pushed to the browser). Reset by:
        //   * a successful capture (writes MCM INI, PushFullState);
        //   * ESC during capture (cancels);
        //   * ne_cancelHotkeyRebind from the modal's Cancel button;
        //   * ToggleVisibility's hide branch (safety — a mid-rebind hide
        //     mustn't leave a latent capture that snags a random
        //     keypress later).
        std::atomic<bool> g_hotkeyCaptureMode = false;

        // -- Hotkey input sink --------------------------------------------
        //
        // PrismaUI doesn't expose a hotkey-registration API, so we hook
        // SKSE's input device manager and dispatch on matching button-down
        // events. The dispatch marshals back to the main thread before
        // touching PrismaUI / engine state — the input sink may fire on a
        // non-main thread.
        //
        // The comparison is in DirectX-scan-code space throughout — the
        // native input space of SKSE's button events and SkyUI's keymap
        // controls. `Settings::Get().dashboardHotkeyDXSC` is likewise a
        // scan code (populated from the plugin INI or overridden at
        // runtime by _ne_MCM.psc's ModEvent), so no scan/VK translation
        // is needed.

        // DIK_ESCAPE. Used for the "ESC closes the dashboard" affordance
        // below. Modifier reads still use GetAsyncKeyState, which speaks
        // VK — but that's an OS-level "is the key currently held" probe,
        // not an event-time key match, and doesn't participate in the
        // scan-code comparison.
        constexpr std::uint32_t kDIK_ESCAPE = 1;

        struct HotkeySink : public RE::BSTEventSink<RE::InputEvent*>
        {
            RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_event,
                                                  RE::BSTEventSource<RE::InputEvent*>* /*a_source*/) override
            {
                if (!a_event || !*a_event) {
                    return RE::BSEventNotifyControl::kContinue;
                }

                bool fire = false;
                for (auto* e = *a_event; e; e = e->next) {
                    if (e->GetEventType() != RE::INPUT_EVENT_TYPE::kButton)
                        continue;
                    auto* btn = e->AsButtonEvent();
                    if (!btn || !btn->IsDown())
                        continue;

                    const std::uint32_t dxsc = btn->GetIDCode();
                    if (dxsc == 0)
                        continue;

                    // Hotkey-rebind capture mode. Consumes the next
                    // non-modifier keypress, writes the four MCM-INI
                    // hotkey keys via WriteMcmOverride, and drops back
                    // to normal binding-match mode. Modifier keys seen
                    // during capture are skipped so pressing Shift on
                    // the way to Shift+F7 doesn't bind just Shift; the
                    // held-modifier state is probed at capture time via
                    // GetAsyncKeyState. ESC cancels rather than binds.
                    if (g_hotkeyCaptureMode.load(std::memory_order_acquire)) {
                        // Left/Right Shift, Ctrl, Alt DIK codes.
                        if (dxsc == 42 || dxsc == 54 || dxsc == 29 || dxsc == 157 || dxsc == 56 || dxsc == 184) {
                            continue;
                        }
                        if (dxsc == kDIK_ESCAPE) {
                            g_hotkeyCaptureMode.store(false, std::memory_order_release);
                            logger::info("DashboardUIManager: hotkey rebind cancelled (ESC)");
                            AsyncDispatch::MarshalToMainThread([] { PushFullState(); });
                            return RE::BSEventNotifyControl::kContinue;
                        }
                        const bool shift = (::GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                        const bool ctrl = (::GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                        const bool alt = (::GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
                        const int capturedDxsc = static_cast<int>(dxsc);
                        g_hotkeyCaptureMode.store(false, std::memory_order_release);
                        logger::info("DashboardUIManager: hotkey rebound DXSC={} shift={} ctrl={} alt={}",
                                     capturedDxsc,
                                     shift ? 1 : 0,
                                     ctrl ? 1 : 0,
                                     alt ? 1 : 0);
                        AsyncDispatch::MarshalToMainThread([capturedDxsc, shift, ctrl, alt] {
                            Settings::McmOverride mut;
                            mut.dashboardHotkeyDXSC = capturedDxsc;
                            mut.hotkeyShift = shift;
                            mut.hotkeyCtrl = ctrl;
                            mut.hotkeyAlt = alt;
                            Settings::WriteMcmOverride(mut);
                            PushFullState();
                        });
                        return RE::BSEventNotifyControl::kContinue;
                    }

                    // ESC closes the dashboard when it's open and no
                    // modifier keys are held. Doesn't open it when closed
                    // — matches the standard "Escape dismisses overlays"
                    // affordance without stealing the keybind in normal
                    // play (where ESC opens the pause menu). Modifier-
                    // combo ESC bindings are left alone so the player can
                    // still use them for other mods.
                    if (dxsc == kDIK_ESCAPE) {
                        if (!g_visible.load())
                            continue;
                        const bool anyMod = (::GetAsyncKeyState(VK_CONTROL) & 0x8000)
                                            || (::GetAsyncKeyState(VK_SHIFT) & 0x8000)
                                            || (::GetAsyncKeyState(VK_MENU) & 0x8000);
                        if (anyMod)
                            continue;
                        fire = true;
                        break;
                    }

                    if (static_cast<int>(dxsc) != Settings::Get().dashboardHotkeyDXSC)
                        continue;

                    // Modifier match is exact, not a superset. (F7+Shift is
                    // a different binding than plain F7.) The bitmask is
                    // packed to SkyUI convention: bit 0 = Shift, bit 1 =
                    // Ctrl, bit 2 = Alt.
                    std::uint8_t actualMods = 0;
                    if (::GetAsyncKeyState(VK_SHIFT) & 0x8000)
                        actualMods |= Settings::kModShift;
                    if (::GetAsyncKeyState(VK_CONTROL) & 0x8000)
                        actualMods |= Settings::kModCtrl;
                    if (::GetAsyncKeyState(VK_MENU) & 0x8000)
                        actualMods |= Settings::kModAlt;
                    if (actualMods != Settings::Get().dashboardHotkeyModifiers)
                        continue;

                    fire = true;
                    break;
                }

                if (fire) {
                    // Gate the *open* action so the hotkey doesn't stack our
                    // overlay on top of the Main Menu, MCM, Inventory,
                    // Journal, or another mod's PrismaUI view. The *close*
                    // action always fires: if the dashboard is somehow
                    // already visible during a pause (or while another
                    // PrismaUI overlay has focus), the player should still
                    // be able to dismiss it with the hotkey.
                    //
                    // Two gates:
                    //   - RE::UI::GameIsPaused() catches every engine menu
                    //     that pauses the game (Main Menu, Inventory, MCM,
                    //     load screens, ...).
                    //   - PrismaUI::HasAnyActiveFocus() catches other mods'
                    //     PrismaUI overlays, which don't pause the game.
                    if (!g_visible.load()) {
                        auto* ui = RE::UI::GetSingleton();
                        if ((ui && ui->GameIsPaused()) || PrismaUI_API::HasAnyActiveFocus()) {
                            fire = false;
                        }
                    }
                }

                if (fire) {
                    AsyncDispatch::MarshalToMainThread([] { ToggleVisibility(); });
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

        // Friendly-name display for a DIK scan code + SkyUI-convention
        // modifier bitmask. Powers state.settings.dashboard_hotkey_display
        // so the Settings tab shows "Ctrl+F7" instead of raw numbers.
        // Uncommon codes fall back to "DIK #<N>". Modifiers render in a
        // fixed order (Ctrl, Shift, Alt) — one canonical shape so
        // rebinds always display the same way.
        std::string FormatHotkeyBinding(int dxsc, std::uint8_t mods)
        {
            if (dxsc < 0) {
                return "(none)";
            }
            std::string s;
            if (mods & Settings::kModCtrl)
                s += "Ctrl+";
            if (mods & Settings::kModShift)
                s += "Shift+";
            if (mods & Settings::kModAlt)
                s += "Alt+";

            // DIK constants — same values SKSE's button events emit.
            switch (dxsc) {
            case 1:
                s += "Esc";
                break;
            case 14:
                s += "Backspace";
                break;
            case 15:
                s += "Tab";
                break;
            case 28:
                s += "Enter";
                break;
            case 41:
                s += "`";
                break;
            case 43:
                s += "\\";
                break;
            case 57:
                s += "Space";
                break;
            case 59:
                s += "F1";
                break;
            case 60:
                s += "F2";
                break;
            case 61:
                s += "F3";
                break;
            case 62:
                s += "F4";
                break;
            case 63:
                s += "F5";
                break;
            case 64:
                s += "F6";
                break;
            case 65:
                s += "F7";
                break;
            case 66:
                s += "F8";
                break;
            case 67:
                s += "F9";
                break;
            case 68:
                s += "F10";
                break;
            case 87:
                s += "F11";
                break;
            case 88:
                s += "F12";
                break;
            // Digits (main row).
            case 2:
                s += "1";
                break;
            case 3:
                s += "2";
                break;
            case 4:
                s += "3";
                break;
            case 5:
                s += "4";
                break;
            case 6:
                s += "5";
                break;
            case 7:
                s += "6";
                break;
            case 8:
                s += "7";
                break;
            case 9:
                s += "8";
                break;
            case 10:
                s += "9";
                break;
            case 11:
                s += "0";
                break;
            // Letters (QWERTY-order DIK layout — not alphabetical).
            case 16:
                s += "Q";
                break;
            case 17:
                s += "W";
                break;
            case 18:
                s += "E";
                break;
            case 19:
                s += "R";
                break;
            case 20:
                s += "T";
                break;
            case 21:
                s += "Y";
                break;
            case 22:
                s += "U";
                break;
            case 23:
                s += "I";
                break;
            case 24:
                s += "O";
                break;
            case 25:
                s += "P";
                break;
            case 30:
                s += "A";
                break;
            case 31:
                s += "S";
                break;
            case 32:
                s += "D";
                break;
            case 33:
                s += "F";
                break;
            case 34:
                s += "G";
                break;
            case 35:
                s += "H";
                break;
            case 36:
                s += "J";
                break;
            case 37:
                s += "K";
                break;
            case 38:
                s += "L";
                break;
            case 44:
                s += "Z";
                break;
            case 45:
                s += "X";
                break;
            case 46:
                s += "C";
                break;
            case 47:
                s += "V";
                break;
            case 48:
                s += "B";
                break;
            case 49:
                s += "N";
                break;
            case 50:
                s += "M";
                break;
            // Arrows.
            case 200:
                s += "Up";
                break;
            case 208:
                s += "Down";
                break;
            case 203:
                s += "Left";
                break;
            case 205:
                s += "Right";
                break;
            default:
                s += "DIK #" + std::to_string(dxsc);
                break;
            }
            return s;
        }

        // JS -> C++ listener for the dashboard debug tick killswitch.
        // Fires on PrismaUI's worker thread when the dashboard checkbox
        // is toggled; marshal to the main thread before touching engine
        // state. Payload is `"true"` or `"false"` (JSON booleans stringified).
        void OnSetTickEnabled(const char* argument)
        {
            const bool enabled = ParseBoolArg(argument);
            logger::info("DashboardUIManager: ne_setTickEnabled({}) received", enabled ? "true" : "false");
            AsyncDispatch::MarshalToMainThread([enabled] {
                Tick::SetEnabled(enabled);
                // Persist so the toggle survives reboot. Phase 08 change:
                // pre-Phase 08 this was session-only. Both dashboard mount
                // points (Dispatch and Settings tabs) flow through this
                // handler, so the write happens once regardless of which
                // surface the player toggled.
                Settings::McmOverride mut;
                mut.tickEnabled = enabled;
                Settings::WriteMcmOverride(mut);
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
                logger::warn("DashboardUIManager: ne_setActionEnabled: malformed payload '{}'", arg);
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
                logger::warn("DashboardUIManager: ne_setActionEnabled: missing/empty name in '{}'", arg);
                return;
            }
            logger::info("DashboardUIManager: ne_setActionEnabled(name='{}', enabled={}) received", name, enabled);
            AsyncDispatch::MarshalToMainThread([name = std::move(name), enabled]() mutable {
                BeatRegistry::SetEnabled(name, enabled);
                PushFullState();
            });
        }

        // Backs the "Enable All" / "Disable All" bulk buttons under
        // the beats table. Payload is `"true"` or `"false"`.
        void OnSetAllActionsEnabled(const char* argument)
        {
            const bool enabled = ParseBoolArg(argument);
            logger::info("DashboardUIManager: ne_setAllActionsEnabled({}) received", enabled ? "true" : "false");
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
            logger::info("DashboardUIManager: ne_dispatchAction('{}') received", name);
            AsyncDispatch::MarshalToMainThread([name]() mutable {
                BeatSystem::ForceDispatchBeat(name);
                // Push once now so the dashboard reflects the "in-
                // flight" state within a frame.
                PushFullState();
            });
        }

        // Backs the Dispatch tab's Abort button (after confirmation).
        // Marshals to main and delegates to BeatSystem::AbortRunningBeat.
        // No argument. Safe if the button somehow fires when no beat is
        // in flight — the backend no-ops in that case.
        void OnAbortRunningBeat(const char* /*argument*/)
        {
            logger::info("DashboardUIManager: ne_abortRunningBeat received");
            AsyncDispatch::MarshalToMainThread([] {
                BeatSystem::AbortRunningBeat();
                PushFullState();
            });
        }

        // Backs the Settings tab's Debug Mode checkbox. Payload is
        // `"true"` or `"false"`.
        void OnSetDebugMode(const char* argument)
        {
            const bool enabled = ParseBoolArg(argument);
            logger::info("DashboardUIManager: ne_setDebugMode({}) received", enabled ? "true" : "false");
            AsyncDispatch::MarshalToMainThread([enabled] {
                Settings::McmOverride mut;
                mut.debugMode = enabled;
                Settings::WriteMcmOverride(mut);
                PushFullState();
            });
        }

        // Backs the Settings tab's Tick Interval slider. Payload is a
        // bare integer string; clamped to the slider's [10, 600] range
        // defensively so a browser-side glitch can't write a wild value.
        void OnSetTickInterval(const char* argument)
        {
            const std::string arg = argument ? argument : "";
            int value = 0;
            const auto* first = arg.data();
            const auto* last = arg.data() + arg.size();
            auto [ptr, ec] = std::from_chars(first, last, value);
            if (ec != std::errc{} || ptr != last) {
                logger::warn("DashboardUIManager: ne_setTickInterval: malformed payload '{}'", arg);
                return;
            }
            if (value < 10)
                value = 10;
            if (value > 600)
                value = 600;
            logger::info("DashboardUIManager: ne_setTickInterval({}) received", value);
            AsyncDispatch::MarshalToMainThread([value] {
                Settings::McmOverride mut;
                mut.tickIntervalSeconds = value;
                Settings::WriteMcmOverride(mut);
                PushFullState();
            });
        }

        // Backs the Settings tab's Minimum Phase Duration slider. Payload
        // is a bare integer string, clamped to `[0, 600]`. Same shape as
        // OnSetTickInterval — one integer, one INI write, one push.
        void OnSetMinPhaseDuration(const char* argument)
        {
            const std::string arg = argument ? argument : "";
            int value = 0;
            const auto* first = arg.data();
            const auto* last = arg.data() + arg.size();
            auto [ptr, ec] = std::from_chars(first, last, value);
            if (ec != std::errc{} || ptr != last) {
                logger::warn("DashboardUIManager: ne_setMinPhaseDuration: malformed payload '{}'", arg);
                return;
            }
            if (value < 0)
                value = 0;
            if (value > 600)
                value = 600;
            logger::info("DashboardUIManager: ne_setMinPhaseDuration({}) received", value);
            AsyncDispatch::MarshalToMainThread([value] {
                Settings::McmOverride mut;
                mut.minPhaseDurationSeconds = value;
                Settings::WriteMcmOverride(mut);
                PushFullState();
            });
        }

        // Backs the Settings tab's five per-phase duration sliders.
        // Payload is `{"phase":"exposition","seconds":420}`. Phase name
        // is validated against the five known values; seconds is
        // clamped to the phase's per-slider range.
        void OnSetPhaseIdealDuration(const char* argument)
        {
            const std::string arg = argument ? argument : "";
            auto parsed = nlohmann::json::parse(arg, nullptr, false);
            if (parsed.is_discarded() || !parsed.is_object()) {
                logger::warn("DashboardUIManager: ne_setPhaseIdealDuration: malformed payload '{}'", arg);
                return;
            }
            std::string phase;
            int seconds = 0;
            if (auto it = parsed.find("phase"); it != parsed.end() && it->is_string()) {
                phase = it->get<std::string>();
            }
            if (auto it = parsed.find("seconds"); it != parsed.end() && it->is_number_integer()) {
                seconds = it->get<int>();
            }
            if (phase.empty()) {
                logger::warn("DashboardUIManager: ne_setPhaseIdealDuration: missing/empty phase in '{}'", arg);
                return;
            }

            // Per-phase slider ranges match Settings tab UI: climax is
            // 30..600 (shorter phase), the other four are 60..1200.
            const auto clamp = [](int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); };
            Settings::McmOverride mut;
            if (phase == "exposition") {
                mut.idealDurationExposition = clamp(seconds, 60, 1200);
            } else if (phase == "rising_action") {
                mut.idealDurationRisingAction = clamp(seconds, 60, 1200);
            } else if (phase == "climax") {
                mut.idealDurationClimax = clamp(seconds, 30, 600);
            } else if (phase == "falling_action") {
                mut.idealDurationFallingAction = clamp(seconds, 60, 1200);
            } else if (phase == "resolution") {
                mut.idealDurationResolution = clamp(seconds, 60, 1200);
            } else {
                logger::warn("DashboardUIManager: ne_setPhaseIdealDuration: unknown phase '{}'", phase);
                return;
            }
            logger::info(
                "DashboardUIManager: ne_setPhaseIdealDuration(phase='{}', seconds={}) received", phase, seconds);
            AsyncDispatch::MarshalToMainThread([mut = std::move(mut)]() mutable {
                Settings::WriteMcmOverride(mut);
                PushFullState();
            });
        }

        // Backs the Settings tab's Rebind button. Flips the input sink
        // into hotkey-capture mode; the next non-modifier keypress
        // becomes the new binding (see HotkeySink::ProcessEvent). No
        // argument.
        void OnBeginHotkeyRebind(const char* /*argument*/)
        {
            logger::info("DashboardUIManager: hotkey rebind capture armed");
            AsyncDispatch::MarshalToMainThread([] {
                g_hotkeyCaptureMode.store(true, std::memory_order_release);
                PushFullState();
            });
        }

        // Backs the Settings tab's rebind modal's Cancel button.
        // Idempotent — if the capture already ended (e.g. via ESC in
        // the input sink) this is a no-op. No argument.
        void OnCancelHotkeyRebind(const char* /*argument*/)
        {
            AsyncDispatch::MarshalToMainThread([] {
                const bool was = g_hotkeyCaptureMode.exchange(false, std::memory_order_acq_rel);
                if (was) {
                    logger::info("DashboardUIManager: hotkey rebind cancelled (Cancel button)");
                } else {
                    logger::debug("DashboardUIManager: ne_cancelHotkeyRebind: capture already inactive");
                }
                PushFullState();
            });
        }
    } // namespace

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
        PrismaUI_API::RegisterJSListener(g_view, "ne_setTickEnabled", &OnSetTickEnabled);
        PrismaUI_API::RegisterJSListener(g_view, "ne_setActionEnabled", &OnSetActionEnabled);
        PrismaUI_API::RegisterJSListener(g_view, "ne_setAllActionsEnabled", &OnSetAllActionsEnabled);
        PrismaUI_API::RegisterJSListener(g_view, "ne_dispatchAction", &OnDispatchAction);
        PrismaUI_API::RegisterJSListener(g_view, "ne_abortRunningBeat", &OnAbortRunningBeat);
        // Phase 08 Settings tab listeners.
        PrismaUI_API::RegisterJSListener(g_view, "ne_setDebugMode", &OnSetDebugMode);
        PrismaUI_API::RegisterJSListener(g_view, "ne_setTickInterval", &OnSetTickInterval);
        PrismaUI_API::RegisterJSListener(g_view, "ne_setMinPhaseDuration", &OnSetMinPhaseDuration);
        PrismaUI_API::RegisterJSListener(g_view, "ne_setPhaseIdealDuration", &OnSetPhaseIdealDuration);
        PrismaUI_API::RegisterJSListener(g_view, "ne_beginHotkeyRebind", &OnBeginHotkeyRebind);
        PrismaUI_API::RegisterJSListener(g_view, "ne_cancelHotkeyRebind", &OnCancelHotkeyRebind);

        // Hook input events for the hotkey.
        if (auto* inputManager = RE::BSInputDeviceManager::GetSingleton()) {
            inputManager->AddEventSink<RE::InputEvent*>(GetHotkeySink());
        } else {
            logger::warn("DashboardUIManager: BSInputDeviceManager unavailable; hotkey disabled");
        }

        const auto& cfg = Settings::Get();
        logger::info("DashboardUIManager: initialized (view created, hotkey DXSC={} mods={})",
                     cfg.dashboardHotkeyDXSC,
                     static_cast<int>(cfg.dashboardHotkeyModifiers));
    }

    void Shutdown()
    {
        if (g_view != PrismaUI_API::kInvalidView) {
            PrismaUI_API::Destroy(g_view);
            g_view = PrismaUI_API::kInvalidView;
            g_visible = false;
        }
    }

    std::string ComposeFullStateJSON(const DashboardEngineReads& reads)
    {
        nlohmann::json j;

        // status
        j["status"] = {
            {"skyrim_net_available", SkyrimNetAPI::IsAvailable()},
            {"skyrim_net_version", SkyrimNetAPI::GetVersion()},
            // MVP has no enable/disable toggle. Future settings could expose
            // one; for now the Director is unconditionally on whenever the
            // plugin loaded.
            {"director_enabled", true},
            {"prisma_ui_available", PrismaUI_API::IsAvailable()},
            // Runtime debug killswitch. The dashboard renders a checkbox
            // bound to this and calls back via `window.ne_setTickEnabled`.
            {"tick_enabled", Tick::IsEnabled()},
        };

        // settings — Phase 08. Backs the Settings tab; also provides a
        // second read of tick_enabled for the Dispatch tab (via its own
        // status field) and the Settings tab (via this one) so both
        // mount points share one on-disk source of truth.
        {
            const auto& cfg = Settings::Get();
            j["settings"] = {
                {"debug_mode", cfg.debugMode},
                {"dashboard_hotkey_display",
                 FormatHotkeyBinding(cfg.dashboardHotkeyDXSC, cfg.dashboardHotkeyModifiers)},
                {"dashboard_hotkey_capture_active", g_hotkeyCaptureMode.load(std::memory_order_acquire)},
                {"tick_enabled", Tick::IsEnabled()},
                {"tick_interval_seconds", cfg.tickIntervalSeconds},
                {"min_phase_duration_seconds", cfg.minPhaseDurationSeconds},
                {"ideal_duration_seconds",
                 {
                     {"exposition", cfg.idealDurationExposition},
                     {"rising_action", cfg.idealDurationRisingAction},
                     {"climax", cfg.idealDurationClimax},
                     {"falling_action", cfg.idealDurationFallingAction},
                     {"resolution", cfg.idealDurationResolution},
                 }},
            };
        }

        // phase
        j["current_phase"] = PhaseTracker::PhaseName(PhaseTracker::Get());
        j["time_in_phase_seconds"] = PhaseTracker::TimeInPhaseSeconds();

        // recent_decisions + last_evaluation (the latter is just the newest
        // record on the tail, surfaced as a separate panel so the dashboard
        // can highlight the alpha_canon_signals bitmask via the names
        // helper; the list entries don't carry that field).
        const auto tail = DecisionLog::Tail(10);
        nlohmann::json decisions = nlohmann::json::array();
        for (const auto& r : tail) {
            decisions.push_back({
                {"timestamp", r.realTimeSec},
                {"tension_score", r.tensionScore},
                {"phase", PhaseTracker::PhaseName(r.currentPhase)},
                {"action", r.beatSelected.empty() ? nlohmann::json(nullptr) : nlohmann::json(r.beatSelected)},
                {"narrative_note", r.narrativeNote},
            });
        }
        j["recent_decisions"] = std::move(decisions);

        if (!tail.empty()) {
            const auto& latest = tail.back();
            const auto mask = static_cast<AlphaCanon::Signal>(latest.alphaCanonActiveSignals);
            j["last_evaluation"] = {
                {"timestamp", latest.realTimeSec},
                {"tension_score", latest.tensionScore},
                {"narrative_note", latest.narrativeNote},
                {"advanced_to",
                 latest.advancedToPhase ? nlohmann::json(PhaseTracker::PhaseName(*latest.advancedToPhase))
                                        : nlohmann::json(nullptr)},
                {"alpha_canon_signals", AlphaCanon::Names(mask)},
                {"action", latest.beatSelected.empty() ? nlohmann::json(nullptr) : nlohmann::json(latest.beatSelected)},
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
                {"name", info->name},
                {"started_at", info->startedAtRealSeconds},
                {"state", static_cast<int>(info->state)},
            };
        } else {
            j["action_in_flight"] = nullptr;
        }

        // recent_events — SkyrimNet's tail merged with NarrativeEngine's
        // internal combat tail, formatted via the shared helper (same
        // `text` synthesis + condensation the LLM prompt uses, so the
        // dashboard reads identically to what the Director sees).
        //
        // currentGameTimeSeconds comes from the caller — RE::Calendar
        // reads are main-thread-only and PushFullState bundles that
        // read into a single MainThread::Run alongside the per-beat
        // cooldown gather below.
        const double currentGameTimeSeconds = reads.currentGameTimeSeconds;

        const auto eventsJson = SkyrimNetAPI::GetRecentEvents(0, 20, "");
        auto parsed = nlohmann::json::parse(eventsJson, /*cb=*/nullptr, /*allow_exceptions=*/false);
        nlohmann::json skyrimSide = nlohmann::json::array();
        if (parsed.is_array()) {
            std::reverse(parsed.begin(), parsed.end());
            SkyrimNetEvents::FormatEventsText(parsed, currentGameTimeSeconds);
            skyrimSide = std::move(parsed);
        }
        j["recent_events"] =
            SkyrimNetEvents::BuildMergedTimeline(std::move(skyrimSide),
                                                 CombatEventLog::GetRenderedTail(currentGameTimeSeconds),
                                                 WeatherEventLog::GetRenderedTail(currentGameTimeSeconds),
                                                 TravelEventLog::GetRenderedTail(currentGameTimeSeconds),
                                                 currentGameTimeSeconds);

        // letter_pool — full per-slot snapshot for the Letters tab, plus
        // the most-recent-dispatch index so the client doesn't have to
        // scan for it. `most_recent_dispatch_slot` is the slot with the
        // largest deliveredAt among non-Free slots, or null when every
        // slot is Free.
        {
            const auto snapshots = LetterPool::GetSlotSnapshots();
            nlohmann::json slots = nlohmann::json::array();
            int mostRecentIdx = -1;
            double mostRecentTs = 0.0;
            for (const auto& s : snapshots) {
                const bool isFree = (s.state == LetterPool::State::Free);
                const char* stateStr = "free";
                switch (s.state) {
                case LetterPool::State::Free:
                    stateStr = "free";
                    break;
                case LetterPool::State::PendingDelivery:
                    stateStr = "pending_delivery";
                    break;
                case LetterPool::State::InInventory:
                    stateStr = "in_inventory";
                    break;
                case LetterPool::State::Read:
                    stateStr = "read";
                    break;
                case LetterPool::State::Discarded:
                    stateStr = "free";
                    break;
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
                        if (open == std::string::npos)
                            break;
                        const auto close = preview.find('>', open);
                        if (close == std::string::npos)
                            break;
                        preview.erase(open, close - open + 1);
                    }
                    for (;;) {
                        const auto pos = preview.find("</font>");
                        if (pos == std::string::npos)
                            break;
                        preview.erase(pos, std::string_view{"</font>"}.size());
                    }
                    if (preview.size() > 200) {
                        preview.resize(200);
                    }
                }
                slots.push_back({
                    {"index", s.index},
                    {"state", stateStr},
                    {"letter_label", isFree ? std::string{} : s.senderLabel},
                    {"topic_tag", isFree ? std::string{} : s.topicTag},
                    {"mood", isFree ? std::string{} : s.mood},
                    {"body_preview", preview},
                    {"delivered_at", isFree ? 0.0 : s.deliveredAt},
                    {"read_at", s.readAt},
                });
                if (!isFree && s.deliveredAt > mostRecentTs) {
                    mostRecentTs = s.deliveredAt;
                    mostRecentIdx = static_cast<int>(s.index);
                }
            }
            j["letter_pool"] = {
                {"slots", std::move(slots)},
                {"most_recent_dispatch_slot",
                 mostRecentIdx >= 0 ? nlohmann::json(mostRecentIdx) : nlohmann::json(nullptr)},
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
                if (!entry.beat)
                    continue;
                // Cooldown hours were captured on the main thread by
                // PushFullState alongside the calendar read; look up
                // by name here rather than re-invoking the IBeat method
                // off-main.
                double cooldownHours = 0.0;
                if (auto it = reads.beatCooldownHours.find(entry.name); it != reads.beatCooldownHours.end()) {
                    cooldownHours = it->second;
                }
                actions.push_back({
                    {"name", entry.name},
                    {"enabled", entry.enabled},
                    {"last_dispatched_at", entry.lastDispatchedRealTime},
                    {"remaining_cooldown_hours", cooldownHours},
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
            const auto snap = VisitState::GetSnapshot();
            const auto mode = VisitState::DerivePhase();
            const auto history = VisitState::GetHistory();
            const auto verdicts = VisitConclusionPoll::GetRecentVerdicts();

            const auto modeStr = [](VisitState::Mode m) -> const char* {
                switch (m) {
                case VisitState::Mode::Idle:
                    return "idle";
                case VisitState::Mode::Composing:
                    return "composing";
                case VisitState::Mode::Salutation:
                    return "salutation";
                case VisitState::Mode::Discuss:
                    return "discuss";
                case VisitState::Mode::OnHold:
                    return "on_hold";
                case VisitState::Mode::ReEngage:
                    return "reengage";
                case VisitState::Mode::Valediction:
                    return "valediction";
                case VisitState::Mode::ReturnHome:
                    return "return_home";
                }
                return "idle";
            };
            const auto outcomeStr = [](VisitState::Outcome o) -> const char* {
                switch (o) {
                case VisitState::Outcome::Completed:
                    return "completed";
                case VisitState::Outcome::Unsatisfied:
                    return "unsatisfied";
                case VisitState::Outcome::RolledBack:
                    return "rolled_back";
                case VisitState::Outcome::Aborted:
                    return "aborted";
                }
                return "completed";
            };

            nlohmann::json current = nullptr;
            if (mode != VisitState::Mode::Idle) {
                std::string briefingPreview = snap.briefingText;
                if (briefingPreview.size() > 200)
                    briefingPreview.resize(200);
                current = {
                    {"mode", modeStr(mode)},
                    {"sender_form_id", snap.senderFormID},
                    {"topic_tag", snap.topicTag},
                    {"mood", snap.mood},
                    {"briefing_preview", briefingPreview},
                    {"dispatched_at", snap.dispatchedAtRealSeconds},
                    {"ignore_nudge_count", snap.ignoreNudgeCount},
                };
            }

            nlohmann::json verdictsJson = nlohmann::json::array();
            for (const auto& v : verdicts) {
                verdictsJson.push_back({
                    {"fired_at", v.firedAtRealSeconds},
                    {"should_conclude", v.shouldConclude},
                    {"rationale", v.rationale},
                });
            }

            nlohmann::json historyJson = nlohmann::json::array();
            for (const auto& h : history) {
                historyJson.push_back({
                    {"dispatched_at", h.dispatchedAt},
                    {"sender_name", h.senderName},
                    {"topic_tag", h.topicTag},
                    {"outcome", outcomeStr(h.outcome)},
                    {"duration_seconds", h.durationSeconds},
                });
            }

            j["visit"] = {
                {"current", current},
                {"recent_verdicts", std::move(verdictsJson)},
                {"history", std::move(historyJson)},
            };
        }

        return j.dump();
    }

    void PushFullState()
    {
        if (!PrismaUI_API::IsAvailable() || g_view == PrismaUI_API::kInvalidView) {
            return;
        }
        // Audit finding 4 fix: skip the compose entirely when the
        // dashboard is hidden. Every applied Director decision used to
        // recompute the full state JSON regardless of whether anybody
        // was looking — pure waste on the main thread.
        if (!g_visible.load(std::memory_order_acquire)) {
            return;
        }

        // Move the compose off main. The caller may be on any thread
        // (main from ToggleVisibility / MarshalToMainThread input
        // handlers, plugin from ApplyDecision) — either way we
        // enqueue onto the plugin thread and run the JSON build
        // there. One bundled MainThread::Run hop covers RE::Calendar
        // and per-beat IBeat::RemainingCooldownGameHours (the only
        // engine-touching reads ComposeFullStateJSON needs); the rest
        // of its reads (LetterPool, BeatRegistry, PhaseTracker,
        // DecisionLog, VisitState, VisitConclusionPoll, SkyrimNetAPI
        // event tail) are all mutex-guarded or DLL-thread-safe.
        AsyncDispatch::EnqueueWork([](const PluginThread::Token& pt) {
            const DashboardEngineReads reads = MainThread::Run(pt, [](const MainThread::Token&) {
                DashboardEngineReads r;
                if (auto* cal = RE::Calendar::GetSingleton()) {
                    r.currentGameTimeSeconds = static_cast<double>(cal->GetDaysPassed()) * 86400.0;
                }
                for (const auto& entry : BeatRegistry::All()) {
                    if (!entry.beat) {
                        continue;
                    }
                    r.beatCooldownHours.emplace(entry.name, entry.beat->RemainingCooldownGameHours());
                }
                return r;
            });
            const std::string json = ComposeFullStateJSON(reads);
            // PrismaUI_API::InvokeJS is documented as async on
            // PrismaUI's end — safe to call off main.
            PrismaUI_API::InvokeJS(g_view, "updateFullState", json);
        });
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
            // Safety: if the player hides the dashboard mid-rebind, clear
            // the capture flag so the next arbitrary keypress (with the
            // dashboard closed) doesn't land as an unintended binding.
            g_hotkeyCaptureMode.store(false, std::memory_order_release);
            logger::debug("DashboardUIManager: hidden");
        } else {
            // Flip the visibility flag BEFORE PushFullState so the
            // visibility guard inside PushFullState (audit finding 4
            // fix — skip the compose when hidden) accepts this initial
            // "seed the view with fresh state" push. The compose runs
            // on the plugin thread; Show/Focus below fire immediately
            // on main. The InvokeJS therefore races Show — either
            // lands first (view appears with fresh data) or shortly
            // after (view briefly shows empty state, then pops in
            // within a frame or two). Either way better than the
            // pre-fix behaviour where the initial push was silently
            // dropped and the view stayed on the "waiting" placeholder.
            g_visible = true;
            PushFullState();
            PrismaUI_API::Show(g_view);
            // Focus pauses the game while the dashboard is on screen. The
            // dashboard is a read-only observability surface; pausing makes
            // it easier for the player to actually read the state instead
            // of dodging arrows while squinting at the panel.
            PrismaUI_API::Focus(g_view, /*pauseGame=*/true, /*disableFocusMenu=*/false);
            logger::debug("DashboardUIManager: shown");
        }
    }
} // namespace NarrativeEngine::DashboardUIManager
