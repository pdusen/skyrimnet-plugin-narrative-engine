#pragma once

#include <string>
#include <string_view>

#include <RE/Skyrim.h>

#include <PlotThread.h>

// CharacterBios — an NPC's character profile, the one SkyrimNet would render
// for them, whether or not SkyrimNet has registered them yet.
//
// SkyrimNet only hands out a profile through a UUID, and has one only for
// actors it has registered in this save: on a fresh save it knew 88 of the
// 881 people plots draw from. So this works out the answer the way SkyrimNet does --
// the same bio name, the same content layer, the same fallbacks -- from the
// rules decoded out of its DLL:
//
//   docs/engine-findings/skyrimnet-bio-template-name-assignment.md
//   docs/engine-findings/skyrimnet-character-profile-lookup.md
//
// The rules themselves are pure and live in CharacterBioResolver. This
// module gathers their inputs from the engine and the disk, and holds the
// result.
//
// ---------------------------------------------------------------------
// WHO DOES WHAT, ON WHICH THREAD
//
// Engine facts -- names, plugins, the unique flag -- are record data, read
// once per save on the main thread by OnSessionStart and published as one
// immutable snapshot.
//
// Everything else changes during play: SkyrimNet writes dynamic bios, the
// player edits and pins profiles in the dashboard, actors get registered. So the
// plot worker re-resolves every population member at the start of every
// plot tick (Refresh), and OWNS the result. Every reader -- castability,
// the prompt sections, occupations -- runs inside a tick after that tick's
// refresh, so none needs a lock and none can see another save's profiles.
namespace NarrativeEngine::CharacterBios
{
    // One biography, block by block.
    //
    // Kept as separate fields rather than one string because the prompts
    // render them under their own headings -- a scheme prompt wants the
    // occupation set apart from the personality, not both glommed
    // together into a wall of text.
    //
    // `appearance`, `speech_style` and a dynamic bio's `interject_summary`
    // are deliberately absent. They say how somebody looks and sounds,
    // which answers neither "what would this person scheme for" nor "who
    // should carry this out".
    struct Bio
    {
        std::string summary;
        std::string background;
        std::string personality;
        std::string aspirations;
        std::string relationships;
        std::string occupation;
        std::string skills;

        [[nodiscard]] bool Empty() const noexcept;
    };

    // Pull the blocks out of a bio file's text. Every block goes through
    // LLMTextSanitizer: dynamic bios are LLM output, and a dashboard-written
    // one may be too.
    [[nodiscard]] Bio ParseBio(std::string_view file);

    // Main thread, at kNewGame / kPostLoadGame, after
    // PlotPopulation::OnSessionStart. Reads the engine facts for every
    // population member and every generic translation row, and publishes
    // them for the plot worker.
    void OnSessionStart();

    // Main thread, from the serialization revert callback. Drops the
    // published facts, so a tick that runs before the next load resolves
    // nobody rather than the previous save's people.
    void OnRevert();

    // Plot worker, first thing in every tick. Re-reads SkyrimNet's stored
    // bio names, the content registry, every layer and the profile files,
    // and replaces what Has and For answer.
    void Refresh(const PlotThread::Token& pt);

    // Does this NPC have a profile of their OWN -- a dynamic bio or a file
    // written for them? A _generic or default profile does not count: a
    // mastermind who reads like everyone else is what plots exist to avoid.
    [[nodiscard]] bool Has(RE::FormID npc);

    // Keyed on the BASE NPC form, the way the rest of the plugin refers to
    // people. Whatever SkyrimNet would render, generic and default
    // included; an empty Bio for anyone with no profile at all.
    [[nodiscard]] const Bio& For(RE::FormID npc);

} // namespace NarrativeEngine::CharacterBios
