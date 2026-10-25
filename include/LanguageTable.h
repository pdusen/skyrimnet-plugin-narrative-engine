#pragma once

#include <TextKeys.h>
#include <TextMatch.h>
#include <TextTemplate.h>

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// One language's text, loaded, validated and immutable.
//
// A table is built from up to three INI texts: English, which is the
// reference and always present; the shipped file for the chosen language, when
// that is not English; and the player's never-shipped overlay
// (`<stem>.custom.ini`), when one exists. Every declared key is taken from the
// first of overlay, shipped file and English that both has it and passes
// validation, so one bad line costs that line and nothing else.
//
// Pure: it reads text, not files, and touches no engine. Reading the files and
// choosing which language to load belongs to Localization.
namespace NarrativeEngine::LanguageTable
{
    enum class Status
    {
        Draft,
        Reviewed,
    };

    // `[Language]`: who the file says it is.
    struct Header
    {
        // The English name, for telling a model which language to write in.
        std::string name;
        // The language's own name for itself, for the MCM and the dashboard.
        std::string nativeName;
        // Bare ISO 639-1, the form SkyrimNet uses.
        std::string code;
        Status status = Status::Draft;
        // The vanilla font sets whose glyphs cover the language: en, pl, ru, ja, cn.
        std::vector<std::string> fonts;
    };

    // What building the table found, logged once per session.
    struct Census
    {
        // Declared keys that have text, and where it came from.
        std::size_t keys = 0;
        std::size_t fromOverlay = 0;
        std::size_t fromShipped = 0;
        std::size_t fromEnglish = 0;

        // A key the shipped file or the overlay lacked or got wrong, so a
        // later source supplied it. Missing keys are expected in a draft.
        std::vector<std::string> fellBack;

        // Something a file says that cannot be used: a template that does not
        // parse or validate, a header or grammar value that means nothing, a
        // key no part of the plugin declares. A shipped file should have none.
        std::vector<std::string> rejected;

        // Overlay lines that override nothing: almost always a typo, which
        // would otherwise look like a rule in force.
        std::vector<std::string> overlayUnmatched;

        // Declared keys not even English can supply. Should never happen.
        std::vector<std::string> unavailable;

        // [Matching] checks the language turns off, each with the file's
        // reason: a check that cannot be said in this language is off on
        // purpose, never by omission.
        std::vector<std::string> disabled;
    };

    struct Sources
    {
        std::string_view english;
        // Empty when the chosen language is English itself.
        std::optional<std::string_view> shipped;
        std::optional<std::string_view> overlay;
    };

    class Table
    {
    public:
        // `keys` is the full declared list (TextKeys::All() in the plugin).
        [[nodiscard]] static Table Build(const Sources& sources, std::span<const TextKeys::Key> keys);

        [[nodiscard]] const Header& GetHeader() const noexcept
        {
            return header_;
        }
        [[nodiscard]] const TextTemplate::Grammar& Grammar() const noexcept
        {
            return grammar_;
        }
        [[nodiscard]] const Census& GetCensus() const noexcept
        {
            return census_;
        }

        // The content checks and word lists of [Matching], in this language.
        // A check, or a plain list, is taken whole from the first of overlay
        // and shipped file that says anything of it, else from English: a
        // language's own rules are never mixed with English's for the same
        // check.
        [[nodiscard]] const TextMatch::Rules& Matching() const noexcept
        {
            return matching_;
        }

        // The shape [Prompt.Form] asks of a content field spliced into one of
        // this language's templates ("an infinitive clause ending in zu ..."),
        // or null. Read from the language's own file and overlay only:
        // English's would describe an English phrase.
        [[nodiscard]] const std::string* PromptForm(std::string_view field) const;

        // [Dashboard]: every key English's file declares there, with this
        // language's template for it as written, in English's order. The
        // dashboard renders them itself, through its port of the formatter.
        [[nodiscard]] std::vector<std::pair<std::string, std::string>> Strings() const;

        // The parsed template for `key`, or null when it has none.
        [[nodiscard]] const TextTemplate::Message* Find(const TextKeys::Key& key) const;

        // The same template as its file wrote it, for checks that read the
        // words rather than render them.
        [[nodiscard]] const std::string* Source(const TextKeys::Key& key) const;

        // Display text for a row of one of the plugin's data files, from the
        // open-ended [Data.*] sections ([Data.Faction], [Data.Item],
        // [Data.AttackerGroup]), keyed by the row's own id. Null when no file
        // overrides it, and the data file's own text stands. Open-ended
        // because modded rows are first-class: no key list could name them.
        [[nodiscard]] const std::string* DataText(std::string_view section, std::string_view id) const;

        // Every override in `section` whose id is none of `ids`, as written:
        // a typo, or a row from a mod that is not installed. The loader that
        // knows the ids reports these, since the table cannot.
        [[nodiscard]] std::vector<std::string> UnmatchedData(std::string_view section,
                                                             std::span<const std::string> ids) const;

        // `key` rendered with this language's grammar. A key with no text
        // renders empty.
        [[nodiscard]] std::string Render(const TextKeys::Key& key,
                                         const TextTemplate::Arguments& arguments,
                                         bool sentence = false,
                                         std::function<std::string(std::string_view name)> missing = {}) const;

    private:
        Header header_;
        TextTemplate::Grammar grammar_;
        TextMatch::Rules matching_;
        std::unordered_map<std::string, std::string> promptForms_;
        std::vector<std::string> dashboardKeys_;
        Census census_;
        std::unordered_map<std::string, TextTemplate::Message> messages_;
        std::unordered_map<std::string, std::string> sources_;
        // [Data.*] overrides by Id(section, key), and each key as written.
        std::unordered_map<std::string, std::string> data_;
        std::vector<std::pair<std::string, std::string>> dataKeys_;
    };
} // namespace NarrativeEngine::LanguageTable
