#include <GossipContent.h>

#include <EvaluationPipeline.h>
#include <EventLogUtil.h>
#include <GossipClaims.h>
#include <GossipGraph.h>
#include <GossipLog.h>
#include <GossipSim.h>
#include <LLMTextSanitizer.h>
#include <logger.h>
#include <Settings.h>
#include <SkyrimNetAPI.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <memory>
#include <vector>

namespace NarrativeEngine::GossipContent
{
    namespace
    {
        // Generations per band. Edges at 3 and 6: measured depth reaches 12,
        // but generations 9+ carry only 4% of traffic, so a fourth band
        // would be output spent on almost nothing.
        constexpr std::uint32_t kGenerationsPerBand = 3;

        // Not settings. The prompts ship in statics/ and their contract with
        // this file -- the context keys they read, the JSON shape they must
        // return -- is compiled in just below, so repointing a call at a
        // different asset could only break it.
        //
        // TWO CALLS, TWO MODELS, and the split is the whole point. Judging a
        // memory is classification: read a profile, compare against a list,
        // return one word from a fixed set. Writing the bands is creative
        // writing in a specific register. Those want different models, and
        // the expensive one should not be paying for the answer "no".
        //
        // Most candidates never reach the second call. `private`,
        // `not_worthy` and `duplicate` all settle on the cheap model, and on
        // the first working run those three accounted for the clear majority
        // of verdicts -- every one of which used to be a composer call spent
        // to be told the memory was unusable.
        //
        // The eval variant is the director's: structured-JSON decision work,
        // same shape as the per-tick tension evaluation. The seed variant is
        // the letter composer's: writing in a voice, same shape as a letter.
        // Neither needed a new manifest entry.
        constexpr const char* kEvalPromptName = "narrative_engine_gossip_eval";
        constexpr const char* kEvalVariant = "narrative_engine_director";
        constexpr const char* kSeedPromptName = "narrative_engine_gossip_seed";
        constexpr const char* kSeedVariant = "narrative_engine_composer";

        // The relationship between two participants, for framing only.
        struct Tie
        {
            std::string kinship; // "sister", "cousin", … or empty
            bool sameHousehold = false;
            bool sameSettlement = false;
            bool sameHold = false;
        };

        Tie DescribeTie(RE::FormID teller, RE::FormID listener)
        {
            Tie tie;
            const auto* a = GossipGraph::Find(teller);
            const auto* b = GossipGraph::Find(listener);
            if (!a || !b) {
                return tie;
            }
            tie.sameHousehold = a->household != 0 && a->household == b->household;
            tie.sameSettlement = a->settlement != 0 && a->settlement == b->settlement;
            tie.sameHold = a->hold == b->hold;

            auto* na = RE::TESForm::LookupByID<RE::TESNPC>(teller);
            auto* nb = RE::TESForm::LookupByID<RE::TESNPC>(listener);
            if (!na || !nb) {
                return tie;
            }
            auto* rel = RE::BGSRelationship::GetRelationship(na, nb);
            if (!rel || !rel->assocType) {
                return tie;
            }
            // associationLabels[Members][Sexes]. Which row applies depends on
            // which side of the relationship the LISTENER sits — the label
            // is what the teller calls them.
            const auto member =
                (rel->npc1 == nb) ? RE::BGSAssociationType::Members::kParent : RE::BGSAssociationType::Members::kChild;
            const auto sex = (nb->GetSex() == RE::SEX::kFemale) ? RE::BGSAssociationType::Sexes::kFemale
                                                                : RE::BGSAssociationType::Sexes::kMale;
            if (const auto& label = rel->assocType->associationLabels[member][sex]; !label.empty()) {
                tie.kinship = label.c_str();
            }
            return tie;
        }

        std::string NameOf(RE::FormID npc)
        {
            const auto& n = GossipGraph::NpcName(npc);
            return n.empty() ? std::string{"someone"} : n;
        }

        std::string HoldName(RE::FormID npc)
        {
            const auto* p = GossipGraph::Find(npc);
            if (!p) {
                return {};
            }
            const auto& n = GossipGraph::LocationName(p->settlement ? p->settlement : p->hold);
            return n;
        }

        // The two ways a candidate can end without a rumor, distinguished by
        // what they do to the claims. Free functions rather than lambdas
        // because both stages need them and the second stage is reached from
        // inside the first one's callback.
        //
        // `Abandon` is for the cases where nothing was learned — a call
        // failed, or an answer could not be read. The memory goes back
        // entirely so it can be tried again on a later sweep.
        void Abandon(std::int64_t memoryId, const std::string& why)
        {
            GossipClaims::Release(memoryId);
            GossipLog::Note(std::format("content: memory {} released — {}", memoryId, why));
        }

