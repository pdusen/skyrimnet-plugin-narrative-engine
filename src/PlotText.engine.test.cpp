#include <PlotMemories.h>
#include <PlotModel.h>
#include <PlotText.h>
#include <PlotVerbs.h>

#include <ShippedLanguage.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <vector>

// Characterization of every plot sentence a player can read: the memories each
// verb writes, the dashboard's line and caption, and the recipe phrases that
// become slot text.
//
// Phase 18 Step 5 moved all of it out of C++ into english.ini. This test was
// written first and pinned against the code before the move, so the digest
// below is what the C++ templates produced; the move had to leave it unchanged.
// Every rendering goes to a transcript in the temp directory, so a mismatch can
// be diffed rather than guessed at.

namespace
{
    namespace PM = NarrativeEngine::PlotModel;
    namespace PV = NarrativeEngine::PlotVerbs;
    namespace PMem = NarrativeEngine::PlotMemories;
    namespace PT = NarrativeEngine::PlotText;
    using NarrativeEngine::Testing::ShippedEnglish;

    constexpr RE::FormID kMastermind = 0x00010100;
    constexpr RE::FormID kAgent = 0x00010300;

    // The renderers under test, behind one seam so the cases below are the
    // same before and after the text moved.
    std::vector<PMem::Memory> Dispatch(const PM::Plot& plot, const PM::Step& step)
    {
        return PMem::OnDispatch(ShippedEnglish(), plot, step);
    }
    std::vector<PMem::Memory> Resolve(const PM::Plot& plot, const PM::Step& step)
    {
        return PMem::OnResolve(ShippedEnglish(), plot, step);
    }
    std::vector<PMem::Memory> Answered(const PM::Plot& plot, const PM::Step& step, bool wentOn)
    {
        return PMem::OnFailureAnswered(ShippedEnglish(), plot, step, wentOn);
    }
    std::vector<PMem::Memory> Succeeded(const PM::Plot& plot)
    {
        return PMem::OnSucceeded(ShippedEnglish(), plot);
    }
    std::vector<PMem::Memory> FinishFailed(const PM::Plot& plot)
    {
        return PMem::OnFinishFailed(ShippedEnglish(), plot);
    }
    std::vector<PMem::Memory> Conceded(const PM::Plot& plot)
    {
        return PMem::OnConceded(ShippedEnglish(), plot);
    }
    std::string DashboardLine(const PM::Step& step)
    {
        return PT::DashboardLine(ShippedEnglish(), step);
    }
    std::string DashboardCaption(const PM::Step& step)
    {
        return PT::DashboardCaption(ShippedEnglish(), step);
    }
    std::string ItemPhrase(std::string_view name, bool ammo)
    {
        return PT::ItemPhrase(ShippedEnglish(), name, ammo);
    }

    PM::Plot MakePlot(std::string scheme)
    {
        PM::Plot plot;
        plot.mastermind = kMastermind;
        plot.mastermindName = "Maven Black-Briar";
        plot.scheme = std::move(scheme);
        return plot;
    }

    // `fill`: 0 none of the verb's slots, 1 the first only, 2 all.
    PM::Step MakeStep(const PV::Verb& verb, int fill, bool ownHand)
    {
        PM::Step step;
        step.verb = std::string{verb.id};
        step.actor = ownHand ? kMastermind : kAgent;
        step.actorName = ownHand ? "Maven Black-Briar" : "Mercer Frey";
        step.slots.push_back({"mastermind", kMastermind, "Maven Black-Briar"});
        for (std::size_t i = 0; i < verb.slotCount; ++i) {
            if (fill == 2 || (fill == 1 && i == 0)) {
                const auto& slot = verb.slots[i];
                const RE::FormID form = slot.kind == PV::SlotKind::Person ? 0x00010400 + static_cast<RE::FormID>(i) : 0;
                step.slots.push_back({std::string{slot.name}, form, std::format("The {}", slot.name)});
            }
        }
        return step;
    }

    std::string Line(std::string_view label, const PMem::Memory& m)
    {
        std::string related;
        for (const auto npc : m.related) {
            related += std::format("{:X},", npc);
        }
        return std::format(
            "{}|{:X}|{}|{:.2f}|{}|{}|{}|{}", label, m.npc, m.source, m.importance, m.type, m.location, related, m.text);
    }

