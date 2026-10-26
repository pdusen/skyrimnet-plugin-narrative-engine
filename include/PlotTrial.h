#pragma once

#include <LanguageTable.h>
#include <PlotBirth.h>
#include <PlotCasting.h>
#include <PlotModel.h>
#include <PlotRejection.h>
#include <PlotState.h>
#include <PlotThread.h>
#include <PluginThread.h>

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// The language trial: plot birth run a fixed number of times under each of a
// few languages, inside an ordinary session, to measure how well each draft's
// rules hold against the real model. The session's language never changes.
//
// Each run reads everything plot birth reads from a language table -- the
// output-language line, [Prompt.Form], [Matching] and the name matchers --
// from that language's shipped table (Localization::LoadShipped). Nothing a
// trial produces is kept. A trial birth draws its shortlist from the live
// state's occupancy with an RNG of its own, and its plot is discarded: no plot
// is added, no occupancy row engaged, no budget spent, and the live state's
// RNG does not move. See "The language trial" in
// docs/implementation/PHASE_18_INTERNATIONALIZATION.md.
namespace NarrativeEngine::PlotTrial
{
    // English is the baseline; German, Russian and Japanese between them
    // exercise every rule shape in [Matching].
    inline constexpr std::array<std::string_view, 4> kLanguages = {"english", "german", "russian", "japanese"};
    inline constexpr int kBirthsPerLanguage = 8;

    // About what a whole trial costs, for the button to say before it runs:
    // two chat calls and a few decisions a birth, a retry here and there.
    inline constexpr int kApproximateCalls = 100;

    // The [Matching] check, or matcher, behind a refusal: "Scheme",
    // "PlayerTitle" or "names". Empty for a refusal no language rule decides.
    [[nodiscard]] std::string_view CheckOf(PlotRejection::Code code) noexcept;

    struct Birth
    {
        bool accepted = false;
        // Filled in when accepted, and never kept.
        PlotModel::Plot plot;
        std::vector<PlotBirth::Refusal> refusals;
        // Why nothing was composed at all: nobody eligible, or nobody
        // SkyrimNet could describe. Empty when composing was tried.
        std::string skipped;
    };

    // Who a birth may draw, as PlotTick's birth asks it: alive, fit to
    // mastermind, seated where a roster gates office, and describable by
    // SkyrimNet.
    struct Predicates
    {
        PlotCasting::AlivePredicate alive;
        PlotCasting::AlivePredicate viable;
        PlotCasting::SeatedPredicate seated;
        std::function<bool(RE::FormID)> castable;
    };

    // The live game's: PlotPopulation's, and PlotBirth::IsCastable.
    [[nodiscard]] Predicates LivePredicates();

    // One trial birth under `table`. Reads `live`, which it cannot change;
    // draws from `rng`, the trial's own.
    [[nodiscard]] Birth TrialBirth(const PlotThread::Token& pt,
                                   const PlotCasting::Population& population,
                                   const PlotState& live,
                                   std::uint64_t& rng,
                                   const LanguageTable::Table& table,
                                   const Predicates& predicates);

    struct LanguageResult
    {
        std::string stem;
        // The table's names for its language: in English, for the log, and in
        // itself, for the dashboard.
        std::string name;
        std::string nativeName;
        // False when the language's file could not be read; nothing ran.
        bool loaded = false;
        int attempted = 0;
        int accepted = 0;
        // Every refused answer, a retried one included: by code
        // ("scheme_not_verb"; "steps" for planning that failed), and by the
        // check behind it.
        std::map<std::string, int> byCode;
        std::map<std::string, int> byCheck;
        int masterminds = 0;
        // Up to three accepted plots, each as its mastermind's memory of
        // doing its first step themselves, in this language.
        std::vector<std::string> memories;
    };

    struct Snapshot
    {
        bool running = false;
        bool ran = false;
        int done = 0;
        int total = 0;
        std::vector<LanguageResult> languages;
    };

    // Queue a trial on the plot worker, unless one is running. It holds the
    // worker for as long as its calls take; plot ticks wait behind it.
    void Start(const PluginThread::Token& pt);

    // The running or last trial, for the dashboard. Any thread.
    [[nodiscard]] Snapshot Get();
} // namespace NarrativeEngine::PlotTrial
