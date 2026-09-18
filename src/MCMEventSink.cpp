#include <MCMEventSink.h>

#include <AsyncDispatch.h>
#include <logger.h>
#include <MainThread.h>
#include <Settings.h>

#include <atomic>
#include <string_view>

namespace NarrativeEngine::MCMEventSink
{
    namespace
    {
        // Must match the string _ne_MCM.psc passes to SendModEvent.
        constexpr const char* kEventName = "_ne_DashboardHotkeyChanged";

        // Every ModEvent this plugin could possibly have sent starts with
        // this. The sink is registered on the game-wide ModCallback source,
        // so most of what arrives belongs to other mods.
        constexpr std::string_view kOurEventPrefix = "_ne_";

        // One line the first time somebody else's event arrives, then
        // silence. See the comment on the trace call below for why this is
        // not simply dropped.
        std::atomic<bool> g_loggedForeignEvent = false;

        struct HotkeyChangedSink : public RE::BSTEventSink<SKSE::ModCallbackEvent>
        {
            RE::BSEventNotifyControl ProcessEvent(const SKSE::ModCallbackEvent* a_event,
                                                  RE::BSTEventSource<SKSE::ModCallbackEvent>* /*src*/) override
            {
                if (!a_event) {
                    logger::trace("MCMEventSink[trace]: null event pointer");
                    return RE::BSEventNotifyControl::kContinue;
                }
                // This sink is registered on the game-wide ModCallback
                // source, so it sees every mod's events. It used to trace
                // all of them, which in a real load order is hundreds of
                // lines about other people's mods and not one of them can
                // say anything about ours.
                //
                // Two questions made that seem worth it, and both survive
                // the filter:
                //
                //   "Did _ne_MCM.psc fire at all, or fire under a wrong
                //    name?" — anything we send starts with `_ne_`, and a
                //    mistyped event name is still ours, so tracing our own
                //    prefix answers it. That ambiguity burned diagnosis
                //    time on the "MCM won't load" report and is the reason
                //    the unfiltered trace existed.
                //
                //   "Is the sink even wired up?" — answered once, by the
                //    first foreign event to arrive, and then dropped. One
                //    line proves the registration took; the next three
                //    hundred prove nothing further.
                const std::string_view eventName{a_event->eventName};
                const bool isOurs = eventName.starts_with(kOurEventPrefix);
                if (!isOurs) {
                    if (!g_loggedForeignEvent.exchange(true, std::memory_order_acq_rel)) {
                        logger::trace("MCMEventSink[trace]: sink is live — first ModCallback from elsewhere was "
                                      "'{}'; further foreign events are not logged",
                                      std::string{eventName});
                    }
                    return RE::BSEventNotifyControl::kContinue;
                }

                const std::string sender = a_event->sender ? std::string{a_event->sender->GetName()} : std::string{};
                logger::trace("MCMEventSink[trace]: ModCallback received: name='{}' strArg='{}' numArg={:.3f} "
                              "sender='{}' (expect name='{}' -> {})",
                              std::string{eventName},
                              std::string{a_event->strArg},
                              a_event->numArg,
                              sender,
                              kEventName,
                              eventName == kEventName ? "MATCH" : "skip");
                if (eventName != kEventName) {
                    return RE::BSEventNotifyControl::kContinue;
                }

                AsyncDispatch::EnqueueWork([](const PluginThread::Token& pt) {
                    MainThread::FireAndForget(pt, [](const MainThread::Token&) {
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
