#include <RunChecks.h>

#include <DashboardUIManager.h>
#include <DebugNotify.h>
#include <LLMTextSanitizer.h>
#include <Localization.h>
#include <logger.h>
#include <MainThread.h>
#include <SkyrimNetAPI.h>

#include <RE/M/Misc.h>

#include <mutex>
#include <optional>
#include <random>

namespace NarrativeEngine::RunChecks
{
    namespace
    {
        std::mutex g_mutex;
        Snapshot g_snapshot;
        // The running check's sentinels, while its reply is awaited.
        std::optional<RenderCheck::Sentinels> g_sentinels;

        // The Director's model: the cheapest, and the check needs no voice.
        constexpr const char* kVariant = "narrative_engine_director";

        void Finish(const PluginThread::Token&,
                    Snapshot::Failure failure,
                    std::string detail,
                    const RenderCheck::Result& result)
        {
            {
                const std::lock_guard lock{g_mutex};
                g_snapshot.running = false;
                g_snapshot.ran = true;
                g_snapshot.failure = failure;
                g_snapshot.detail = std::move(detail);
                g_snapshot.result = result;
                g_sentinels.reset();
            }
            logger::info("RunChecks: render check: computed name {}, decorator field {}, include variables {}{}",
                         result.file ? "rendered" : "MISSING",
                         result.decorator ? "rendered" : "MISSING",
                         result.include ? "rendered" : "MISSING",
                         failure == Snapshot::Failure::None ? "" : " (SkyrimNet reported a failure)");
            DashboardUIManager::PushFullState();
        }
    } // namespace

    void Start(const PluginThread::Token& pt)
    {
        RenderCheck::Sentinels sentinels;
        {
            const std::lock_guard lock{g_mutex};
            if (g_snapshot.running) {
                return;
            }
            sentinels =
                RenderCheck::Make((static_cast<std::uint64_t>(std::random_device{}()) << 32) | std::random_device{}());
            g_sentinels = sentinels;
            g_snapshot = {};
            g_snapshot.running = true;
        }

        // Shown whatever the debug-notification setting: the button asked for it.
        MainThread::FireAndForget(pt, [](const MainThread::Token&) {
            const auto text = DebugNotify::Text::FontSample(Localization::Get());
            logger::info("RunChecks: font sample queued: {}", text);
            RE::DebugNotification(text.c_str());
        });

        const bool queued = SkyrimNetAPI::SendCustomPromptToLLM(
            RenderCheck::kTemplate,
            kVariant,
            RenderCheck::Context(sentinels),
            [sentinels](const PluginThread::Token& callbackPt, std::string response, bool success) {
                if (!success) {
                    auto detail = LLMTextSanitizer::Sanitize(response);
                    LLMTextSanitizer::TruncateUTF8(detail, 300);
                    Finish(callbackPt, Snapshot::Failure::Reported, std::move(detail), {});
                    return;
                }
                const auto reply = LLMTextSanitizer::Sanitize(response);
                Finish(callbackPt, Snapshot::Failure::None, {}, RenderCheck::Evaluate(reply, sentinels));
            });
        if (!queued) {
            Finish(pt, Snapshot::Failure::NotQueued, {}, {});
        }
    }

    Snapshot Get()
    {
        const std::lock_guard lock{g_mutex};
        return g_snapshot;
    }

    std::string DecoratorRecord()
    {
        const std::lock_guard lock{g_mutex};
        return g_sentinels ? RenderCheck::DecoratorRecord(*g_sentinels) : std::string{};
    }
} // namespace NarrativeEngine::RunChecks
