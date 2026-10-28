#include <Decorators.h>

#include <DecisionLog.h>
#include <logger.h>
#include <PhaseTracker.h>
#include <PlotAwareness.h>
#include <PlotState.h>
#include <SkyrimNetAPI.h>

#include <functional>
#include <string>

namespace NarrativeEngine::Decorators
{
    namespace
    {
        constexpr const char* kTensionName = "ne_narrative_tension";
        constexpr const char* kPhaseName = "ne_narrative_phase";
        constexpr const char* kPlotAwarenessName = "ne_plot_awareness";

        constexpr const char* kTensionDescription = "NarrativeEngine: most recent Director tension score, "
                                                    "rendered as a decimal string in the range 0..100. "
                                                    "Pre-first-evaluation default is 0. Ignores the actor argument.";

        constexpr const char* kPhaseDescription = "NarrativeEngine: current Freytag-pyramid phase name "
                                                  "(Exposition | RisingAction | Climax | FallingAction | Resolution). "
                                                  "Ignores the actor argument.";

        constexpr const char* kPlotAwarenessDescription =
            "NarrativeEngine: the plot this actor is in, in the second person: as its mastermind, "
            "the scheme and every step of it; as an agent, the errand they were sent on. "
            "Empty for an actor in no plot.";

        std::string TensionCallback(RE::Actor* /*ignored*/)
        {
            // Pre-first-evaluation default is 0 (calm). After that, the most
            // recent decision's score. Collapsing the optional here matches
            // the design decision in PHASE_01_MVP.md Step 12: the prompt
            // template's mood-mapping table should always have a numeric
            // value to interpret, never an empty string.
            return std::to_string(DecisionLog::LatestTensionScore().value_or(0));
        }

        std::string PhaseCallback(RE::Actor* /*ignored*/)
        {
            return std::string{PhaseTracker::PhaseName(PhaseTracker::Get())};
        }

        // Called on SkyrimNet's render thread, which owns nothing of ours: the
        // published snapshot is the one piece of plot state any thread may
        // read. The actor's base is a field read, the same one SkyrimNet makes
        // to render the rest of their prompt.
        std::string PlotAwarenessCallback(RE::Actor* actor)
        {
            const auto* base = actor != nullptr ? actor->GetActorBase() : nullptr;
            return base != nullptr ? PlotAwareness::Describe(*Plots::Snapshot(), base->GetFormID()) : std::string{};
        }

        void RegisterOne(const std::string& name,
                         const std::string& description,
                         std::function<std::string(RE::Actor*)> callback)
        {
            if (!SkyrimNetAPI::IsAvailable()) {
                logger::warn("Decorators::Register: SkyrimNet unavailable; skipping '{}'", name);
                return;
            }
            if (SkyrimNetAPI::HasDecorator(name)) {
                logger::warn("Decorators::Register: '{}' is already registered "
                             "(SkyrimNet built-in or another plugin); registration may fail",
                             name);
            }
            if (SkyrimNetAPI::RegisterDecorator(name, description, std::move(callback))) {
                logger::info("Decorators: registered {}", name);
            } else {
                logger::error("Decorators: failed to register {}", name);
            }
        }
    } // namespace

    void Register()
    {
        RegisterOne(kTensionName, kTensionDescription, &TensionCallback);
        RegisterOne(kPhaseName, kPhaseDescription, &PhaseCallback);
        RegisterOne(kPlotAwarenessName, kPlotAwarenessDescription, &PlotAwarenessCallback);
    }
} // namespace NarrativeEngine::Decorators
