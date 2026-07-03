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

    namespace NPCLetterAction_Init
    {
        // Resolve the 20 `_ne_PooledLetterQuestNN` EditorIDs into the
        // per-slot delivery-quest cache, and warm the vanilla WICourier /
        // courier-container resolution so the verification polls don't
        // have to do the lookup lazily on first use.
        //
        // Must run AFTER LetterPool::Initialize at kDataLoaded, because
        // the per-slot quest array is keyed against LetterPool slot
        // indices.
        //
        // Idempotent — second call rewires nothing and is cheap.
        void Initialize();
    }
}
