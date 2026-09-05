#include <CharacterIndexSmokeTest.h>

#include <atomic>
#include <string>
#include <string_view>
#include <vector>

#include <AsyncDispatch.h>
#include <CharacterBios.h>
#include <CharacterIndex.h>
#include <logger.h>
#include <PlotPopulation.h>

namespace NarrativeEngine::CharacterIndexSmokeTest
{
    namespace
    {
        std::atomic_bool gRan{false};

        // The queries. Deliberately phrased as descriptions of a PERSON,
        // not as steps -- a step description is about its subject, so
        // querying with one returns the step's target rather than
        // anybody who could carry it out.
        //
        // Mixed on purpose. Some name an occupation the bios use in
        // those words, some name a place, some name a disposition with
        // no obvious vocabulary at all, and a few are institutions --
        // the case the offline harness found BM25 worst at, because an
        // institution's NAME carries almost no weight next to its
        // internal jargon. If those are the only ones that come back
        // wrong, that is the expected result and not a surprise.
        constexpr std::string_view kQueries[]{
            "a courier who carries letters between holds for coin",
            "a clerk who handles shipping manifests and warehouse paperwork",
            "a blacksmith who forges weapons and sells them",
            "an innkeeper who hears everything said in the common room",
            "a servant in a wealthy household who resents their employer",
            "a guard who patrols the walls and watches who comes and goes",
            "a court wizard who advises a jarl on arcane matters",
            "a steward who manages a jarl's household and accounts",
            "a priest who tends a temple and hears confessions",
            "a fence who moves stolen goods without asking questions",
            "a miner who works underground for wages",
            "a farmer who works the fields outside a walled city",
            "a hunter who tracks game through the wilds and sells pelts",
            "an alchemist who brews potions and sells ingredients",
            "a bard who performs in taverns and collects gossip",
            "a mercenary who takes coin to fight for anyone",
            "a thief who picks pockets and breaks into houses",
            "a sailor who works the docks and knows what comes off the boats",
            "a mage of the College of Winterhold with access to its archives",
            "a member of the Thieves Guild in Riften",
            "a Companion of Jorrvaskr who takes jobs for coin",
            "a Thalmor agent who watches for Talos worship",
            "a Stormcloak loyalist who despises the Empire",
            "an Imperial Legion officer stationed in Skyrim",
            "a Redoran guard in Raven Rock",
            "someone deeply in debt who needs money badly",
            "someone with a grudge against their own family",
            "someone who drinks too much and talks when they do",
            "an ambitious person impatient to rise above their station",
            "a cautious person who avoids trouble and keeps their head down",
            "a widow grieving a husband lost to the war",
            "an orphaned child living rough in a city",
        };

        struct Loaded
        {
            std::size_t indexed = 0;
        };

        Loaded Fill(CharacterIndex::Index& index)
        {
            Loaded stats;
            for (const auto& member : PlotPopulation::Get().members) {
                const auto& bio = CharacterBios::For(member.npc);
                if (bio.Empty()) {
                    continue;
                }
                index.Add(member.npc, bio.Prose());
                ++stats.indexed;
            }
            return stats;
        }

        void Run(const PluginThread::Token&)
        {
            const auto& population = PlotPopulation::Get();
            if (population.members.empty()) {
                logger::warn("CharacterIndexSmokeTest: the population is empty; nothing to index.");
                return;
            }

            if (!CharacterBios::IsReady()) {
                logger::warn("CharacterIndexSmokeTest: CharacterBios has not been built; nothing to index.");
                return;
            }
            const auto& census = CharacterBios::GetCensus();
            logger::info("CharacterIndexSmokeTest: {} bio(s) available from {} catalogued file(s) -- {} confirmed by "
                         "name, {} on the suffix alone, {} refused as ambiguous, {} with no file.",
                         census.loaded,
                         census.files,
                         census.confirmed,
                         census.bySuffixAlone,
                         census.ambiguous,
                         census.noFile);

            CharacterIndex::Index index;
            const auto stats = Fill(index);
            index.Build();

            logger::info(
                "CharacterIndexSmokeTest: {} of {} member(s) indexed.", stats.indexed, population.members.size());
            logger::info("CharacterIndexSmokeTest: vocabulary {} term(s), mean document {:.0f} term(s)",
                         index.VocabularySize(),
                         index.AverageLength());

            if (index.Size() == 0) {
                logger::warn("CharacterIndexSmokeTest: nothing was indexed; the queries below would all be empty.");
                return;
            }

            constexpr std::size_t kShow = 5;
            for (const auto query : kQueries) {
                logger::info("CharacterIndexSmokeTest: QUERY \"{}\"", query);
                const auto hits = index.Query(query, kShow);
                if (hits.empty()) {
                    logger::info("CharacterIndexSmokeTest:     (no match)");
                    continue;
                }
                for (std::size_t rank = 0; rank < hits.size(); ++rank) {
                    const auto& hit = hits[rank];
                    const auto* member = population.Find(hit.character);
                    logger::info("CharacterIndexSmokeTest:     {}. {:<28} {:6.2f}  {:3.0f}%",
                                 rank + 1,
                                 member ? member->name : std::string{"<unknown>"},
                                 hit.score,
                                 hit.relative * 100.0f);
                }
            }
            logger::info("CharacterIndexSmokeTest: done.");
        }
    } // namespace

    void OnSessionStart()
    {
        // Once per game run, not once per load. Re-indexing on every
        // save the player loads would add several hundred file reads to
        // each one for an answer that cannot have changed.
        if (gRan.exchange(true)) {
            return;
        }
        AsyncDispatch::EnqueueWork([](const PluginThread::Token& pt) { Run(pt); });
    }
} // namespace NarrativeEngine::CharacterIndexSmokeTest