        // `KeepMemoryOnly` is for a verdict about THIS owner rather than
        // about the happening: they would not repeat it, or it is not worth
        // repeating. The memory stays claimed so this owner is not asked
        // again, but the events go back so another witness with their own
        // account of the same happening can still seed from it.
        void KeepMemoryOnly(std::int64_t memoryId, const std::string& why)
        {
            GossipClaims::ReleaseEvents(memoryId);
            GossipLog::Note(std::format("content: memory {} kept, events freed — {}", memoryId, why));
        }

        // Stage two. Reached ONLY from a `seed` verdict, which is what keeps
        // the expensive model off the refusals.
        //
        // Deliberately narrower context than the evaluation: no character
        // profile, no active-rumor list, no importance. Discretion and
        // duplication were settled upstream, and re-showing the material
        // behind those judgements here would be paying the composer to
        // reconsider a decision it is explicitly told not to revisit. What
        // is left is what writing the tellings actually needs — the event,
        // who it happened to, and where.
        void RequestBands(const Candidate& c)
        {
            const auto sourceMemoryId = c.memoryId;
            const auto owner = c.owner;
            const auto importance = c.importance;
            const int bandCount = std::max(1, Settings::Get().gossipContentBands);

            nlohmann::json ctx;
            ctx["band_count"] = bandCount;
            ctx["source_text"] = c.text;
            ctx["source_owner"] = NameOf(owner);
            ctx["source_location"] = c.locationName;

            const bool queued = SkyrimNetAPI::SendCustomPromptToLLM(
                kSeedPromptName,
                kSeedVariant,
                ctx.dump(),
                [sourceMemoryId, owner, importance, bandCount](
                    const PluginThread::Token&, std::string response, bool success) {
                    if (!success) {
                        Abandon(sourceMemoryId, "seed call failed");
                        return;
                    }
                    const auto body = EvaluationPipeline::StripMarkdownFences(response);
                    auto j = nlohmann::json::parse(body, nullptr, false);
                    if (j.is_discarded() || !j.is_object()) {
                        logger::warn("GossipContent: seed response not a JSON object: {}", body);
                        Abandon(sourceMemoryId, "unparseable seed response");
                        return;
                    }

                    const auto rawIt = j.find("bands");
                    if (rawIt == j.end() || !rawIt->is_array() || rawIt->empty()) {
                        Abandon(sourceMemoryId, "seed response carried no bands");
                        return;
                    }

                    std::vector<std::string> bands;
                    bands.reserve(rawIt->size());
                    for (const auto& b : *rawIt) {
                        if (!b.is_string()) {
                            continue;
                        }
                        // Sanitize AT THE POINT OF EXTRACTION, before this
                        // text is cached, persisted to the co-save, or
                        // written into a memory. Band text reaches both the
                        // save payload and SkyrimNet's prompt context, so
                        // smart quotes, dashes and zero-width characters
                        // would travel a long way.
                        bands.push_back(LLMTextSanitizer::Sanitize(b.get<std::string>()));
                    }
                    if (bands.empty()) {
                        Abandon(sourceMemoryId, "all bands empty after sanitize");
                        return;
                    }
                    // Short responses are padded by repeating the last band
                    // rather than rejected — a model returning two of three
                    // usable versions is still usable.
                    while (static_cast<int>(bands.size()) < bandCount) {
                        bands.push_back(bands.back());
                    }

                    const auto id = GossipSim::SeedRumor(owner, importance, sourceMemoryId, std::move(bands));
                    if (id == 0) {
                        Abandon(sourceMemoryId, "simulation refused the seed");
                    }
                });

            if (!queued) {
                Abandon(sourceMemoryId, "could not queue the seed call");
            }
        }

        // The state of one sweep's walk down its candidate pool. Held by
        // shared_ptr because it has to outlive the call that created it:
        // each step continues from an LLM callback, so the walk is a chain
        // of plugin-thread continuations rather than a loop on a stack.
        //
        // Nothing here needs a mutex. Every step runs on the plugin thread,
        // and only one step of a given walk is ever outstanding — the next
        // is scheduled from the previous one's callback, never alongside it.
        struct Walk
        {
            std::vector<Candidate> pool;
            std::size_t next = 0;
            int seedsRemaining = 1;
            double claimGameDay = 0.0;
        };

        void Advance(const std::shared_ptr<Walk>& walk);

