#include <CharacterBios.h>

#include <cctype>
#include <fstream>
#include <sstream>
#include <unordered_map>

#include <GossipGraph.h>
#include <logger.h>
#include <PlotPopulation.h>
#include <SkyrimNetAPI.h>

// CharacterBioLibrary — the engine-bound half of CharacterBios.
//
// The catalogue, the filename matching and the block parsing are all
// pure and live in CharacterBios.cpp, where a probe can drive them with
// invented stems and no game. This file is what needs a running plugin:
// it knows where the bios are on disk, it crosses base NPC forms to
// reference ids through GossipGraph, and it holds the result for the
// session.
//
// The split is PlotCasting / PlotPopulation's, for the same reason. The
// rule that a filename suffix is the low 12 bits of a REFERENCE id is
// the part worth asserting directly, and it would not be assertable if
// it only existed inside a function that reads a directory.
namespace NarrativeEngine::CharacterBios
{
    namespace
    {
        const std::filesystem::path kPromptRoot{"Data/SKSE/Plugins/SkyrimNet/prompts"};
        const std::filesystem::path kSharedBios = kPromptRoot / "characters";

        // Where a save keeps its OWN copies of the profiles it has
        // edited. Only the ones it has edited: these directories hold a
        // handful of files each, and everything else still comes from
        // the shared folder.
        [[nodiscard]] std::filesystem::path SaveBios(const std::string& saveId)
        {
            return kPromptRoot / "_saves" / saveId / "characters";
        }

        [[nodiscard]] std::string ReadFile(const std::filesystem::path& path)
        {
            std::ifstream in{path, std::ios::binary};
            if (!in) {
                return {};
            }
            std::ostringstream buffer;
            buffer << in.rdbuf();
            return buffer.str();
        }
        // What Build managed, for the one line it logs. Internal: the
        // only consumer outside this file was the smoke test.
        struct Census
        {
            // The save whose overrides were read, or empty when only the
            // shared directory was used.
            std::string saveId;
            std::size_t saveFiles = 0;     // catalogued from this save's own directory
            std::size_t files = 0;         // catalogued in total, with a usable suffix
            std::size_t generic = 0;       // shared templates, skipped
            std::size_t confirmed = 0;     // matched, name agreed
            std::size_t bySuffixAlone = 0; // matched on the suffix alone -- excluded
            std::size_t ambiguous = 0;     // several candidates, none matched -- refused
            std::size_t noFile = 0;
            std::size_t noRef = 0;
            std::size_t unreadable = 0;
            std::size_t loaded = 0;
        };

        Census g_census;
        std::unordered_map<RE::FormID, Bio> g_bios;
        CharacterIndex::Index g_search;
        // Normalised display name -> population member. Only people who
        // actually have a biography: naming somebody this plugin cannot
        // describe is the same as naming nobody.
        std::unordered_map<std::string, RE::FormID> g_byName;

        // One WORD of a display name -> the member it belongs to, but
        // only for words belonging to exactly one of them. A word two
        // people share resolves to neither, which is what keeps the
        // Black-Briars and the Gray-Manes out of this.
        std::unordered_map<std::string, RE::FormID> g_byNameWord;
        const Bio g_empty;
    } // namespace

    void OnRevert()
    {
        g_bios.clear();
        g_search.Clear();
        g_byName.clear();
        g_byNameWord.clear();
        g_census = Census{};
    }

