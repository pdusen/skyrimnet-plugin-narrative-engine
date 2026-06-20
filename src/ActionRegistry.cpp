#include <ActionRegistry.h>

#include <logger.h>

#include <mutex>

namespace NarrativeEngine::ActionRegistry
{
    namespace
    {
        std::mutex                            g_mutex;
        std::vector<std::unique_ptr<IAction>> g_actions;
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

        std::scoped_lock lock(g_mutex);
        for (const auto& existing : g_actions) {
            if (existing && existing->Name() == name) {
                logger::warn("ActionRegistry::Register: '{}' is already registered; ignoring duplicate", name);
                return;
            }
        }
        g_actions.push_back(std::move(action));
        logger::info("ActionRegistry: registered '{}'", name);
    }

    const std::vector<std::unique_ptr<IAction>>& All()
    {
        // The vector itself is stable for the plugin's lifetime; returning a
        // reference is safe. The mutex isn't held across the return because
        // we only ever push_back at startup and never erase — so iteration
        // can't race with a structural change in practice.
        return g_actions;
    }

    IAction* Find(std::string_view name)
    {
        std::scoped_lock lock(g_mutex);
        for (const auto& a : g_actions) {
            if (a && a->Name() == name) {
                return a.get();
            }
        }
        return nullptr;
    }

    std::vector<IAction*> AvailableMatching(const ActionContext& ctx,
                                            ActionPolarity        desired)
    {
        std::vector<IAction*> out;
        std::scoped_lock lock(g_mutex);
        out.reserve(g_actions.size());
        for (const auto& a : g_actions) {
            if (!a) continue;
            const auto pol = a->Polarity();
            const bool polarityFits =
                (pol == ActionPolarity::Either) || (pol == desired);
            if (!polarityFits) continue;
            if (!a->IsAvailable(ctx)) continue;
            out.push_back(a.get());
        }
        return out;
    }
}
