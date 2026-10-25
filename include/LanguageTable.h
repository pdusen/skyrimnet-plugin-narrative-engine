#pragma once

#include <TextKeys.h>
#include <TextTemplate.h>

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
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

        // The parsed template for `key`, or null when it has none.
        [[nodiscard]] const TextTemplate::Message* Find(const TextKeys::Key& key) const;

        // `key` rendered with this language's grammar. A key with no text
        // renders empty.
        [[nodiscard]] std::string Render(const TextKeys::Key& key,
                                         const TextTemplate::Arguments& arguments,
                                         bool sentence = false,
                                         std::function<std::string(std::string_view name)> missing = {}) const;

    private:
        Header header_;
        TextTemplate::Grammar grammar_;
        Census census_;
        std::unordered_map<std::string, TextTemplate::Message> messages_;
    };
} // namespace NarrativeEngine::LanguageTable
