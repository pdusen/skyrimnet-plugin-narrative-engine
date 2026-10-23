#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
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

} // namespace NarrativeEngine::CharacterBioResolver
