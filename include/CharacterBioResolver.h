#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// CharacterBioResolver — the profile SkyrimNet would render for an NPC,
// worked out without asking SkyrimNet.
//
// SkyrimNet hands a profile out only through a UUID, and mints a UUID when
// the player MEETS someone. Plots need profiles for people the player has
// never met. The way round that is to do what SkyrimNet does, exactly, from
// the same inputs -- and the rules for that were decoded from its DLL:
//
//   docs/engine-findings/skyrimnet-bio-template-name-assignment.md
//   docs/engine-findings/skyrimnet-character-profile-lookup.md
//
// Those documents are the contract. Every function here is pure: the engine
// facts and the file contents arrive as plain data, so the unit tests drive
// the real object file with no game.
namespace NarrativeEngine::CharacterBioResolver
{
    // --- The bio name -----------------------------------------------------
    //
    // SkyrimNet COMPUTES an actor's bio name (the stem of
    // prompts/characters/<name>.prompt) once, when it first registers them,
    // and never checks a file exists. UUIDResolver::AddMapping.

    // What the engine knows about one actor, as SkyrimNet reads it.
    struct ActorFacts
    {
        // The REFERENCE form id. Never the base NPC's: the suffix is the low
        // twelve bits of this, and the unique table is keyed by it.
        std::uint32_t refId = 0;
        // TESObjectREFR::GetDisplayFullName() on the reference, raw bytes.
        std::string displayName;
        // The base NPC's TESFullName, the fallback when the display name is
        // empty.
        std::string baseName;
        // The file name of the base NPC's GetFile(0) -- the base's, NOT the
        // reference's. Brand-Shei's reference reports USSEP; her row is
        // keyed Skyrim.esm.
        std::string basePlugin;
        // The reference's own GetFile(0), used only when the base has none.
        std::string refPlugin;
        // The base NPC's kUnique flag. A unique actor never falls back to a
        // _generic profile.
        bool baseUnique = false;
    };

    // The two translation tables, already built (from the CSVs under
    // prompts/translation/). Keyed exactly as the lookups below form keys.
    struct Tables
    {
        // UniqueKey(plugin, local id) -> English key.
        std::unordered_map<std::string, std::string> unique;
        // Slug(Trim(localized name)) -> English key.
        std::unordered_map<std::string, std::string> generic;
    };

    inline constexpr std::uint32_t kPlayerRefId = 0x14;

    // The local part of a form id: twelve bits for a light plugin (high
    // byte 0xFE), twenty-four otherwise.
    [[nodiscard]] std::uint32_t LocalId(std::uint32_t formId) noexcept;

    // The unique table's key: the plugin trimmed and ASCII-lowercased, and
    // the local id as six uppercase hex digits.
    [[nodiscard]] std::string UniqueKey(std::string_view plugin, std::uint32_t localId);

    // Strip " \t\n\r\f\v" from both ends.
    [[nodiscard]] std::string_view Trim(std::string_view text) noexcept;

    // ASCII-lowercase, then ' ', '\'', '\\' and '/' become '_'. Every other
    // byte is kept: hyphens, brackets, digits and UTF-8 alike. NOT trimmed --
    // the generic lookup trims first and the fallback does not.
    [[nodiscard]] std::string Slug(std::string_view text);

    // Valid UTF-8 comes back untouched. Otherwise every byte that does not
    // begin a valid UTF-8 sequence is read as Windows-1252 (0x80-0x9F through
    // its table, the rest as Latin-1) and re-encoded; valid sequences around
    // it are kept.
    [[nodiscard]] std::string RepairWindows1252(std::string_view text);

    // The bio name SkyrimNet assigns this actor, or "" when it assigns none.
    // In order: the unique table, the generic table, player_special for the
    // player, nothing for an empty name or "Unknown", then the slug -- each
    // but player_special suffixed with _{refId & 0xFFF:03X}.
    [[nodiscard]] std::string BioName(const ActorFacts& facts, const Tables& tables);

    // --- The translation tables -------------------------------------------
    //
    // prompts/translation/unique/*.csv and prompts/translation/generic/*.csv,
    // as PromptNameTranslation loads them. Which files are in play is the
    // layer stack's business; these take the files already chosen.

    struct CsvFile
    {
        // The resolved content path. Used only for ORDER: files are applied
        // in byte-wise path order, and a later row replaces an earlier one
        // with the same key.
        std::string path;
        std::string text;
    };

    // Whether a plugin is loaded as a light plugin. A unique row's id is
    // masked to twelve bits when it is and to twenty-four otherwise --
    // including when the plugin is not loaded at all.
    using IsLightPlugin = std::function<bool(std::string_view plugin)>;

    // A form's TESFullName, raw, looked up by plugin and the id as the CSV
    // gives it; "" when the form does not resolve, which drops the row.
    using FormName = std::function<std::string(std::string_view plugin, std::uint32_t formId)>;