        // Evaluate one candidate. Whatever the outcome, the walk continues
        // from inside the callback — this is the only place that decides
        // whether a sweep is finished.
        void Evaluate(const std::shared_ptr<Walk>& walk, const Candidate& c)
        {
            nlohmann::json ctx;
            ctx["source_text"] = c.text;
            ctx["source_owner"] = NameOf(c.owner);
            ctx["source_location"] = c.locationName;
            ctx["importance"] = c.importance;

            // Seats the character profile. SkyrimNet's system_head and
            // character_profile submodules read the subject from `npc.UUID`;
            // that is the key render_character_profile(...) resolves
            // against. The UUID is keyed on the PLACED REFERENCE, like
            // everything else SkyrimNet exposes, so the graph's base form
            // has to be converted.
            //
            // Without this the prompt could only be told the owner's name,
            // which is not enough to judge whether they would repeat a
            // thing.
            const auto ownerRef = GossipGraph::ActorRefFor(c.owner);
            const std::uint64_t ownerUUID = ownerRef ? SkyrimNetAPI::FormIDToUUID(ownerRef) : 0;
            if (ownerUUID == 0) {
                logger::warn("GossipContent: no SkyrimNet UUID for {} (ref 0x{:X}); the evaluation will "
                             "judge discretion without a character profile",
                             NameOf(c.owner),
                             ownerRef);
            }
            nlohmann::json npc = nlohmann::json::object();
            npc["UUID"] = ownerUUID;
            ctx["npc"] = std::move(npc);

            // Rebuilt for every step rather than once per walk: a rumor
            // seeded earlier in this same sweep belongs in the list the next
            // candidate is judged against.
            //
            // Band 0 only. The later bands are the same story degraded, and
            // showing all three would just be noise.
            nlohmann::json activeRumors = nlohmann::json::array();
            for (const auto& r : GossipSim::GetRumorViews()) {
                if (!r.text.empty()) {
                    activeRumors.push_back(r.text);
                }
            }
            ctx["active_rumors"] = std::move(activeRumors);

            const auto memoryId = c.memoryId;
            const bool queued = SkyrimNetAPI::SendCustomPromptToLLM(
                kEvalPromptName,
                kEvalVariant,
                ctx.dump(),
                // `c` by value: the walk owns the pool, but copying the one
                // candidate under evaluation keeps this callback independent
                // of the pool's lifetime and of any later mutation of it.
                [walk, c](const PluginThread::Token&, std::string response, bool success) {
                    if (!success) {
                        Abandon(c.memoryId, "evaluation call failed");
                        Advance(walk);
                        return;
                    }
                    // Strip a wrapping markdown fence before parsing. Models
                    // wrap their JSON in a json code fence often enough that
                    // the instruction not to cannot be relied on, and the
                    // whole response is discarded when they do — one seed
                    // was lost this way to an otherwise perfect object.
                    const auto body = EvaluationPipeline::StripMarkdownFences(response);
                    auto j = nlohmann::json::parse(body, nullptr, false);
                    if (j.is_discarded() || !j.is_object()) {
                        logger::warn("GossipContent: evaluation response not a JSON object: {}", body);
                        Abandon(c.memoryId, "unparseable evaluation response");
                        Advance(walk);
                        return;
                    }

                    auto verdict = j.value("verdict", std::string{});
                    std::transform(verdict.begin(), verdict.end(), verdict.begin(), [](unsigned char ch) {
                        return static_cast<char>(std::tolower(ch));
                    });

                    if (verdict == "private") {
                        // The owner would not tell this. Their memory is
                        // spent; the happening is not.
                        KeepMemoryOnly(c.memoryId, "owner would not share this publicly");
                        Advance(walk);
                        return;
                    }
                    if (verdict == "not_worthy") {
                        // Nothing here anyone would stop to listen to. Same
                        // claim handling as `private`: this owner is done
                        // with it, the happening is still open.
                        KeepMemoryOnly(c.memoryId, "not worth gossiping about");
                        Advance(walk);
                        return;
                    }
                    if (verdict == "duplicate") {
                        // The story is already going round. Both the memory
                        // and its events stay claimed — this happening has
                        // had its rumor, and no other account of it should
                        // start a second one.
                        const auto which = j.value("duplicate_of", std::string{});
                        GossipLog::Note(std::format("content: memory {} claimed, no rumor — already covered{}{}",
                                                    c.memoryId,
                                                    which.empty() ? "" : " by: ",
                                                    LLMTextSanitizer::Sanitize(which)));
                        Advance(walk);
                        return;
                    }
                    if (verdict != "seed") {
                        // An unrecognised verdict is a prompt or model
                        // fault, not a judgement about the memory, so give
                        // it all back.
                        Abandon(c.memoryId,
                                verdict.empty() ? "evaluation carried no verdict"
                                                : "evaluation carried an unrecognised verdict");
                        Advance(walk);
                        return;
                    }

                    // Accepted. The sweep's seed budget is spent HERE rather
                    // than when the bands arrive: composition is a separate
                    // call that can still fail, and retrying a different
                    // candidate on that failure would make a sweep's cost
                    // unbounded by anything the settings say. A failed
                    // composition releases the memory, so it comes back
                    // round on a later sweep.
                    --walk->seedsRemaining;
                    GossipLog::Note(std::format("content: memory {} passed evaluation — composing", c.memoryId));
                    RequestBands(c);
                    if (walk->seedsRemaining > 0) {
                        Advance(walk);
                    }
                });

            if (!queued) {
                Abandon(memoryId, "could not queue the evaluation call");
                Advance(walk);
            }
        }

