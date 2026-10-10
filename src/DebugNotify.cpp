#include <DebugNotify.h>

#include <AsyncDispatch.h>
#include <logger.h>
#include <MainThread.h>
#include <Settings.h>

#include <RE/M/Misc.h>

#include <format>
#include <utility>

namespace NarrativeEngine::DebugNotify
{
    void Post(std::string text)
    {
        if (text.empty()) {
            return;
        }
        // Checked here as well as on arrival: when the setting is off
        // this is the common case, and there is no reason to queue work
        // across two threads only to drop it.
        if (!Settings::Get().debugNotifications) {
            return;
        }

        AsyncDispatch::EnqueueWork([text = std::move(text)](const PluginThread::Token& pt) mutable {
            MainThread::FireAndForget(pt, [text = std::move(text)](const MainThread::Token&) {
                // Re-checked on arrival. A post queued a moment before
                // the dashboard toggle went off should not still appear.
                if (!Settings::Get().debugNotifications) {
                    return;
                }
                logger::debug("DebugNotify: {}", text);
                RE::DebugNotification(text.c_str());
            });
        });
    }

    void PostActorNamed(RE::FormID actor, std::string pattern)
    {
        if (!Settings::Get().debugNotifications) {
            return;
        }

        AsyncDispatch::EnqueueWork([actor, pattern = std::move(pattern)](const PluginThread::Token& pt) mutable {
            MainThread::FireAndForget(pt, [actor, pattern = std::move(pattern)](const MainThread::Token&) {
                if (!Settings::Get().debugNotifications) {
                    return;
                }
                const char* name = nullptr;
                if (auto* form = RE::TESForm::LookupByID(actor)) {
                    name = form->GetName();
                }
                const std::string resolved = (name && *name) ? name : "someone";
                std::string text;
                try {
                    text = std::vformat(pattern, std::make_format_args(resolved));
                } catch (const std::format_error&) {
                    // A pattern with the wrong placeholders is a coding
                    // slip, not a reason to lose the notice entirely.
                    logger::warn("DebugNotify: bad pattern '{}'", pattern);
                    text = pattern;
                }
                logger::debug("DebugNotify: {}", text);
                RE::DebugNotification(text.c_str());
            });
        });
    }
} // namespace NarrativeEngine::DebugNotify