    std::uint64_t Fnv1a(std::string_view text)
    {
        std::uint64_t hash = 0xcbf29ce484222325ull;
        for (const char c : text) {
            hash ^= static_cast<unsigned char>(c);
            hash *= 0x100000001b3ull;
        }
        return hash;
    }
} // namespace

TEST_CASE("PlotText: every plot sentence renders as it did before the move", "[PlotText][engine]")
{
    std::vector<std::string> lines;
    const auto add = [&](std::string_view label, const std::vector<PMem::Memory>& memories) {
        for (const auto& m : memories) {
            lines.push_back(Line(label, m));
        }
    };

    for (const std::string_view scheme :
         {std::string_view{"Ruin the Battle-Borns, then take the meadery. "}, std::string_view{}}) {
        const auto plot = MakePlot(std::string{scheme});
        const auto schemeLabel = scheme.empty() ? "noscheme" : "scheme";
        for (const auto& verb : PV::kVerbs) {
            for (int fill = 0; fill < 3; ++fill) {
                for (const bool ownHand : {false, true}) {
                    const auto label =
                        std::format("{}/{}/fill{}/{}", verb.id, schemeLabel, fill, ownHand ? "own" : "sent");
                    auto step = MakeStep(verb, fill, ownHand);
                    lines.push_back(std::format("{}|line|{}", label, DashboardLine(step)));
                    lines.push_back(std::format("{}|caption|{}", label, DashboardCaption(step)));
                    add(label + "/dispatch", Dispatch(plot, step));
                    for (const auto outcome :
                         {PM::StepOutcome::Succeeded, PM::StepOutcome::FailedCaught, PM::StepOutcome::FailedTimeout}) {
                        step.outcome = outcome;
                        const auto outcomeLabel = std::format("{}/{}", label, PM::StepOutcomeId(outcome));
                        add(outcomeLabel + "/resolve", Resolve(plot, step));
                        add(outcomeLabel + "/answered", Answered(plot, step, false));
                        add(outcomeLabel + "/wenton", Answered(plot, step, true));
                    }
                }
            }
        }
        add(std::string{schemeLabel} + "/succeeded", Succeeded(plot));
        add(std::string{schemeLabel} + "/finishfailed", FinishFailed(plot));
        add(std::string{schemeLabel} + "/conceded", Conceded(plot));
    }
    for (const auto& [name, ammo] : std::vector<std::pair<std::string, bool>>{
             {"Iron Sword", false},
             {"Elven Arrow", true},
             {"apple pie", false},
             {"Orcish Bow", false},
             {"Ebony Dagger", false},
             {"Uncle's Ring", false},
             {"", false},
             {"", true},
         }) {
        lines.push_back(std::format("item|{}|{}|{}", name, ammo, ItemPhrase(name, ammo)));
    }

    std::string transcript;
    for (const auto& line : lines) {
        transcript += line;
        transcript += '\n';
    }
    const auto path = std::filesystem::temp_directory_path() / "NarrativeEngine-PlotText-characterization.txt";
    std::ofstream{path, std::ios::binary} << transcript;

    INFO("transcript: " << path.string());
    CHECK(lines.size() == 3110);
    CHECK(Fnv1a(transcript) == 0xc2fc18bbda4005bcull);
}

TEST_CASE("PlotText: the prompts word every verb and every role", "[PlotText][engine]")
{
    // The prompts word a step themselves, in plot_step.prompt, and what each
    // role of a verb is for in plot_role.prompt, one branch per verb or
    // role. A verb added to PlotVerbs without a branch would reach the model
    // as nothing, so it fails here instead.
    const auto step = NarrativeEngine::Testing::ShippedPrompt("components/narrative_engine/plot_step");
    const auto role = NarrativeEngine::Testing::ShippedPrompt("components/narrative_engine/plot_role");
    REQUIRE_FALSE(step.empty());
    REQUIRE_FALSE(role.empty());
    for (const auto& verb : PV::kVerbs) {
        INFO(verb.id);
        CHECK(step.find(std::format("step.verb == \"{}\"", verb.id)) != std::string::npos);
        for (std::size_t i = 0; i < verb.slotCount; ++i) {
            INFO(verb.slots[i].name);
            CHECK(role.find(std::format("verb == \"{}\" and role == \"{}\"", verb.id, verb.slots[i].name))
                  != std::string::npos);
        }
        // The caption is the dashboard's, and the language file has it.
        const auto* caption = ShippedEnglish().Source(PT::CaptionKey(verb));
        REQUIRE(caption != nullptr);
        CHECK(*caption == verb.caption);
    }
}
