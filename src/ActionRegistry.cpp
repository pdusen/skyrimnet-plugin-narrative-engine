#include <ActionRegistry.h>

#include <Settings.h>
#include <logger.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>

namespace NarrativeEngine::ActionRegistry
{
    namespace
    {
        // Per-registration state. Wrapped in unique_ptr because atomics
        // are non-movable, so we can't hold Entry values directly in a
        // std::vector that may reallocate.
        struct Entry
        {
            std::unique_ptr<IAction> action;
            std::atomic<bool>        enabled{true};
            std::atomic<double>      lastDispatchedRealTime{0.0};
        };

        std::mutex                          g_mutex;
        std::vector<std::unique_ptr<Entry>> g_entries;

        // Seed enabled state from Settings by action name. Runtime toggles
        // don't write back; the INI is the source of truth on each boot.
        bool InitialEnabledFromSettings(const std::string& name)
        {
            const auto& cfg = Settings::Get();
            if (name == "ambush")     return cfg.enableAmbush;
            if (name == "npc_letter") return cfg.enableNpcLetter;
            // Unknown action — default enabled. Any new action added to
            // the registry gets a matching bEnableXxx INI key + Settings
            // wiring; until then, "true" is the safe default.
            return true;
        }

        // Linear-scan lookup; the registry has a handful of entries so
        // this is cheaper than a map. Caller holds g_mutex.
        Entry* FindEntryLocked(std::string_view name)
        {
            for (const auto& e : g_entries) {
                if (e && e->action && e->action->Name() == name) {
                    return e.get();
                }
            }
            return nullptr;
        }

        double NowUnixSeconds()
        {
            return std::chrono::duration<double>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        }
    }

    void Register(std::unique_ptr<IAction> action)
    {
        if (!action) {
            logger::warn("ActionRegistry::Register: refusing null action");
            return;
        }
        const std::string name = action->Name();
        if (name.empty()) {
            logger::warn("ActionRegistry::Register: refusing action with empty Name()");
            return;
        }

        const bool initialEnabled = InitialEnabledFromSettings(name);

        std::scoped_lock lock(g_mutex);
        if (FindEntryLocked(name) != nullptr) {
            logger::warn("ActionRegistry::Register: '{}' is already registered; ignoring duplicate", name);
            return;
        }
        auto entry = std::make_unique<Entry>();
        entry->action = std::move(action);
        entry->enabled.store(initialEnabled, std::memory_order_release);
        g_entries.push_back(std::move(entry));
        logger::info(
            "ActionRegistry: registered '{}' (initial enabled={})",
            name, initialEnabled);
    }

    std::vector<EntryView> All()
    {
        std::vector<EntryView> out;
        std::scoped_lock lock(g_mutex);
        out.reserve(g_entries.size());
        for (const auto& e : g_entries) {
            if (!e || !e->action) continue;
            out.push_back({
                e->action.get(),
                e->action->Name(),
                e->enabled.load(std::memory_order_acquire),
                e->lastDispatchedRealTime.load(std::memory_order_acquire),
            });
        }
        return out;
    }

    IAction* Find(std::string_view name)
    {
        std::scoped_lock lock(g_mutex);
        auto* e = FindEntryLocked(name);
        return e ? e->action.get() : nullptr;
    }

    void SetEnabled(std::string_view name, bool enabled)
    {
        std::scoped_lock lock(g_mutex);
        auto* e = FindEntryLocked(name);
        if (!e) {
            logger::warn(
                "ActionRegistry::SetEnabled: unknown action '{}'",
                std::string{name});
            return;
        }
        const bool prev = e->enabled.exchange(enabled, std::memory_order_acq_rel);
        if (prev != enabled) {
            logger::info(
                "ActionRegistry: '{}' enabled -> {}",
                std::string{name}, enabled);
        }
    }

    bool IsEnabled(std::string_view name)
    {
        std::scoped_lock lock(g_mutex);
        auto* e = FindEntryLocked(name);
        if (!e) return false;
        return e->enabled.load(std::memory_order_acquire);
    }

    void MarkDispatched(std::string_view name)
    {
        std::scoped_lock lock(g_mutex);
        auto* e = FindEntryLocked(name);
        if (!e) return;
        e->lastDispatchedRealTime.store(NowUnixSeconds(), std::memory_order_release);
    }

    double LastDispatchedRealTime(std::string_view name)
    {
        std::scoped_lock lock(g_mutex);
        auto* e = FindEntryLocked(name);
        if (!e) return 0.0;
        return e->lastDispatchedRealTime.load(std::memory_order_acquire);
    }

    std::vector<IAction*> AvailableMatching(const ActionContext& ctx,
                                            ActionPolarity        desired)
    {
        std::vector<IAction*> out;
        std::scoped_lock lock(g_mutex);
        out.reserve(g_entries.size());
        for (const auto& e : g_entries) {
            if (!e || !e->action) continue;
            if (!e->enabled.load(std::memory_order_acquire)) continue;
            const auto pol = e->action->Polarity();
            const bool polarityFits =
                (pol == ActionPolarity::Either) || (pol == desired);
            if (!polarityFits) continue;
            if (!e->action->IsAvailable(ctx)) continue;
            out.push_back(e->action.get());
        }
        return out;
    }
}
