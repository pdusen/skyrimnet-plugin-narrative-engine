#include <Decorators.h>

#include <DecisionLog.h>
#include <logger.h>
#include <PhaseTracker.h>
#include <SkyrimNetAPI.h>

#include <functional>
#include <string>

namespace NarrativeEngine::Decorators
{
    namespace
    {
        constexpr const char* kTensionName = "ne_narrative_tension";
        constexpr const char* kPhaseName = "ne_narrative_phase";

        constexpr const char* kTensionDescription = "NarrativeEngine: most recent Director tension score, "
                                                    "rendered as a decimal string in the range 0..100. "
                                                    "Pre-first-evaluation default is 0. Ignores the actor argument.";

        constexpr const char* kPhaseDescription = "NarrativeEngine: current Freytag-pyramid phase name "
                                                  "(Exposition | RisingAction | Climax | FallingAction | Resolution). "
                                                  "Ignores the actor argument.";

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
    }
} // namespace NarrativeEngine::Decorators