        void Advance(const std::shared_ptr<Walk>& walk)
        {
            while (walk->next < walk->pool.size()) {
                const auto c = walk->pool[walk->next++];

                // Re-checked here, not just at qualification. Nothing in the
                // pool is claimed until its own step runs, so two accounts
                // of the same happening can both be sitting in it; whichever
                // the shuffle put first claims the events, and the other
                // must see that.
                if (GossipClaims::AreEventsClaimed(c.eventIds)) {
                    GossipLog::Note(std::format("content: memory {} skipped — another account of the same "
                                                "happening is already in play",
                                                c.memoryId));
                    continue;
                }

                // Claim BEFORE evaluating, so a candidate cannot be picked
                // up elsewhere while its call is in flight. Every path out
                // of Evaluate settles this claim one way or another.
                GossipClaims::Claim(c.memoryId, c.eventIds, walk->claimGameDay);
                Evaluate(walk, c);
                return;
            }

            // Only worth saying when the pool was used up without seeding. A
            // walk that stopped early did so because it seeded, which the
            // SEED line already records.
            if (walk->seedsRemaining > 0) {
                GossipLog::Note(
                    std::format("content: no rumor this sweep — all {} candidate(s) refused", walk->pool.size()));
            }
        }
    } // namespace

    void Initialize()
    {
        logger::info("GossipContent: initialized (eval='{}' on '{}', seed='{}' on '{}', bands={})",
                     kEvalPromptName,
                     kEvalVariant,
                     kSeedPromptName,
                     kSeedVariant,
                     Settings::Get().gossipContentBands);
    }

    std::size_t BandForGeneration(std::uint32_t generation)
    {
        const auto bands = static_cast<std::size_t>(std::max(1, Settings::Get().gossipContentBands));
        return std::min<std::size_t>(generation / kGenerationsPerBand, bands - 1);
    }

    void RequestRumors(std::vector<Candidate> pool, int maxSeeds, double claimGameDay)
    {
        if (pool.empty()) {
            return;
        }
        auto walk = std::make_shared<Walk>();
        walk->pool = std::move(pool);
        walk->seedsRemaining = std::max(1, maxSeeds);
        walk->claimGameDay = claimGameDay;
        Advance(walk);
    }

    ComposedPair Compose(const std::string& bandText, RE::FormID teller, RE::FormID listener)
    {
        const auto tie = DescribeTie(teller, listener);
        const auto tellerName = NameOf(teller);
        const auto listenerName = NameOf(listener);

        // How the listener will remember hearing it. The last row is the
        // common case by a wide margin, and that is fine — it is genuinely
        // how most gossip arrives.
        std::string heard;
        if (!tie.kinship.empty() && !tie.sameHold) {
            const auto where = HoldName(teller);
            heard = std::format(
                "My {} came from {}, and told me that {}", tie.kinship, where.empty() ? "away" : where, bandText);
        } else if (!tie.kinship.empty()) {
            heard = std::format("My {} {} told me that {}", tie.kinship, tellerName, bandText);
        } else if (tie.sameHousehold) {
            heard = std::format("{} mentioned over supper that {}", tellerName, bandText);
        } else if (tie.sameSettlement) {
            heard = std::format("{} told me that {}", tellerName, bandText);
        } else {
            heard = std::format("I heard a rumor that {}", bandText);
        }

        return ComposedPair{std::format("I told {} that {}", listenerName, bandText), std::move(heard)};
    }
} // namespace NarrativeEngine::GossipContent
