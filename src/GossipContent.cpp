#include <GossipContent.h>

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
#include <vector>

namespace NarrativeEngine::GossipContent
{
    namespace
    {
        // Generations per band. Edges at 3 and 6: measured depth reaches 12,
        // but generations 9+ carry only 4% of traffic, so a fourth band
        // would be output spent on almost nothing.
        constexpr std::uint32_t kGenerationsPerBand = 3;

        // Not settings. The prompt ships in statics/ and its contract with
        // this file -- the context keys it reads, the JSON shape it must
        // return -- is compiled in just below, so repointing the call at a
        // different asset could only break it.
        //
        // The variant is the one the letter composer already uses: gossip
        // generation is creative writing in a specific voice, which is the
        // same task shape, so it wants the same model. A variant of its own
        // would mean another entry in the SkyrimNet manifest and another
        // override category for the user to tune, with nothing behind it.
        constexpr const char* kSeedPromptName = "narrative_engine_gossip_seed";
        constexpr const char* kLLMVariant = "narrative_engine_composer";

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
    } // namespace

    void Initialize()
    {
        logger::info("GossipContent: initialized (prompt='{}', variant='{}', bands={})",
                     kSeedPromptName,
                     kLLMVariant,
                     Settings::Get().gossipContentBands);
    }

    std::size_t BandForGeneration(std::uint32_t generation)
    {
        const auto bands = static_cast<std::size_t>(std::max(1, Settings::Get().gossipContentBands));
        return std::min<std::size_t>(generation / kGenerationsPerBand, bands - 1);
    }

    void RequestBands(std::int64_t sourceMemoryId,
                      RE::FormID owner,
                      const std::string& sourceText,
                      const std::string& locationName,
                      float importance)
    {
        const auto& cfg = Settings::Get();
        const int bandCount = std::max(1, cfg.gossipContentBands);

        nlohmann::json ctx;
        ctx["band_count"] = bandCount;
        ctx["source_text"] = sourceText;
        ctx["source_owner"] = NameOf(owner);
        ctx["source_location"] = locationName;
        ctx["importance"] = importance;

        const auto abandon = [sourceMemoryId](const char* why) {
            GossipClaims::Release(sourceMemoryId);
            GossipLog::Note(std::format("content: memory {} released — {}", sourceMemoryId, why));
        };

        const bool queued = SkyrimNetAPI::SendCustomPromptToLLM(
            kSeedPromptName,
            kLLMVariant,
            ctx.dump(),
            [sourceMemoryId, owner, importance, bandCount, abandon](
                const PluginThread::Token&, std::string response, bool success) {
                if (!success) {
                    abandon("LLM call failed");
                    return;
                }
                auto j = nlohmann::json::parse(response, nullptr, false);
                if (j.is_discarded() || !j.is_object()) {
                    abandon("unparseable response");
                    return;
                }
                if (!j.value("should_seed", false)) {
                    // A refusal is a correct and expected answer — the
                    // memory simply was not worth gossiping about.
                    abandon("model judged it not gossip-worthy");
                    return;
                }
                const auto& raw = j["bands"];
                if (!raw.is_array() || raw.empty()) {
                    abandon("no bands returned");
                    return;
                }

                std::vector<std::string> bands;
                bands.reserve(raw.size());
                for (const auto& b : raw) {
                    if (!b.is_string()) {
                        continue;
                    }
                    // Sanitize AT THE POINT OF EXTRACTION, before this text
                    // is cached, persisted to the co-save, or written into a
                    // memory. Band text reaches both the save payload and
                    // SkyrimNet's prompt context, so smart quotes, dashes and
                    // zero-width characters would travel a long way.
                    bands.push_back(LLMTextSanitizer::Sanitize(b.get<std::string>()));
                }
                if (bands.empty()) {
                    abandon("all bands empty after sanitize");
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
                    abandon("simulation refused the seed");
                }
            });

        if (!queued) {
            abandon("could not queue the LLM call");
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
