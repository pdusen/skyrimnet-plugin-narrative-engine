#include <BeatRegistry.h>

#include <logger.h>
#include <Settings.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>

namespace NarrativeEngine::BeatRegistry
{
    namespace
    {
        // Per-registration state. Wrapped in unique_ptr because atomics
        // are non-movable, so we can't hold Entry values directly in a
        // std::vector that may reallocate.
        struct Entry
        {
            std::unique_ptr<IBeat> beat;
            std::atomic<bool> enabled{true};
            std::atomic<double> lastDispatchedRealTime{0.0};
        };

        std::mutex g_mutex;
        std::vector<std::unique_ptr<Entry>> g_entries;

        // Seed enabled state from Settings by beat name. Runtime toggles
        // don't write back; the INI is the source of truth on each boot.
        bool InitialEnabledFromSettings(const std::string& name)
        {
            const auto& cfg = Settings::Get();
            if (name == "ambush")
                return cfg.enableAmbush;
            if (name == "npc_letter")
                return cfg.enableNpcLetter;
            if (name == "npc_visit")
                return cfg.enableNpcVisit;
            // Unknown beat — default enabled. Any new beat added to the
            // registry gets a matching bEnableXxx INI key + Settings
            // wiring; until then, "true" is the safe default.
            return true;
        }

        // Linear-scan lookup; the registry has a handful of entries so
        // this is cheaper than a map. Caller holds g_mutex.
        Entry* FindEntryLocked(std::string_view name)
        {
            for (const auto& e : g_entries) {
                if (e && e->beat && e->beat->Name() == name) {
                    return e.get();
                }
            }
            return nullptr;
        }

        double NowUnixSeconds()
        {
            return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
        }
    } // namespace

    void Initialize()
    {
        std::scoped_lock lock(g_mutex);
        logger::info("BeatRegistry: registered {} beats", g_entries.size());
    }

    void Register(std::unique_ptr<IBeat> beat)
    {
        if (!beat) {
            logger::warn("BeatRegistry::Register: refusing null beat");
            return;
        }
        const std::string name = beat->Name();
        if (name.empty()) {
            logger::warn("BeatRegistry::Register: refusing beat with empty Name()");
            return;
        }

        const bool initialEnabled = InitialEnabledFromSettings(name);

        std::scoped_lock lock(g_mutex);
        if (FindEntryLocked(name) != nullptr) {
            logger::warn("BeatRegistry::Register: '{}' is already registered; ignoring duplicate", name);
            return;
        }
        auto entry = std::make_unique<Entry>();
        entry->beat = std::move(beat);
        entry->enabled.store(initialEnabled, std::memory_order_release);
        g_entries.push_back(std::move(entry));
        logger::info("BeatRegistry: registered '{}' (initial enabled={})", name, initialEnabled);
    }

    std::vector<EntryView> All()
    {
        std::vector<EntryView> out;
        std::scoped_lock lock(g_mutex);
        out.reserve(g_entries.size());
        for (const auto& e : g_entries) {
            if (!e || !e->beat)
                continue;
            out.push_back({
                e->beat.get(),
                e->beat->Name(),
                e->enabled.load(std::memory_order_acquire),
                e->lastDispatchedRealTime.load(std::memory_order_acquire),
            });
        }
        return out;
    }

    IBeat* Find(std::string_view name)
    {
        std::scoped_lock lock(g_mutex);
        auto* e = FindEntryLocked(name);
        return e ? e->beat.get() : nullptr;
    }

    void SetEnabled(std::string_view name, bool enabled)
    {
        std::scoped_lock lock(g_mutex);
        auto* e = FindEntryLocked(name);
        if (!e) {
            logger::warn("BeatRegistry::SetEnabled: unknown beat '{}'", std::string{name});
            return;
        }
        const bool prev = e->enabled.exchange(enabled, std::memory_order_acq_rel);
        if (prev != enabled) {
            logger::info("BeatRegistry: '{}' enabled -> {}", std::string{name}, enabled);
        }
    }

    bool IsEnabled(std::string_view name)
    {
        std::scoped_lock lock(g_mutex);
        auto* e = FindEntryLocked(name);
        if (!e)
            return false;
        return e->enabled.load(std::memory_order_acquire);
    }

    void MarkDispatched(std::string_view name)
    {
        std::scoped_lock lock(g_mutex);
        auto* e = FindEntryLocked(name);
        if (!e)
            return;
        e->lastDispatchedRealTime.store(NowUnixSeconds(), std::memory_order_release);
    }

    double LastDispatchedRealTime(std::string_view name)
    {
        std::scoped_lock lock(g_mutex);
        auto* e = FindEntryLocked(name);
        if (!e)
            return 0.0;
        return e->lastDispatchedRealTime.load(std::memory_order_acquire);
    }

    std::vector<IBeat*> AvailableMatching(const BeatContext& ctx, BeatPolarity desired)
    {
        std::vector<IBeat*> out;
        std::scoped_lock lock(g_mutex);
        out.reserve(g_entries.size());
        for (const auto& e : g_entries) {
            if (!e || !e->beat)
                continue;
            if (!e->enabled.load(std::memory_order_acquire))
                continue;
            const auto pol = e->beat->Polarity();
            const bool polarityFits = (pol == BeatPolarity::Either) || (pol == desired);
            if (!polarityFits)
                continue;
            if (!e->beat->IsAvailable(ctx))
                continue;
            out.push_back(e->beat.get());
        }
        return out;
    }
} // namespace NarrativeEngine::BeatRegistry
