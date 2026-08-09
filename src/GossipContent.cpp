#include <GossipContent.h>

#include <EvaluationPipeline.h>
#include <EventLogUtil.h>
#include <GossipClaims.h>
#include <GossipDispatch.h>
#include <GossipGraph.h>
#include <GossipLog.h>
#include <GossipSim.h>
#include <GossipThread.h>
#include <JsonUtils.h>
#include <LLMTextSanitizer.h>
#include <logger.h>
#include <Settings.h>
#include <SkyrimNetAPI.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <exception>
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

        // How long the telling at `index` should run, as the prompt asks
        // for it.
        //
        // Band 0 gets 4-6 sentences, matching the upper half of what
        // SkyrimNet's own memory generation produces (2-4 soft, 6 hard) —
        // a first-hand account should read like any other memory that
        // actor holds. Each later band loses a sentence off both ends, and
        // the floor stops at "no more than 2" because a band with a
        // minimum of zero sentences is a band with no rumor in it.
        //
        //     band 0   4 to 6        band 3   1 to 3
        //     band 1   3 to 5        band 4+  no more than 2
        //     band 2   2 to 4
        //
        // Shrinking length IS the decay model. An earlier revision spelled
        // out a telephone game — soften a name, drift a number, let the
        // teller's sympathies leak in — and that turned out to be doing
        // the same work twice: a teller with two sentences for something
        // that took six to say properly drops the specifics on their own,
        // and drops the ones that matter least first. Instructing the
        // vagueness as well produced drift on top of compression.
        //
        // Computed here rather than in the template because the shape is
        // arithmetic over band_count, and this project has lost two test
        // runs to Inja constructs that looked fine and did not render.
        std::string BandLengthSpec(int index)
        {
            const int lo = std::max(0, 4 - index);
            const int hi = std::max(2, 6 - index);
            if (lo == 0) {
                return "no more than 2 sentences";
            }
            return std::format("{} to {} sentences", lo, hi);
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
        void Abandon(const GossipThread::Token& gt, std::int64_t memoryId, const std::string& why)
        {
            GossipClaims::Release(gt, memoryId);
            GossipLog::Note(std::format("content: memory {} released — {}", memoryId, why));
        }

        // `KeepMemoryOnly` is for a verdict about THIS owner rather than
        // about the happening: they would not repeat it, or it is not worth
        // repeating. The memory stays claimed so this owner is not asked
        // again, but the events go back so another witness with their own
        // account of the same happening can still seed from it.
        void KeepMemoryOnly(const GossipThread::Token& gt, std::int64_t memoryId, const std::string& why)
        {
            GossipClaims::ReleaseEvents(gt, memoryId);
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
        //
        // Blocks. That is the point: nothing else runs on the gossip
        // thread, so there is no one to yield to and no reason to hand the
        // rest of the work to a callback.
        bool RequestBands(const GossipThread::Token& gt, const Candidate& c)
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

            // One entry per band, in order, each carrying the length the
            // prompt should ask for. The template renders the list rather
            // than deriving it, so adding a band can never produce a spec
            // the code does not agree with.
            nlohmann::json bandSpecs = nlohmann::json::array();
            for (int i = 0; i < bandCount; ++i) {
                nlohmann::json entry = nlohmann::json::object();
                entry["index"] = i;
                entry["length"] = BandLengthSpec(i);
                bandSpecs.push_back(std::move(entry));
            }
            ctx["band_specs"] = std::move(bandSpecs);

            const auto result = SkyrimNetAPI::SendCustomPromptToLLM(gt, kSeedPromptName, kSeedVariant, ctx.dump());
            if (!result.ok) {
                Abandon(gt, sourceMemoryId, "seed call failed");
                return false;
            }
            // Reading the response is fallible in ways the checks below
            // cannot enumerate, so it is contained: a throw here fails this
            // one seed instead of unwinding the tick that asked for it.
            // The scope ends before SeedRumor — releasing a claim for a
            // rumor that may already have been created is a worse failure
            // than the one being guarded against.
            std::vector<std::string> bands;
            try {
                // Strip a wrapping markdown fence before parsing. Models
                // wrap their JSON in a json code fence often enough that the
                // instruction not to cannot be relied on, and the whole
                // response is discarded when they do — one seed was lost
                // this way to an otherwise perfect object.
                const auto body = EvaluationPipeline::StripMarkdownFences(result.response);
                auto j = nlohmann::json::parse(body, nullptr, false);
                if (j.is_discarded() || !j.is_object()) {
                    logger::warn("GossipContent: seed response not a JSON object: {}", body);
                    Abandon(gt, sourceMemoryId, "unparseable seed response");
                    return false;
                }

                const auto rawIt = j.find("bands");
                if (rawIt == j.end() || !rawIt->is_array() || rawIt->empty()) {
                    Abandon(gt, sourceMemoryId, "seed response carried no bands");
                    return false;
                }

                bands.reserve(rawIt->size());
                for (const auto& b : *rawIt) {
                    if (!b.is_string()) {
                        continue;
                    }
                    // Sanitize AT THE POINT OF EXTRACTION, before this text
                    // is cached, persisted to the co-save, or written into a
                    // memory. Band text reaches both the save payload and
                    // SkyrimNet's prompt context, so smart quotes, dashes
                    // and zero-width characters would travel a long way.
                    bands.push_back(LLMTextSanitizer::Sanitize(b.get<std::string>()));
                }
            } catch (const std::exception& e) {
                logger::error(
                    "GossipContent: seed response for memory {} could not be read: {}", sourceMemoryId, e.what());
                Abandon(gt, sourceMemoryId, "unreadable seed response");
                return false;
            } catch (...) {
                logger::error("GossipContent: seed response for memory {} could not be read", sourceMemoryId);
                Abandon(gt, sourceMemoryId, "unreadable seed response");
                return false;
            }

            if (bands.empty()) {
                Abandon(gt, sourceMemoryId, "all bands empty after sanitize");
                return false;
            }
            // Short responses are padded by repeating the last band rather
            // than rejected — a model returning two of three usable
            // versions is still usable.
            while (static_cast<int>(bands.size()) < bandCount) {
                bands.push_back(bands.back());
            }

            const auto id = GossipSim::SeedRumor(gt, owner, importance, sourceMemoryId, std::move(bands));
            if (id == 0) {
                Abandon(gt, sourceMemoryId, "simulation refused the seed");
                return false;
            }
            return true;
        }

        // The verdict. Returns the lowercased string the model gave, or
        // empty if nothing usable came back — the caller distinguishes
        // "refused" from "could not tell", because those settle the claim
        // differently.
        std::string Evaluate(const GossipThread::Token& gt, const Candidate& c, std::string& duplicateOf)
        {
            nlohmann::json ctx;
            ctx["source_text"] = c.text;
            ctx["source_owner"] = NameOf(c.owner);
            ctx["source_location"] = c.locationName;
            ctx["importance"] = c.importance;

            // Seats the character profile. SkyrimNet's system_head and
            // character_profile submodules read the subject from
            // `npc.UUID`; that is the key render_character_profile(...)
            // resolves against. The UUID is keyed on the PLACED REFERENCE,
            // like everything else SkyrimNet exposes, so the graph's base
            // form has to be converted.
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

            // LIVE state, not the published snapshot: a rumor seeded
            // earlier in this same tick belongs in the list this candidate
            // is judged against, and the snapshot is only published at the
            // end of the job.
            //
            // Band 0 only. The later bands are the same story degraded,
            // and showing all three would just be noise.
            nlohmann::json activeRumors = nlohmann::json::array();
            for (const auto& r : GossipSim::GetRumorViews(GossipSim::MutableState(gt))) {
                if (!r.text.empty()) {
                    activeRumors.push_back(r.text);
                }
            }
            ctx["active_rumors"] = std::move(activeRumors);

            const auto result = SkyrimNetAPI::SendCustomPromptToLLM(gt, kEvalPromptName, kEvalVariant, ctx.dump());
            if (!result.ok) {
                return {};
            }
            // Everything from here down reads a payload a model wrote, and
            // nothing a model writes is a guarantee. A throw escaping this
            // function unwinds the whole tick: the candidate's claim is
            // never settled, the simulation never advances, and the
            // snapshot is never published — all for one malformed field.
            // Failing this ONE seed is strictly better, so the failure is
            // contained where the untrusted data is read.
            try {
                const auto body = EvaluationPipeline::StripMarkdownFences(result.response);
                auto j = nlohmann::json::parse(body, nullptr, false);
                if (j.is_discarded() || !j.is_object()) {
                    logger::warn("GossipContent: evaluation response not a JSON object: {}", body);
                    return {};
                }

                auto verdict = JsonUtils::StringOr(j, "verdict");
                std::transform(verdict.begin(), verdict.end(), verdict.begin(), [](unsigned char ch) {
                    return static_cast<char>(std::tolower(ch));
                });
                duplicateOf = JsonUtils::StringOr(j, "duplicate_of");
                return verdict;
            } catch (const std::exception& e) {
                logger::error(
                    "GossipContent: evaluation response for memory {} could not be read: {}", c.memoryId, e.what());
                return {};
            } catch (...) {
                logger::error("GossipContent: evaluation response for memory {} could not be read", c.memoryId);
                return {};
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

    void RequestRumors(const GossipThread::Token& gt,
                       std::vector<Candidate> pool,
                       int maxSeeds,
                       double claimGameDay,
                       const GossipDispatch::CancellationHandle& cancel)
    {
        int seedsRemaining = std::max(1, maxSeeds);
        std::size_t skipped = 0;
        std::size_t refused = 0;

        for (const auto& c : pool) {
            if (seedsRemaining <= 0) {
                break;
            }
            // Checked before each candidate, and again after each LLM call
            // returns. A tick that keeps going past a load spends model
            // calls on a world that no longer exists and, worse, can seed
            // a rumor into it.
            if (cancel && cancel->IsCancelled()) {
                GossipLog::Note(std::format("content: abandoned after {} candidate(s) — the world it was "
                                            "evaluating has been replaced",
                                            skipped + refused));
                return;
            }

            // BOTH claim tests, re-run per candidate rather than trusted
            // from pool-construction time. The memory test is the one that
            // looks redundant and is not: `private` and `not_worthy` keep
            // the memory claim while RELEASING the events, so the event
            // test cannot see them.
            //
            // With one job per tick on a serial queue these can no longer
            // fire from a concurrent walk — that was the double-claim
            // defect, where memory 1356 was evaluated twice seventeen
            // seconds apart for two director calls and one answer. They
            // stay as a guard against claims made by EARLIER ticks, which
            // is still a live case.
            if (GossipClaims::IsClaimed(gt, c.memoryId)) {
                ++skipped;
                GossipLog::Note(
                    std::format("content: memory {} skipped — already claimed since the pool was built", c.memoryId));
                continue;
            }
            if (GossipClaims::AreEventsClaimed(gt, c.eventIds)) {
                ++skipped;
                GossipLog::Note(std::format("content: memory {} skipped — another account of the same "
                                            "happening is already in play",
                                            c.memoryId));
                continue;
            }

            // Claim BEFORE evaluating. Every path below settles it.
            GossipClaims::Claim(gt, c.memoryId, c.eventIds, claimGameDay);

            std::string duplicateOf;
            const auto verdict = Evaluate(gt, c, duplicateOf);

            if (cancel && cancel->IsCancelled()) {
                // The verdict describes a world that has been replaced.
                // Give the memory back rather than acting on it.
                Abandon(gt, c.memoryId, "evaluation returned after the world was replaced");
                return;
            }

            if (verdict == "private") {
                // The owner would not tell this. Their memory is spent;
                // the happening is not.
                ++refused;
                KeepMemoryOnly(gt, c.memoryId, "owner would not share this publicly");
                continue;
            }
            if (verdict == "not_worthy") {
                // Nothing here anyone would stop to listen to. Same claim
                // handling as `private`: this owner is done with it, the
                // happening is still open.
                ++refused;
                KeepMemoryOnly(gt, c.memoryId, "not worth gossiping about");
                continue;
            }
            if (verdict == "duplicate") {
                // The story is already going round. Both the memory and
                // its events stay claimed — this happening has had its
                // rumor, and no other account of it should start a second
                // one.
                ++refused;
                GossipLog::Note(std::format("content: memory {} claimed, no rumor — already covered{}{}",
                                            c.memoryId,
                                            duplicateOf.empty() ? "" : " by: ",
                                            LLMTextSanitizer::Sanitize(duplicateOf)));
                continue;
            }
            if (verdict != "seed") {
                // An unrecognised verdict is a prompt or model fault, not
                // a judgement about the memory, so give it all back.
                ++refused;
                Abandon(gt,
                        c.memoryId,
                        verdict.empty() ? "evaluation gave no usable answer"
                                        : "evaluation carried an unrecognised verdict");
                continue;
            }

            // Only here does the expensive model get involved.
            GossipLog::Note(std::format("content: memory {} passed evaluation — composing", c.memoryId));
            if (RequestBands(gt, c)) {
                --seedsRemaining;
            } else {
                // Composition failed and released the memory. The tick's
                // seed budget is still spent: retrying a different
                // candidate here would make a tick's cost unbounded by
                // anything the settings say, and the memory comes back
                // round on a later tick anyway.
                --seedsRemaining;
            }
        }

        // Refused and skipped are counted apart because they mean opposite
        // things: refused is the evaluator doing its job, while skipped is
        // a pool slot that never became a question at all — a tick
        // reporting mostly skips is one whose pool went stale under it,
        // not one whose candidates were poor.
        if (seedsRemaining > 0) {
            GossipLog::Note(std::format("content: no rumor this tick — {} refused, {} skipped", refused, skipped));
        }
    }

    ComposedPair Compose(const std::string& bandText, RE::FormID teller, RE::FormID listener)
    {
        const auto tie = DescribeTie(teller, listener);
        const auto tellerName = NameOf(teller);
        const auto listenerName = NameOf(listener);

        // How the listener will remember hearing it. The last row is the
        // common case by a wide margin, and that is fine — it is genuinely
        // how most gossip arrives.
        //
        // Every framing is a LEAD-IN ENDING IN A COLON, never a "…told me
        // that" clause. Band text is one to six standalone sentences, and
        // a subordinating "that" can only govern the first of them: "I
        // heard a rumor that A College mage was caught. The Arch-Mage
        // covered it up." reads as a grammatical error from the second
        // sentence on, and did so in the logs even at one sentence,
        // because the model returns a capitalised sentence rather than a
        // clause. A colon takes any number of sentences without caring.
        std::string heard;
        if (!tie.kinship.empty() && !tie.sameHold) {
            const auto where = HoldName(teller);
            heard =
                std::format("My {} came from {} with news: {}", tie.kinship, where.empty() ? "away" : where, bandText);
        } else if (!tie.kinship.empty()) {
            heard = std::format("My {} {} told me this: {}", tie.kinship, tellerName, bandText);
        } else if (tie.sameHousehold) {
            heard = std::format("{} mentioned this over supper: {}", tellerName, bandText);
        } else if (tie.sameSettlement) {
            heard = std::format("{} told me this: {}", tellerName, bandText);
        } else {
            heard = std::format("I heard a rumor going round: {}", bandText);
        }

        return ComposedPair{std::format("I told {} this: {}", listenerName, bandText), std::move(heard)};
    }
} // namespace NarrativeEngine::GossipContent
