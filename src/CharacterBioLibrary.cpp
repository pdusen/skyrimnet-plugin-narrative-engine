#include <CharacterBios.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <CharacterBioResolver.h>
#include <GossipGraph.h>
#include <logger.h>
#include <PlotPopulation.h>
#include <SkyrimNetAPI.h>

// CharacterBioLibrary — the engine-bound half of CharacterBios.
//
// CharacterBioResolver holds SkyrimNet's rules and is pure. This file feeds
// it: the engine facts at session start on the main thread, and the disk at
// every refresh on the plot worker.
namespace NarrativeEngine::CharacterBios
{
    namespace
    {
        namespace Resolver = CharacterBioResolver;

        const std::filesystem::path kSkyrimNetRoot{"Data/SKSE/Plugins/SkyrimNet"};

        // --- Engine facts, once per save -------------------------------------

        struct Member
        {
            RE::FormID npc = 0;
            std::string name;
            Resolver::ActorFacts facts;
        };

        struct SessionFacts
        {
            std::vector<Member> members;
            // Every plugin the data handler knows that is flagged light,
            // case-folded: a unique translation row's id is masked by it.
            std::unordered_set<std::string> lightPlugins;
            // (case-folded plugin, id as the CSV gives it) -> TESFullName, for
            // every generic translation row in any layer.
            std::map<std::pair<std::string, std::uint32_t>, std::string> formNames;
        };

        // Published by the main thread, read by the plot worker. One
        // immutable snapshot, swapped whole, so a tick in flight across a
        // load keeps the copy it started with.
        std::atomic<std::shared_ptr<const SessionFacts>> g_facts;

        [[nodiscard]] std::string Folded(std::string_view text)
        {
            std::string out{text};
            std::ranges::transform(out, out.begin(), [](unsigned char c) {
                return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
            });
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

        [[nodiscard]] std::string FileName(const RE::TESForm* form)
        {
            const auto* file = form != nullptr ? form->GetFile(0) : nullptr;
            return file != nullptr ? std::string{file->GetFilename()} : std::string{};
        }

        // --- The disk ----------------------------------------------------------

        // Where a layer lives, relative to the SkyrimNet root.
        [[nodiscard]] std::filesystem::path LayerDirectory(const Resolver::Layer& layer)
        {
            switch (layer.kind) {
            case Resolver::LayerKind::Base:
            case Resolver::LayerKind::Library:
                return kSkyrimNetRoot / "library" / layer.folder;
            case Resolver::LayerKind::External:
                return kSkyrimNetRoot / "external" / layer.folder;
            case Resolver::LayerKind::Overlay:
                return kSkyrimNetRoot / "overlay";
            case Resolver::LayerKind::Save:
                return kSkyrimNetRoot / "saves" / layer.folder;
            }
            return {};
        }

        // One layer: its manifest, and every file under its prompts/, which
        // is all profile resolution ever asks about.
        [[nodiscard]] Resolver::Layer ScanLayer(Resolver::LayerKind kind, std::string folder)
        {
            Resolver::Layer layer;
            layer.kind = kind;
            layer.folder = std::move(folder);
            const auto directory = LayerDirectory(layer);

            const auto manifest = nlohmann::json::parse(ReadFile(directory / "manifest.json"), nullptr, false);
            if (manifest.is_object()) {
                layer.manifestId = manifest.value("id", std::string{});
                layer.version = manifest.value("version", std::string{});
            }

            std::error_code ec;
            const auto prompts = directory / "prompts";
            for (auto it = std::filesystem::recursive_directory_iterator{prompts, ec};
                 !ec && it != std::filesystem::recursive_directory_iterator{};
                 it.increment(ec)) {
                if (it->is_regular_file(ec)) {
                    layer.paths.insert(std::filesystem::relative(it->path(), directory, ec).generic_string());
                }
            }
            return layer;
        }

        // Every layer on disk. The save layer only when there is a save.
        [[nodiscard]] std::vector<Resolver::Layer> ScanLayers(const std::string& saveId)
        {
            std::vector<Resolver::Layer> layers;
            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator{kSkyrimNetRoot / "library", ec}) {
                if (entry.is_directory(ec)) {
                    const auto folder = entry.path().filename().string();
                    const auto folded = Folded(folder);
                    const auto kind = folded == Resolver::kBaseId || folded == Resolver::kBaseUpdateFolder
                                          ? Resolver::LayerKind::Base
                                          : Resolver::LayerKind::Library;
                    layers.push_back(ScanLayer(kind, folder));
                }
            }
            for (const auto& entry : std::filesystem::directory_iterator{kSkyrimNetRoot / "external", ec}) {
                if (entry.is_directory(ec)) {
                    layers.push_back(ScanLayer(Resolver::LayerKind::External, entry.path().filename().string()));
                }
            }
            if (std::filesystem::is_directory(kSkyrimNetRoot / "overlay", ec)) {
                layers.push_back(ScanLayer(Resolver::LayerKind::Overlay, "overlay"));
            }
            if (!saveId.empty() && std::filesystem::is_directory(kSkyrimNetRoot / "saves" / saveId, ec)) {
                layers.push_back(ScanLayer(Resolver::LayerKind::Save, saveId));
            }
            return layers;
        }

