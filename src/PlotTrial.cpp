#include <PlotTrial.h>

#include <DashboardUIManager.h>
#include <Localization.h>
#include <logger.h>
#include <PlotDispatch.h>
#include <PlotLog.h>
#include <PlotPopulation.h>
#include <PlotText.h>
#include <PlotVerbs.h>

#include <algorithm>
#include <chrono>
#include <format>
#include <mutex>
#include <set>
#include <utility>

namespace NarrativeEngine::PlotTrial
{
    namespace
    {
        // As PlotTick draws for a real birth.
        constexpr std::size_t kShortlistSize = 5;
        constexpr std::size_t kCandidateDraw = 25;
        constexpr std::size_t kMemoriesShown = 3;

        std::mutex g_mutex;
        Snapshot g_snapshot;

        TextTemplate::Sex SexOf(const PlotCasting::Population& population, RE::FormID npc)
        {
            const auto* member = population.Find(npc);
            if (member == nullptr) {
                return TextTemplate::Sex::Other;
            }
            return member->female ? TextTemplate::Sex::Female : TextTemplate::Sex::Male;
        }

        // The mastermind's memory of doing the plan's first step themselves:
        // the one memory every plot can write, whoever is later cast.
        std::string MemoryOf(const LanguageTable::Table& table,
                             const PlotCasting::Population& population,
                             const PlotModel::Plot& plot)
        {
            if (plot.plan.empty()) {
                return {};
            }
            auto step = plot.plan.front();
            step.actor = plot.mastermind;
            step.actorName = plot.mastermindName;
            return PlotText::Memory(table, step, PlotVerbs::Template::SelfDispatched, plot.scheme, [&](RE::FormID npc) {
                return SexOf(population, npc);
            });
        }

        void Publish(const std::function<void(Snapshot&)>& change)
        {
            {
                const std::lock_guard lock{g_mutex};
                change(g_snapshot);
            }
            DashboardUIManager::PushFullState();
        }

        void Run(const PlotThread::Token& pt)
        {
            // Planning is made of decisions; a session that cannot answer one
            // can plan nothing, under any language.
            if (!PlotBirth::CanDecide()) {
                PlotLog::Note("Language trial: SkyrimNet cannot answer decisions this session; nothing was run");
                Publish([](Snapshot& s) {
                    s.running = false;
                    s.ran = true;
                });
                return;
            }
            const auto& population = PlotPopulation::Get();
            const auto& live = Plots::MutableState(pt);
            const auto predicates = LivePredicates();
            // The trial's own stream, seeded from the clock: it never reads
            // or advances the live state's.
            auto rng = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) | 1u;

