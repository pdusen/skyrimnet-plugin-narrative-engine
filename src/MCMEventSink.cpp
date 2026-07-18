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
                    return RE::BSEventNotifyControl::kContinue;
                }
                if (a_event->eventName != kEventName) {
                    return RE::BSEventNotifyControl::kContinue;
                }

                AsyncDispatch::MarshalToMainThread([] {
                    Settings::ApplyMcmOverride();
                    const auto& cfg = Settings::Get();
                    logger::info("MCMEventSink: dashboard hotkey rebound DXSC={} mods={}",
                                 cfg.dashboardHotkeyDXSC,
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
            return;
        }

        auto* source = SKSE::GetModCallbackEventSource();
        if (!source) {
            logger::error("MCMEventSink: ModCallbackEventSource unavailable");
            g_registered.store(false);
            return;
        }
        source->AddEventSink<SKSE::ModCallbackEvent>(&g_sink);
        logger::info("MCMEventSink: initialized (listening for {})", kEventName);
    }
} // namespace NarrativeEngine::MCMEventSink
