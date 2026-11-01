#pragma once

namespace RE
{
    class TESForm;
} // namespace RE

// A form's EditorID, read from wherever the running game keeps it.
//
// Asking the form is not enough. The engine discards the EditorIDs of most
// record types at load -- factions, locations, keywords, NPCs -- and the mods
// that restore them do not all put them back on the form. powerofthree's
// Tweaks and Native EditorID Fix (in its default mode) restore them only to the
// engine's EditorID-to-form table: TESForm::LookupByEditorID finds GovRuling,
// while GovRuling's own GetFormEditorID() still answers "". Every faction then
// reads as unnamed, which is how gossip came to admit no faction at all.
//
// So the table is inverted once, and a name the form will not give is read back
// from it. Forms that do keep their own EditorID (worldspaces, quests, cells
// under Tweaks) answer directly, so routing every read through here costs them
// nothing.
//
// Token-free, as EngineUtils::IsGhostRecord is: the callers include the gossip
// and plot workers, which hold no token to pass, and both reads are ones the
// plugin already makes off the main thread.
namespace NarrativeEngine::EditorIds
{
    // Inverts the engine's EditorID table. Call once at kDataLoaded, before
    // anything asks for a name: the table is complete by then, and until this
    // runs Of() can only report what a form stores itself.
    void Initialize();

    // The form's EditorID, or "" when it has none. Never null; "" for a null
    // form too.
    const char* Of(const RE::TESForm* form);
} // namespace NarrativeEngine::EditorIds
