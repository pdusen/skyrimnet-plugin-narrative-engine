#include <MCMEventSink.h>

#include <AsyncDispatch.h>
#include <logger.h>
#include <Settings.h>

#include <atomic>

namespace NarrativeEngine::MCMEventSink
{
    namespace
    {
        // Must match the string _ne_MCM.psc passes to SendModEvent.
        constexpr const char* kEventName = "_ne_DashboardHotkeyChanged";

        struct HotkeyChangedSink : public RE::BSTEventSink<SKSE::ModCallbackEvent>
        {
            RE::BSEventNotifyControl ProcessEvent(const SKSE::ModCallbackEvent* a_event,
                                                  RE::BSTEventSource<SKSE::ModCallbackEvent>* /*src*/) override
            {
                if (!a_event) {
                    logger::trace("MCMEventSink[trace]: null event pointer");
                    return RE::BSEventNotifyControl::kContinue;
                }
                // Trace EVERY ModEvent that lands on the sink, matched or
                // not. A missing "dashboard hotkey rebound" log line makes
                // it impossible to tell whether _ne_MCM.psc fired at all
                // vs fired with a wrong event name — that ambiguity has
                // burned diagnosis time on the "MCM won't load" report.
                const std::string sender = a_event->sender ? std::string{a_event->sender->GetName()} : std::string{};
                logger::trace("MCMEventSink[trace]: ModCallback received: name='{}' strArg='{}' numArg={:.3f} "
                              "sender='{}' (expect name='{}' -> {})",
                              std::string{a_event->eventName},
                              std::string{a_event->strArg},
                              a_event->numArg,
                              sender,
                              kEventName,
                              a_event->eventName == kEventName ? "MATCH" : "skip");
                if (a_event->eventName != kEventName) {
                    return RE::BSEventNotifyControl::kContinue;
                }

                AsyncDispatch::MarshalToMainThread([] {
                    const auto& before = Settings::Get();
                    const int prevDxsc = before.dashboardHotkeyDXSC;
                    const std::uint8_t prevMods = before.dashboardHotkeyModifiers;
                    Settings::ApplyMcmOverride();
                    const auto& cfg = Settings::Get();
                    logger::info("MCMEventSink: dashboard hotkey rebound DXSC={} mods={}",
                                 cfg.dashboardHotkeyDXSC,
                                 static_cast<int>(cfg.dashboardHotkeyModifiers));
                    logger::trace("MCMEventSink[trace]: post-apply hotkey DXSC {}->{} mods 0x{:02X}->0x{:02X}",
                                  prevDxsc,
                                  cfg.dashboardHotkeyDXSC,
                                  static_cast<int>(prevMods),
                                  static_cast<int>(cfg.dashboardHotkeyModifiers));
                });

                return RE::BSEventNotifyControl::kContinue;
            }
        };

        HotkeyChangedSink g_sink;
        std::atomic<bool> g_registered{false};
    } // namespace

    void Initialize()
    {
        bool expected = false;
        if (!g_registered.compare_exchange_strong(expected, true)) {
            logger::trace("MCMEventSink[trace]: Initialize called twice; second call is a no-op");
            return;
        }

        auto* source = SKSE::GetModCallbackEventSource();
        logger::trace("MCMEventSink[trace]: GetModCallbackEventSource() -> {}",
                      source ? "OK" : "NULL (SKSE messaging not ready?)");
        if (!source) {
            logger::error("MCMEventSink: ModCallbackEventSource unavailable");
            g_registered.store(false);
            return;
        }
        source->AddEventSink<SKSE::ModCallbackEvent>(&g_sink);
        logger::info("MCMEventSink: initialized (listening for {})", kEventName);
        logger::trace("MCMEventSink[trace]: sink registered on ModCallback source at {}. "
                      "If _ne_MCM.psc's OnSettingChange ever fires, the sink will trace-log the event.",
                      static_cast<const void*>(source));
    }
} // namespace NarrativeEngine::MCMEventSink
