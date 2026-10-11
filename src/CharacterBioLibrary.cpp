#include <CharacterBios.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include <nlohmann/json.hpp>

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
        const std::filesystem::path kSkyrimNetRoot{"Data/SKSE/Plugins/SkyrimNet"};

        // --- where the bios are ------------------------------------------
        //
        // SkyrimNet Beta 25 serves a bio from a stack of LAYERS, each a
        // folder holding its own `prompts/characters/`, and the highest
        // layer that has the file provides it. Highest first:
        //
        //   saves/<save id>/      this playthrough's own edited profiles
        //   overlay/              the player's dashboard edits
        //   pins                  a file the player told one plugin to win
        //   plugins               external/<id>/ and library/<id>/, in the
        //                         order content-registry.json gives, with
        //                         any external one it does not list yet on
        //                         top -- which is where SkyrimNet puts a
        //                         newly found layer
        //   library/skyrimnet.base/   SkyrimNet's shipped defaults
        //
        // The loose `prompts/characters/` and `prompts/_saves/` of the
        // betas before 25 are NOT read: Beta 25 ignores them and leaves
        // them in place, so a stale copy there would describe somebody
        // the way SkyrimNet itself no longer does.
        //
        // Catalog::Scan is first-wins, so walking this list in order is
        // the whole of the precedence rule.
        [[nodiscard]] std::filesystem::path BiosIn(const std::filesystem::path& layer)
        {
            return layer / "prompts" / "characters";
        }

        struct Layers
        {
            std::vector<std::filesystem::path> plugins; // highest first
            // A pinned bio, keyed by its file name, and the plugin layer
            // the player chose to provide it.
            std::vector<std::pair<std::string, std::filesystem::path>> pins;
        };

        // A plugin the registry does not mention, or one it mentions but
        // whose folder has gone, is not an error: the registry is
        // SkyrimNet's record of what it last saw, not a manifest.
        [[nodiscard]] Layers ReadPluginLayers()
        {
            Layers out;
            const auto registryPath = kSkyrimNetRoot / "content-registry.json";
            nlohmann::json registry;
            try {
                std::ifstream in{registryPath};
                if (in) {
                    registry = nlohmann::json::parse(in);
                }
            } catch (const std::exception& e) {
                logger::warn("CharacterBios: could not read {} ({}); reading external plugins in folder order.",
                             registryPath.string(),
                             e.what());
                registry = nlohmann::json{};
            }

            const auto* plugins =
                registry.contains("plugins") && registry["plugins"].is_object() ? &registry["plugins"] : nullptr;
            const auto layerOf = [](const std::string& id, const nlohmann::json& entry) {
                const auto source = entry.is_object() ? entry.value("source", std::string{}) : std::string{};
                return kSkyrimNetRoot / (source == "external" ? "external" : "library") / id;
            };
            const auto enabled = [](const nlohmann::json& entry) {
                return entry.is_object() && entry.value("enabled", true) && !entry.value("missing", false);
            };

            // External layers SkyrimNet has not registered yet go on top.
            std::error_code ec;
            for (const auto& dir : std::filesystem::directory_iterator{kSkyrimNetRoot / "external", ec}) {
                const auto id = dir.path().filename().string();
                if (dir.is_directory(ec) && !(plugins && plugins->contains(id))) {
                    out.plugins.push_back(dir.path());
                }
            }

            if (plugins && registry.contains("priority") && registry["priority"].is_array()) {
                for (const auto& idJson : registry["priority"]) {
                    if (!idJson.is_string()) {
                        continue;
                    }
                    const auto id = idJson.get<std::string>();
                    if (const auto entry = plugins->find(id); entry != plugins->end() && enabled(*entry)) {
                        out.plugins.push_back(layerOf(id, *entry));
                    }
                }
            }

            if (plugins && registry.contains("pins") && registry["pins"].is_object()) {
                constexpr std::string_view kBioPrefix = "prompts/characters/";
                for (const auto& [file, idJson] : registry["pins"].items()) {
                    if (!file.starts_with(kBioPrefix) || !idJson.is_string()) {
                        continue;
                    }
                    const auto id = idJson.get<std::string>();
                    // A pin to an external layer SkyrimNet has not
                    // registered yet still names a folder on disk.
                    const auto entry = plugins->find(id);
                    const auto layer = entry != plugins->end() ? layerOf(id, *entry) : kSkyrimNetRoot / "external" / id;
                    if (entry == plugins->end() || enabled(*entry)) {
                        out.pins.emplace_back(file.substr(kBioPrefix.size()), layer);
                    }
                }
            }
            return out;
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
            // shared layers were used.
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
        const Bio g_empty;
    } // namespace

    void OnRevert()
    {
        g_bios.clear();
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
        g_census = Census{};

        Catalog catalog;

        // The save's own copies FIRST, so they win. SkyrimNet writes an
        // edited profile into the save's directory rather than over the
        // shared one, which is what lets two saves disagree about a
        // character; scanning any other layer first would make those
        // edits invisible.
        g_census.saveId = SkyrimNetAPI::GetSaveUniqueID();
        if (!g_census.saveId.empty()) {
            g_census.saveFiles = catalog.Scan(BiosIn(kSkyrimNetRoot / "saves" / g_census.saveId));
        } else {
            // A new game before SkyrimNet has minted an id, an older
            // SkyrimNet without the v7 export, or no SkyrimNet at all.
            // The shared profiles are still correct; only the save's
            // own edits are missing.
            logger::info("CharacterBios: no save id available; reading the shared profiles only.");
        }

        g_census.files = g_census.saveFiles + catalog.Scan(BiosIn(kSkyrimNetRoot / "overlay"));

        const auto layers = ReadPluginLayers();
        for (const auto& [file, layer] : layers.pins) {
            const auto path = BiosIn(layer) / file;
            std::error_code ec;
            if (std::filesystem::is_regular_file(path, ec)) {
                const auto before = catalog.Size();
                catalog.Add(path.stem().string(), path);
                g_census.files += catalog.Size() - before;
            }
        }
        for (const auto& layer : layers.plugins) {
            g_census.files += catalog.Scan(BiosIn(layer));
        }

        const auto baseBios = BiosIn(kSkyrimNetRoot / "library" / "skyrimnet.base");
        g_census.files += catalog.Scan(baseBios);
        g_census.generic = catalog.GenericCount();
        if (g_census.files == 0) {
            logger::warn("CharacterBios: no bio files under {} or any layer above it; plots will have no character "
                         "prose to work from.",
                         baseBios.string());
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
            ++g_census.loaded;
        }

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

} // namespace NarrativeEngine::CharacterBios