    void OnSessionStart()
    {
        // OnRevert has already emptied this on every path that reaches
        // here -- the serialization interface guarantees revert runs
        // before both kNewGame and kPostLoadGame. Cleared again anyway,
        // because emplace() does NOT overwrite an existing key: if that
        // guarantee ever failed, the load below would silently keep the
        // previous save's entry for everybody it had already seen, which
        // is precisely the leak this is here to prevent.
        g_bios.clear();
        g_search.Clear();
        g_byName.clear();
        g_byNameWord.clear();
        g_census = Census{};

        Catalog catalog;

        // The save's own copies FIRST, so they win. SkyrimNet writes an
        // edited profile into the save's directory rather than over the
        // shared one, which is what lets two saves disagree about a
        // character; scanning the shared folder first would make those
        // edits invisible.
        g_census.saveId = SkyrimNetAPI::GetSaveUniqueID();
        if (!g_census.saveId.empty()) {
            g_census.saveFiles = catalog.Scan(SaveBios(g_census.saveId));
        } else {
            // A new game before SkyrimNet has minted an id, an older
            // SkyrimNet without the v7 export, or no SkyrimNet at all.
            // The shared profiles are still correct; only the save's
            // own edits are missing.
            logger::info("CharacterBios: no save id available; reading the shared profiles only.");
        }

        g_census.files = g_census.saveFiles + catalog.Scan(kSharedBios);
        g_census.generic = catalog.GenericCount();
        if (g_census.files == 0) {
            logger::warn("CharacterBios: no bio files under {}; plots will have no character prose to work from.",
                         kSharedBios.string());
            return;
        }

        for (const auto& member : PlotPopulation::Get().members) {
            // A REFERENCE id. The base NPC form is the near miss: the CK
            // usually numbers a reference one past the base it came
            // from, so base forms resolve for most people and quietly
            // mis-file everyone else.
            const auto ref = GossipGraph::ActorRefFor(member.npc);
            if (ref == 0) {
                ++g_census.noRef;
                continue;
            }

            const auto found = catalog.Find(ref, member.name);
            switch (found.outcome) {
            case Outcome::Confirmed:
                ++g_census.confirmed;
                break;
            case Outcome::BySuffixAlone:
                ++g_census.bySuffixAlone;
                // EXCLUDED, not accepted. A suffix is only twelve bits,
                // and with 2237 files landing in 1733 buckets a
                // collision is ordinary rather than rare -- so a lone
                // candidate whose name does not agree is far more often
                // somebody else's biography than an alternate spelling
                // of this person's. Measured on a real run: of 35 such
                // matches roughly 25 were a different character
                // entirely, including the Arch-Mage of Winterhold
                // carrying a Tel Mithryn alchemist's life story.
                //
                // The handful of genuine cases lost with them -- files
                // named for a character's real or former name -- are
                // worth less than the damage a wrong biography does,
                // which is invisible: it reads perfectly, it feeds the
                // agent scorer, and it describes the wrong person.
                logger::debug("CharacterBios: {} (ref 0x{:X}) EXCLUDED -- '{}' matched on the suffix alone and "
                              "the name did not agree, so it is probably not their biography.",
                              member.name,
                              ref,
                              found.stem);
                continue;
            case Outcome::Ambiguous:
                ++g_census.ambiguous;
                continue;
            case Outcome::NoFile:
                ++g_census.noFile;
                continue;
            }

            // The path the catalogue resolved, not one rebuilt from the
            // stem: the same stem exists in both directories whenever
            // this save has edited that profile, and rebuilding would
            // always pick the shared one.
            auto bio = ParseBio(ReadFile(found.path));
            if (bio.Empty()) {
                ++g_census.unreadable;
                continue;
            }
            g_bios.emplace(member.npc, std::move(bio));
            // Keyed on the DISPLAY name, which is what a prompt writes,
            // rather than on the bio filename's slug -- those disagree
            // for anyone whose file is named for someone else.
            auto key = Normalise(member.name);
            if (!key.empty()) {
                g_byName.emplace(std::move(key), member.npc);
            }
            // And each word of it separately. A word already claimed by
            // somebody else is struck out rather than overwritten: a
            // surname shared by six Battle-Borns identifies none of
            // them, and quietly keeping the last one indexed would make
            // it identify whichever happened to load last.
            for (std::size_t at = 0; at < member.name.size();) {
                const auto end = member.name.find(' ', at);
                const auto word =
                    Normalise(member.name.substr(at, end == std::string::npos ? std::string::npos : end - at));
                at = end == std::string::npos ? member.name.size() : end + 1;
                if (word.size() < 4) {
                    continue;
                }
                const auto seen = g_byNameWord.find(word);
                if (seen == g_byNameWord.end()) {
                    g_byNameWord.emplace(word, member.npc);
                } else if (seen->second != member.npc) {
                    seen->second = 0; // shared, so it names nobody
                }
            }
            ++g_census.loaded;
        }

        // Indexed once the whole population is in, because BM25's term
        // weighting is a property of the corpus rather than of any one
        // document: adding a bio changes what every other bio's terms
        // are worth.
        for (const auto& [npc, bio] : g_bios) {
            g_search.Add(npc, bio.Prose());
        }
        g_search.Build();

        logger::info("CharacterBios: {} bio(s) loaded for {} population member(s) from {} file(s) ({} of them this "
                     "save's own, id '{}') -- {} confirmed by name, {} EXCLUDED as suffix-only matches, {} ambiguous, "
                     "{} with no file, {} with no reference id, {} unreadable.",
                     g_census.loaded,
                     PlotPopulation::Get().members.size(),
                     g_census.files,
                     g_census.saveFiles,
                     g_census.saveId,
                     g_census.confirmed,
                     g_census.bySuffixAlone,
                     g_census.ambiguous,
                     g_census.noFile,
                     g_census.noRef,
                     g_census.unreadable);
    }

    bool Has(RE::FormID npc)
    {
        return g_bios.contains(npc);
    }

    const Bio& For(RE::FormID npc)
    {
        const auto found = g_bios.find(npc);
        return found == g_bios.end() ? g_empty : found->second;
    }

    RE::FormID FindByName(std::string_view text)
    {
        const auto needle = Normalise(text);
        if (needle.empty()) {
            return 0;
        }

        // Longest wins. A string naming two people -- somebody and their
        // steward, say -- should resolve to whichever name is more
        // specific, and the longer one always is.
        RE::FormID best = 0;
        std::size_t bestLength = 0;
        for (const auto& [name, npc] : g_byName) {
            const auto length = NameMatchLength(name, needle);
            if (length > bestLength) {
                best = npc;
                bestLength = length;
            }
        }
        if (best != 0) {
            return best;
        }

        // No whole name matched. Try a distinctive WORD of one, taken
        // from the front of the text where a name belongs, and only
        // where the text capitalises it.
        constexpr std::size_t kNameWordWindow = 4;
        std::size_t at = 0;
        for (std::size_t index = 0; index < kNameWordWindow && at < text.size(); ++index) {
            while (at < text.size() && !std::isalpha(static_cast<unsigned char>(text[at]))) {
                ++at;
            }
            const auto start = at;
            while (at < text.size() && std::isalpha(static_cast<unsigned char>(text[at]))) {
                ++at;
            }
            if (start == at) {
                break;
            }
            // Capitalised, which is what tells a person called Hunter
            // from a step asking for a hunter.
            if (!std::isupper(static_cast<unsigned char>(text[start]))) {
                continue;
            }
            const auto word = Normalise(text.substr(start, at - start));
            if (word.size() < 4) {
                continue;
            }
            const auto found = g_byNameWord.find(word);
            if (found != g_byNameWord.end() && found->second != 0) {
                return found->second;
            }
        }
        return 0;
    }

    const CharacterIndex::Index& Search()
    {
        return g_search;
    }

} // namespace NarrativeEngine::CharacterBios