    // What the build kept and dropped, for the census.
    struct TableCensus
    {
        std::size_t uniqueRows = 0;     // applied, overrides included
        std::size_t genericRows = 0;    // applied, overrides included
        std::size_t genericUnnamed = 0; // form did not resolve, or has no name
        std::size_t skipped = 0;        // comment, blank, short, bad id, no key
    };

    // A CSV id as SkyrimNet parses one: trimmed, an optional 0x or 0X, then
    // one to eight hex digits and nothing else.
    [[nodiscard]] std::optional<std::uint32_t> ParseHexId(std::string_view text) noexcept;

    // Both tables from their files. The first line of each file is its
    // header; a row whose trimmed plugin cell is empty or starts with '#' is
    // skipped, as is one with fewer than three cells, a bad id or an empty
    // key.
    [[nodiscard]] Tables BuildTables(std::vector<CsvFile> uniqueFiles,
                                     std::vector<CsvFile> genericFiles,
                                     const IsLightPlugin& isLight,
                                     const FormName& nameOf,
                                     TableCensus* census = nullptr);

    // --- The layer stack --------------------------------------------------
    //
    // SkyrimNet's content store: content-registry.json, the layer folders
    // under SKSE/Plugins/SkyrimNet, and which layer wins each path.
    // Content::BuildLayerStack and Content::ResolveSnapshot.

    // In SkyrimNet's own enum order.
    enum class LayerKind : std::uint8_t
    {
        Base,     // library/skyrimnet.base, or library/skyrimnet.base.update
        Library,  // library/<id>
        External, // external/<id>
        Overlay,  // overlay
        Save      // saves/<save id>, only while a save is active
    };

    inline constexpr std::string_view kBaseId = "skyrimnet.base";
    inline constexpr std::string_view kBaseUpdateFolder = "skyrimnet.base.update";

    struct Layer
    {
        LayerKind kind = LayerKind::External;
        // The directory's own name.
        std::string folder;
        // manifest.json's "id" and "version"; "" when absent.
        std::string manifestId;
        std::string version;
        // Every file the layer holds, relative to it, '/'-separated and in
        // the case the disk has: "prompts/characters/grelka_0B3.prompt".
        std::unordered_set<std::string> paths;
    };

    // The registry as SkyrimNet reads it: case-folded duplicates dropped, a
    // non-boolean "enabled" taken as true, and every record given a priority
    // entry, the ones the file lacks appended at the bottom in key order.
    struct Registry
    {
        struct Plugin
        {
            bool enabled = true;
            bool missing = false;
        };
        std::map<std::string, Plugin> plugins;
        // Highest first.
        std::vector<std::string> priority;
        // Content path -> the plugin id chosen to provide it.
        std::map<std::string, std::string, std::less<>> pins;
    };

    // An unreadable file, or one with a schema_version above 1, is an empty
    // registry: SkyrimNet quarantines it and starts empty.
    [[nodiscard]] Registry ParseRegistry(std::string_view json);

    // -1, 0 or 1. Semantic versioning precedence; a version that does not
    // parse counts as 0.0.0.
    [[nodiscard]] int CompareVersions(std::string_view a, std::string_view b);

    struct Stack
    {
        // Highest first: the save layer, the overlay, plugins in priority
        // order, then skyrimnet.base.
        std::vector<const Layer*> layers;
        std::map<std::string, std::string, std::less<>> pins;
    };

    // Left out of the stack: a plugin layer with no registry record, a
    // disabled or missing one, and a library or base folder whose manifest
    // does not carry its id. Of two copies of one id, the higher version
    // stays; on a tie a library copy beats an external one, and
    // skyrimnet.base beats skyrimnet.base.update.
    //
    // The returned stack points into `layers`, which must outlive it.
    [[nodiscard]] Stack BuildStack(const std::vector<Layer>& layers, const Registry& registry);

    // The layer that provides `path`, or nullptr when none does: the first
    // in the stack that holds it -- outright when that is the save layer --
    // unless a pin names another layer that holds it.
    [[nodiscard]] const Layer* Winner(const Stack& stack, std::string_view path);

    // --- The profile ------------------------------------------------------
    //
    // render_character_profile's chain, from a bio name to the file it
    // renders. DecoratorLibrary::Initialize, lambda 3.

    enum class Tier : std::uint8_t
    {
        Dynamic, // characters/dynamic/<name>: LLM-written, this save only
        Own,     // characters/<name>: written for this person
        Generic, // characters/<key>_generic, by bracketed tag or by name
        Default, // characters/default
        None     // nothing resolves, or the actor has no bio name
    };

    struct Resolution
    {
        Tier tier = Tier::None;
        // The content path that won, "prompts/...", and the layer providing it.
        std::string path;
        const Layer* layer = nullptr;
    };

    // The profile SkyrimNet renders for an actor with this bio name. An
    // empty name resolves to None: SkyrimNet stops there rather than falling
    // back.
    [[nodiscard]] Resolution Resolve(const ActorFacts& facts,
                                     std::string_view bioName,
                                     const Tables& tables,
                                     const Stack& stack);

} // namespace NarrativeEngine::CharacterBioResolver
