#pragma once

#include <IAction.h>

// NPCLetterAction — the Director's first social lever and its first
// Either-polarity action. Composes a letter via LetterComposer, then
// VM-dispatches to vanilla `WICourierScript.AddItemToContainer` so the
// existing courier system delivers it. Tone and polarity are driven by
// the content the LLM generates; the action itself is one entry point
// for both raise- and lower-direction letters.
//
// Phase 04 Step 11 ships only the skeleton: name / description /
// polarity / IsAvailable. `Start` is a stub that fails immediately;
// Step 13 wires up the real Compose → dispatch → verify chain plus
// the IAction::DetectAndRollbackFailedStart / DetectCompletion polls.
namespace NarrativeEngine
{
    class NPCLetterAction : public IAction
    {
    public:
        std::string    Name()        const override;
        std::string    Description() const override;
        ActionPolarity Polarity()    const override;
        bool           IsAvailable(const ActionContext& ctx) const override;
        StartResult    Start(const ActionContext& ctx, const nlohmann::json& parameters) override;
        bool           DetectAndRollbackFailedStart(const ActionContext& ctx,
                                                    double                secondsSinceStart) override;
        bool           DetectCompletion(const ActionContext& ctx,
                                        double                secondsSinceStart) override;
    };
}