        [[nodiscard]] bool HasPrefix(std::string_view path, std::string_view prefix)
        {
            return path.size() >= prefix.size() && Folded(path.substr(0, prefix.size())) == prefix;
        }

        [[nodiscard]] bool IsCsv(std::string_view path)
        {
            return path.size() >= 4 && Folded(path.substr(path.size() - 4)) == ".csv";
        }

        // Every CSV under `prefix` in any layer, each read from the layer that
        // wins it.
        [[nodiscard]] std::vector<Resolver::CsvFile> TranslationFiles(const std::vector<Resolver::Layer>& layers,
                                                                      const Resolver::Stack& stack,
                                                                      std::string_view prefix)
        {
            std::set<std::string> paths;
            for (const auto& layer : layers) {
                for (const auto& path : layer.paths) {
                    if (HasPrefix(path, prefix) && IsCsv(path)) {
                        paths.insert(path);
                    }
                }
            }
            std::vector<Resolver::CsvFile> files;
            for (const auto& path : paths) {
                if (const auto* winner = Resolver::Winner(stack, path); winner != nullptr) {
                    files.push_back({path, ReadFile(LayerDirectory(*winner) / path)});
                }
            }
            return files;
        }

        // Does a profile hold anything but blocks? Reading blocks yields the
        // words a render would only while the answer is no.
        [[nodiscard]] bool HasTemplateLogic(std::string_view text)
        {
            if (text.find("{{") != std::string_view::npos) {
                return true;
            }
            for (auto at = text.find("{%"); at != std::string_view::npos; at = text.find("{%", at + 2)) {
                const auto tag = text.substr(at + 2, 12);
                if (!tag.starts_with(" block ") && !tag.starts_with(" endblock ")) {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] std::string_view TierName(Resolver::Tier tier)
        {
            switch (tier) {
            case Resolver::Tier::Dynamic:
                return "dynamic";
            case Resolver::Tier::Own:
                return "own";
            case Resolver::Tier::Generic:
                return "generic";
            case Resolver::Tier::Default:
                return "default";
            case Resolver::Tier::None:
                return "none";
            }
            return "none";
        }

        // --- What the plot worker holds ---------------------------------------

        struct Held
        {
            // The snapshot these were resolved from; a new one means a new
            // session, and every member is logged afresh.
            const SessionFacts* session = nullptr;
            std::unordered_map<RE::FormID, Bio> bios;
            std::unordered_map<RE::FormID, Resolver::Tier> tiers;
            // name|tier|layer|path per member, to log only what changed.
            std::unordered_map<RE::FormID, std::string> lastLine;
        };

        // Plot worker only.
        Held g_held;
        const Bio g_empty;
    } // namespace

    void OnSessionStart()
    {
        auto facts = std::make_shared<SessionFacts>();

        for (const auto& member : PlotPopulation::Get().members) {
            Member entry{member.npc, member.name, {}};
            entry.facts.refId = GossipGraph::ActorRefFor(member.npc);
            // The reference does not always resolve: for 185 of 808 members
            // on the first in-game run, LookupByID<RE::Actor> returned null.
            // Why is not established (see PlotPopulation::IsAlive). The BASE
            // record resolves, and it carries every fact but the display
            // name -- which, for a reference nobody has renamed, is the
            // base's name anyway, and is what SkyrimNet falls back to when
            // it is empty.
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(entry.facts.refId);
            const RE::TESNPC* base =
                actor != nullptr ? actor->GetActorBase() : RE::TESForm::LookupByID<RE::TESNPC>(member.npc);
            if (actor != nullptr) {
                if (const char* display = actor->GetDisplayFullName(); display != nullptr) {
                    entry.facts.displayName = display;
                }
                entry.facts.refPlugin = FileName(actor);
            }
            if (base != nullptr) {
                if (const char* name = base->GetFullName(); name != nullptr) {
                    entry.facts.baseName = name;
                }
                entry.facts.basePlugin = FileName(base);
                entry.facts.baseUnique = base->IsUnique();
            }
            facts->members.push_back(std::move(entry));
        }

        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        if (dataHandler != nullptr) {
            for (const auto* file : dataHandler->files) {
                if (file != nullptr && file->IsLight()) {
                    facts->lightPlugins.insert(Folded(file->GetFilename()));
                }
            }

            // The generic rows' names, for every generic CSV any layer holds:
            // which copy of a file wins can change during the session, the
            // names cannot.
            const auto layers = ScanLayers(SkyrimNetAPI::GetSaveUniqueID());
            std::vector<Resolver::CsvFile> genericFiles;
            for (const auto& layer : layers) {
                for (const auto& path : layer.paths) {
                    if (HasPrefix(path, "prompts/translation/generic/") && IsCsv(path)) {
                        genericFiles.push_back({path, ReadFile(LayerDirectory(layer) / path)});
                    }
                }
            }
            const auto record = [&](std::string_view plugin, std::uint32_t formId) {
                std::string name;
                if (const auto* form = dataHandler->LookupForm(formId, plugin); form != nullptr) {
                    if (const auto* full = form->As<RE::TESFullName>();
                        full != nullptr && full->GetFullName() != nullptr) {
                        name = full->GetFullName();
                    }
                }
                facts->formNames.insert_or_assign({Folded(plugin), formId}, name);
                return name;
            };
            // Built only for its calls to `record`; no unique rows, so the
            // light test is never asked.
            const auto notLight = [](std::string_view) { return false; };
            static_cast<void>(Resolver::BuildTables({}, std::move(genericFiles), notLight, record));
        }

        logger::info("CharacterBios: session facts for {} population member(s), {} light plugin(s), {} generic "
                     "translation row name(s).",
                     facts->members.size(),
                     facts->lightPlugins.size(),
                     facts->formNames.size());
        g_facts.store(std::move(facts));
    }

    void OnRevert()
    {
        g_facts.store(nullptr);
    }

    void Refresh(const PlotThread::Token&)
    {
        const auto facts = g_facts.load();
        if (!facts) {
            g_held = Held{};
            return;
        }
        const bool firstRefresh = g_held.session != facts.get();

        const auto saveId = SkyrimNetAPI::GetSaveUniqueID();
        const auto layers = ScanLayers(saveId);
        const auto registry = Resolver::ParseRegistry(ReadFile(kSkyrimNetRoot / "content-registry.json"));
        const auto stack = Resolver::BuildStack(layers, registry);

        const auto isLight = [&](std::string_view plugin) { return facts->lightPlugins.contains(Folded(plugin)); };
        const auto nameOf = [&](std::string_view plugin, std::uint32_t formId) {
            const auto it = facts->formNames.find({Folded(plugin), formId});
            return it == facts->formNames.end() ? std::string{} : it->second;
        };
        Resolver::TableCensus tableCensus;
        const auto tables = Resolver::BuildTables(TranslationFiles(layers, stack, "prompts/translation/unique/"),
                                                  TranslationFiles(layers, stack, "prompts/translation/generic/"),
                                                  isLight,
                                                  nameOf,
                                                  &tableCensus);

        if (firstRefresh) {
            std::string order;
            for (const auto* layer : stack.layers) {
                order += std::format("{}{}", order.empty() ? "" : ", ", layer->folder);
            }
            std::string pins;
            for (const auto& [path, id] : stack.pins) {
                pins += std::format("{}{} -> {}", pins.empty() ? "" : ", ", path, id);
            }
            logger::info("CharacterBios: save '{}'; {} layer(s) on disk; stack, highest first: {}; pins: {}.",
                         saveId,
                         layers.size(),
                         order,
                         pins.empty() ? std::string{"none"} : pins);
        }

        Held held;
        held.session = facts.get();
        std::size_t stored = 0;
        std::size_t computed = 0;
        std::size_t agreed = 0;
        std::size_t templated = 0;
        std::size_t changed = 0;
        std::map<Resolver::Tier, std::size_t> byTier;
        std::map<std::string, std::size_t> ownByLayer;

        for (const auto& member : facts->members) {
            // Computed for EVERYONE, registered or not. For somebody
            // SkyrimNet has registered, its stored name is the one used -- it
            // already reflects whatever has happened since -- and the
            // computed one is checked against it: every registered member is
            // a test of the naming rule on real engine data.
            const auto computedName = member.facts.refId != 0 ? Resolver::BioName(member.facts, tables) : std::string{};
            const auto storedName =
                member.facts.refId != 0 ? SkyrimNetAPI::GetBioTemplateName(member.facts.refId) : std::string{};
            const auto& bioName = storedName.empty() ? computedName : storedName;
            if (!storedName.empty()) {
                ++stored;
                if (storedName == computedName) {
                    ++agreed;
                } else if (firstRefresh) {
                    logger::info("CharacterBios: {} (base 0x{:X}, ref 0x{:X}) -- SkyrimNet stored '{}' but the rule "
                                 "computes '{}' (display '{}', base name '{}', base plugin '{}').",
                                 member.name,
                                 member.npc,
                                 member.facts.refId,
                                 storedName,
                                 computedName,
                                 member.facts.displayName,
                                 member.facts.baseName,
                                 member.facts.basePlugin);
                }
            } else if (!computedName.empty()) {
                ++computed;
            }

            const auto resolution = Resolver::Resolve(member.facts, bioName, tables, stack);
            ++byTier[resolution.tier];
            held.tiers.emplace(member.npc, resolution.tier);
            if (resolution.layer != nullptr) {
                const auto text = ReadFile(LayerDirectory(*resolution.layer) / resolution.path);
                templated += HasTemplateLogic(text) ? 1 : 0;
                held.bios.emplace(member.npc, ParseBio(text));
                if (resolution.tier == Resolver::Tier::Own) {
                    ++ownByLayer[resolution.layer->folder];
                }
            }

            auto line = std::format("{}|{}|{}|{}",
                                    bioName,
                                    TierName(resolution.tier),
                                    resolution.layer != nullptr ? resolution.layer->folder : std::string{},
                                    resolution.path);
            const auto previous = g_held.lastLine.find(member.npc);
            if (firstRefresh || previous == g_held.lastLine.end() || previous->second != line) {
                ++changed;
                // Tab-separated key=value, free text last, so a session's log
                // can be checked mechanically against the disk and SkyrimNet.
                logger::debug("CharacterBios: member\tbase=0x{:X}\tref=0x{:X}\ttier={}\tlayer={}\tpath={}\tstored={}"
                              "\tcomputed={}\tunique={}\tbasePlugin={}\trefPlugin={}\tname={}\tdisplay={}\tbaseName={}",
                              member.npc,
                              member.facts.refId,
                              TierName(resolution.tier),
                              resolution.layer != nullptr ? resolution.layer->folder : std::string{},
                              resolution.path,
                              storedName,
                              computedName,
                              member.facts.baseUnique ? 1 : 0,
                              member.facts.basePlugin,
                              member.facts.refPlugin,
                              member.name,
                              member.facts.displayName,
                              member.facts.baseName);
            }
            held.lastLine.emplace(member.npc, std::move(line));
        }

        std::string layerCounts;
        for (const auto& [folder, count] : ownByLayer) {
            layerCounts += std::format("{}{} {}", layerCounts.empty() ? "" : ", ", count, folder);
        }
        logger::info(
            "CharacterBios: resolved {} member(s) -- {} by SkyrimNet's stored name ({} of them computed "
            "identically), {} computed; {} dynamic, {} own, {} generic, {} default, {} none; own by layer: {}. "
            "{} layer(s) stacked, {} unique and {} generic translation row(s), {} profile(s) with template "
            "logic, {} resolution(s) changed{}.",
            facts->members.size(),
            stored,
            agreed,
            computed,
            byTier[Resolver::Tier::Dynamic],
            byTier[Resolver::Tier::Own],
            byTier[Resolver::Tier::Generic],
            byTier[Resolver::Tier::Default],
            byTier[Resolver::Tier::None],
            layerCounts.empty() ? std::string{"none"} : layerCounts,
            stack.layers.size(),
            tableCensus.uniqueRows,
            tableCensus.genericRows,
            templated,
            changed,
            firstRefresh ? " (first refresh this session)" : "");
        if (byTier[Resolver::Tier::None] == facts->members.size() && !facts->members.empty()) {
            logger::warn("CharacterBios: nobody resolved to any profile, not even characters/default. Is SkyrimNet's "
                         "content library at {}?",
                         kSkyrimNetRoot.string());
        }
        g_held = std::move(held);
    }

    bool Has(RE::FormID npc)
    {
        const auto tier = g_held.tiers.find(npc);
        return tier != g_held.tiers.end()
               && (tier->second == Resolver::Tier::Dynamic || tier->second == Resolver::Tier::Own);
    }

    const Bio& For(RE::FormID npc)
    {
        const auto found = g_held.bios.find(npc);
        return found == g_held.bios.end() ? g_empty : found->second;
    }

} // namespace NarrativeEngine::CharacterBios
