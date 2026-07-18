#include <MCMEventSink.h>

#include <AsyncDispatch.h>
#include <logger.h>
#include <Settings.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <string>

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
                if (!a_event)
                    return RE::BSEventNotifyControl::kContinue;

                if (a_event->eventName != kEventName)
                    return RE::BSEventNotifyControl::kContinue;

                // Payload packing (mirrors _ne_MCM.psc::SendHotkeyChangedEvent):
                //   numArg = DXSC (float, exact for small ints)
                //   strArg = modifiers bitmask formatted as decimal string
                const int dxsc = static_cast<int>(a_event->numArg);
                const std::string modsStr(a_event->strArg.c_str() ? a_event->strArg.c_str() : "");
                const int mods = modsStr.empty() ? 0 : std::atoi(modsStr.c_str());

                AsyncDispatch::MarshalToMainThread([dxsc, mods] {
                    Settings::UpdateDashboardHotkey(dxsc, static_cast<std::uint8_t>(mods));
                    logger::info("MCMEventSink: dashboard hotkey rebound DXSC={} mods={}", dxsc, mods);
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
        if (!g_registered.compare_exchange_strong(expected, true))
            return;

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