            PlotLog::Note(std::format(
                "Language trial: {} births under each of {} languages", kBirthsPerLanguage, kLanguages.size()));
            for (std::size_t li = 0; li < kLanguages.size(); ++li) {
                const auto stem = kLanguages[li];
                LanguageResult result;
                result.stem = std::string{stem};
                const auto table = Localization::LoadShipped(stem);
                if (!table) {
                    PlotLog::Note(std::format("Language trial: {}: its language file could not be read", stem));
                    Publish([&](Snapshot& s) {
                        s.languages[li] = result;
                        s.done += kBirthsPerLanguage;
                    });
                    continue;
                }
                result.loaded = true;
                result.name = table->GetHeader().name;
                result.nativeName = table->GetHeader().nativeName;

                std::set<RE::FormID> masterminds;
                for (int i = 0; i < kBirthsPerLanguage; ++i) {
                    const auto birth = TrialBirth(pt, population, live, rng, *table, predicates);
                    ++result.attempted;
                    for (const auto& refusal : birth.refusals) {
                        const auto code = PlotRejection::Id(refusal.code);
                        ++result.byCode[code.empty() ? refusal.stage : std::string{code}];
                        if (const auto check = CheckOf(refusal.code); !check.empty()) {
                            ++result.byCheck[std::string{check}];
                        }
                    }
                    if (!birth.skipped.empty()) {
                        ++result.byCode["skipped"];
                    }
                    if (birth.accepted) {
                        ++result.accepted;
                        masterminds.insert(birth.plot.mastermind);
                        if (result.memories.size() < kMemoriesShown) {
                            result.memories.push_back(MemoryOf(*table, population, birth.plot));
                        }
                    }
                    result.masterminds = static_cast<int>(masterminds.size());
                    Publish([&](Snapshot& s) {
                        s.languages[li] = result;
                        ++s.done;
                    });
                }

                std::string codes;
                for (const auto& [code, n] : result.byCode) {
                    codes += std::format("{}{}={}", codes.empty() ? "" : ", ", code, n);
                }
                PlotLog::Note(std::format("Language trial: {}: {} of {} accepted, {} distinct masterminds; refused: {}",
                                          result.name,
                                          result.accepted,
                                          result.attempted,
                                          result.masterminds,
                                          codes.empty() ? "none" : codes));
                for (const auto& memory : result.memories) {
                    PlotLog::Note(std::format("Language trial: {}: memory: {}", result.name, memory));
                }
            }
            Publish([](Snapshot& s) {
                s.running = false;
                s.ran = true;
            });
        }
    } // namespace

    std::string_view CheckOf(PlotRejection::Code code) noexcept
    {
        using enum PlotRejection::Code;
        switch (code) {
        case SchemeNotVerb:
            return "Scheme";
        case NamesPlayer:
        case RoleNamesPlayer:
            return "PlayerTitle";
        case RoleNamesNobody:
        case RoleAmbiguous:
        case RoleUnnamed:
            return "names";
        default:
            return {};
        }
    }

    Predicates LivePredicates()
    {
        return {PlotPopulation::AlivePredicate(),
                PlotPopulation::ViablePredicate(),
                PlotPopulation::SeatedPredicate(),
                [](RE::FormID npc) { return PlotBirth::IsCastable(npc); }};
    }

    Birth TrialBirth(const PlotThread::Token& pt,
                     const PlotCasting::Population& population,
                     const PlotState& live,
                     std::uint64_t& rng,
                     const LanguageTable::Table& table,
                     const Predicates& predicates)
    {
        Birth birth;
        // The draw a real birth makes, from the live occupancy but the
        // trial's own stream.
        const auto cast = PlotCasting::SelectMastermindShortlist(population,
                                                                 live.occupancy,
                                                                 live.simGameDay,
                                                                 predicates.alive,
                                                                 predicates.viable,
                                                                 predicates.seated,
                                                                 kCandidateDraw,
                                                                 rng);
        std::vector<RE::FormID> shortlist;
        for (const auto npc : cast.shortlist) {
            if (shortlist.size() < kShortlistSize && predicates.castable(npc)) {
                shortlist.push_back(npc);
            }
        }
        if (shortlist.empty()) {
            birth.skipped = cast.shortlist.empty() ? "nobody eligible" : "nobody SkyrimNet could describe";
            return birth;
        }
        PlotState stream;
        stream.rngState = rng;
        for (std::size_t i = shortlist.size(); i > 1; --i) {
            const auto j = static_cast<std::size_t>(Plots::NextRandom(stream) % i);
            std::swap(shortlist[i - 1], shortlist[j]);
        }
        rng = stream.rngState;

        std::size_t decisions = 0;
        birth.plot.bornOnGameDay = live.simGameDay;
        birth.accepted =
            PlotBirth::Compose(pt, population, shortlist, live.past, birth.plot, decisions, table, &birth.refusals);
        return birth;
    }

    void Start(const PluginThread::Token&)
    {
        {
            const std::lock_guard lock{g_mutex};
            if (g_snapshot.running) {
                return;
            }
            g_snapshot = {};
            g_snapshot.running = true;
            g_snapshot.total = static_cast<int>(kLanguages.size()) * kBirthsPerLanguage;
            for (const auto stem : kLanguages) {
                LanguageResult pending;
                pending.stem = std::string{stem};
                g_snapshot.languages.push_back(std::move(pending));
            }
        }
        PlotDispatch::EnqueueWork([](const PlotThread::Token& pt) { Run(pt); });
    }

    Snapshot Get()
    {
        const std::lock_guard lock{g_mutex};
        return g_snapshot;
    }
} // namespace NarrativeEngine::PlotTrial
