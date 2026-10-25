#pragma once

#include <LanguageTable.h>
#include <PlotModel.h>
#include <PlotVerbs.h>
#include <TextKeys.h>
#include <TextTemplate.h>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

// The plot sentences a player can read, rendered from a language table.
//
// Every memory a step writes, what each person a step reached remembers of
// it, the mastermind's last word on the plot, the dashboard's line and
// caption for a step, and the phrase a made item is named by. The words live
// in the language files' [Plot.*] sections; this module says which key each
// one is and what fills it.
//
// What the prompts read about a verb -- its menu line, the plan sketch, the
// task phrases -- is not here: that is still PlotVerbs' C++ text, until it
// moves into a prompt include of its own.
namespace NarrativeEngine::PlotText
{
    // The sex of a person in a step, by base form, for templates that agree
    // with it. Unknown people read as Sex::Other.
    using SexOf = std::function<TextTemplate::Sex(RE::FormID npc)>;

    // How a plot as a whole ended, for the mastermind's last memory of it;
    // and the clause a failed step's memory ends with when the plot goes on.
    enum class Outcome
    {
        Succeeded,
        Failed,
        Conceded,
        WentOn,
    };

    // Every key this module renders, generated from the verb table.
    [[nodiscard]] const std::vector<TextKeys::Key>& Keys();

    [[nodiscard]] const TextKeys::Key& MemoryKey(const PlotVerbs::Verb& verb, PlotVerbs::Template t);
    [[nodiscard]] const TextKeys::Key& RememberedKey(const PlotVerbs::Verb& verb,
                                                     const PlotVerbs::Slot& slot,
                                                     PlotVerbs::TargetTemplate t);
    [[nodiscard]] const TextKeys::Key& LineKey(const PlotVerbs::Verb& verb);
    [[nodiscard]] const TextKeys::Key& CaptionKey(const PlotVerbs::Verb& verb);
    [[nodiscard]] const TextKeys::Key& OutcomeKey(Outcome outcome);

    // A step's memory in the voice `t` is written for: the agent's, the
    // mastermind's, or theirs alone when they did it themselves.
    [[nodiscard]] std::string Memory(const LanguageTable::Table& table,
                                     const PlotModel::Step& step,
                                     PlotVerbs::Template t,
                                     std::string_view scheme,
                                     const SexOf& sexOf = {});

    // What the person in `slot` remembers of the step: they caught it, it was
    // done to them openly, or they noticed what it left behind.
    [[nodiscard]] std::string Remembered(const LanguageTable::Table& table,
                                         const PlotModel::Step& step,
                                         std::string_view slot,
                                         PlotVerbs::TargetTemplate t,
                                         const SexOf& sexOf = {});

    // The people `slot`'s memory names, by placeholder: "agent",
    // "mastermind", or another slot. Who a memory relates is who it names.
    [[nodiscard]] std::vector<std::string> RememberedNames(const LanguageTable::Table& table,
                                                           const PlotModel::Step& step,
                                                           std::string_view slot,
                                                           PlotVerbs::TargetTemplate t);

    [[nodiscard]] std::string PlotOutcome(const LanguageTable::Table& table, Outcome outcome, std::string_view scheme);

    // The dashboard's sentence and caption for a step. A step with no verb
    // (a debug plot's, or one from before verbs existed) keeps its own text.
    [[nodiscard]] std::string DashboardLine(const LanguageTable::Table& table, const PlotModel::Step& step);
    [[nodiscard]] std::string DashboardCaption(const LanguageTable::Table& table, const PlotModel::Step& step);

    // The words of the plots the dashboard seeds for testing, and of the
    // hand-seeded debug plot. They are saved and written to memories like
    // any other plot's, so they are the language file's too.
    enum class TestLine
    {
        BrelynaAmbition,
        BrelynaScheme,
        BrelynaFinish,
        TolfdirAmbition,
        TolfdirScheme,
        TolfdirFinish,
        College,
        Journal,
        DebugMastermind,
        DebugAmbition,
        DebugScheme,
        DebugLocate,
        DebugSurveil,
        DebugAcquire,
    };

    [[nodiscard]] std::string TestText(const LanguageTable::Table& table, TestLine line);

    // Whom a step is done before when it is done before a whole community
    // rather than a person: "the people of Whiterun", or, with no settlement,
    // "the people they lead". What an exposure of a top official is shown to.
    [[nodiscard]] std::string Audience(const LanguageTable::Table& table, std::string_view settlement);

    // What a made item is called in a step: "an Iron Sword", "a sheaf of
    // Elven Arrows". Empty for an empty name.
    [[nodiscard]] std::string ItemPhrase(const LanguageTable::Table& table, std::string_view name, bool ammo);
} // namespace NarrativeEngine::PlotText
