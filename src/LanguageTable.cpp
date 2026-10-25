#include <LanguageTable.h>

#include <TextUtf8.h>

#include <algorithm>
#include <array>
#include <format>
#include <memory>
#include <unordered_set>

// SimpleIni's default converter on Windows is Win32's, and that pulls in
// <windows.h>. Every other user of SimpleIni in the plugin gets the same
// converter, so it stays the default here rather than becoming a second,
// differently-defined instantiation of the same template.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <SimpleIni.h>

namespace NarrativeEngine::LanguageTable
{
    namespace
    {
        constexpr std::string_view kLanguageSection = "Language";
        constexpr std::string_view kGrammarSection = "Grammar";
        constexpr std::string_view kDataPrefix = "data.";
        constexpr std::string_view kMatchingSection = "Matching";
        constexpr std::string_view kPromptFormSection = "Prompt.Form";

        constexpr std::array<std::string_view, 6> kHeaderKeys =
            {"Name", "NativeName", "Code", "Status", "PluralRule", "Fonts"};
        constexpr std::array<std::string_view, 9> kGrammarKeys = {"ListSeparator",
                                                                  "ListPairSeparator",
                                                                  "ListFinalSeparator",
                                                                  "CapitalizeSentenceStart",
                                                                  "PhraseTrailingPunctuation",
                                                                  "PhraseLowercaseFirst",
                                                                  "IndefiniteArticle",
                                                                  "IndefiniteArticleBeforeVowel",
                                                                  "WordSeparated"};

        [[nodiscard]] char AsciiLower(char c) noexcept
        {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        }

        [[nodiscard]] std::string Lowered(std::string_view text)
        {
            std::string out{text};
            std::ranges::transform(out, out.begin(), AsciiLower);
            return out;
        }

        [[nodiscard]] bool SameName(std::string_view a, std::string_view b) noexcept
        {
            return std::ranges::equal(a, b, [](char l, char r) { return AsciiLower(l) == AsciiLower(r); });
        }

        template <std::size_t N>
        [[nodiscard]] bool Contains(const std::array<std::string_view, N>& names, std::string_view name) noexcept
        {
            return std::ranges::any_of(names, [name](std::string_view known) { return SameName(known, name); });
        }

        // Map key: section and name, case-folded the way SimpleIni folds them.
        [[nodiscard]] std::string Id(std::string_view section, std::string_view name)
        {
            return Lowered(section) + "/" + Lowered(name);
        }

        [[nodiscard]] std::string Where(std::string_view section, std::string_view name)
        {
            return std::format("[{}] {}", section, name);
        }

        [[nodiscard]] std::string_view Trimmed(std::string_view text) noexcept
        {
            constexpr std::string_view kSpace = " \t\r\n";
            const auto first = text.find_first_not_of(kSpace);
            if (first == std::string_view::npos) {
                return {};
            }
            return text.substr(first, text.find_last_not_of(kSpace) - first + 1);
        }

        // A grammar value wrapped in double quotes keeps its whitespace, which
        // SimpleIni would otherwise trim: `ListSeparator = ", "`.
        [[nodiscard]] std::string Unquoted(std::string_view value)
        {
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
                return std::string{value.substr(1, value.size() - 2)};
            }
            return std::string{value};
        }

        [[nodiscard]] std::optional<bool> ParseBool(std::string_view value)
        {
            const auto lowered = Lowered(Trimmed(value));
            if (lowered == "true" || lowered == "yes" || lowered == "1") {
                return true;
            }
            if (lowered == "false" || lowered == "no" || lowered == "0") {
                return false;
            }
            return std::nullopt;
        }

        [[nodiscard]] std::optional<TextTemplate::PluralRule> ParsePluralRule(std::string_view value)
        {
            using enum TextTemplate::PluralRule;
            const auto lowered = Lowered(Trimmed(value));
            if (lowered == "none") {
                return None;
            }
            if (lowered == "one_other") {
                return OneOther;
            }
            if (lowered == "french") {
                return French;
            }
            if (lowered == "russian") {
                return Russian;
            }
            if (lowered == "polish") {
                return Polish;
            }
            return std::nullopt;
        }

        [[nodiscard]] std::optional<Status> ParseStatus(std::string_view value)
        {
            const auto lowered = Lowered(Trimmed(value));
            if (lowered == "draft") {
                return Status::Draft;
            }
            if (lowered == "reviewed") {
                return Status::Reviewed;
            }
            return std::nullopt;
        }

        [[nodiscard]] std::vector<std::string> SplitList(std::string_view value)
        {
            std::vector<std::string> out;
            std::size_t start = 0;
            while (start <= value.size()) {
                auto end = value.find(',', start);
                if (end == std::string_view::npos) {
                    end = value.size();
                }
                const auto item = Trimmed(value.substr(start, end - start));
                if (!item.empty()) {
                    out.push_back(Lowered(item));
                }
                start = end + 1;
            }
            return out;
        }

        // One loaded INI text, labelled for the census.
        struct LoadedIni
        {
            std::string_view label;
            std::unique_ptr<CSimpleIniA> ini;

            [[nodiscard]] std::optional<std::string> Value(std::string_view section, std::string_view name) const
            {
                if (!ini) {
                    return std::nullopt;
                }
                const auto* value = ini->GetValue(std::string{section}.c_str(), std::string{name}.c_str(), nullptr);
                return value != nullptr ? std::optional<std::string>{value} : std::nullopt;
            }
        };

        [[nodiscard]] LoadedIni Load(std::string_view label, std::optional<std::string_view> text, Census& census)
        {
            LoadedIni out{label, nullptr};
            if (!text) {
                return out;
            }
            if (!TextUtf8::IsValid(*text)) {
                census.rejected.push_back(std::format("{}: the file is not valid UTF-8", label));
                return out;
            }
            auto ini = std::make_unique<CSimpleIniA>();
            ini->SetUnicode(true);
            ini->SetMultiLine(true);
            if (ini->LoadData(text->data(), text->size()) < 0) {
                census.rejected.push_back(std::format("{}: the file does not parse as INI", label));
                return out;
            }
            out.ini = std::move(ini);
            return out;
        }

        // Every (section, key) a source holds, in file order.
        [[nodiscard]] std::vector<std::pair<std::string, std::string>> Entries(const LoadedIni& source)
        {
            std::vector<std::pair<std::string, std::string>> out;
            if (!source.ini) {
                return out;
            }
            CSimpleIniA::TNamesDepend sections;
            source.ini->GetAllSections(sections);
            sections.sort(CSimpleIniA::Entry::LoadOrder());
            for (const auto& section : sections) {
                CSimpleIniA::TNamesDepend keys;
                source.ini->GetAllKeys(section.pItem, keys);
                keys.sort(CSimpleIniA::Entry::LoadOrder());
                for (const auto& key : keys) {
                    out.emplace_back(section.pItem, key.pItem);
                }
            }
            return out;
        }
    } // namespace

    Table Table::Build(const Sources& sources, std::span<const TextKeys::Key> keys)
    {
        Table table;
        auto& census = table.census_;

        const auto english = Load("english", sources.english, census);
        const auto shipped = Load("shipped", sources.shipped, census);
        const auto overlay = Load("overlay", sources.overlay, census);
        const bool hasShipped = shipped.ini != nullptr;

        // The header is the language's identity, so it comes from the
        // language's own file. A field that file lacks is a rejection, and
        // English stands in for it so the table still works.
        const LoadedIni& identity = hasShipped ? shipped : english;
        const auto headerValue = [&](std::string_view name) -> std::string {
            if (auto value = identity.Value(kLanguageSection, name)) {
                return std::string{Trimmed(*value)};
            }
            census.rejected.push_back(std::format("{}: {} is missing", identity.label, Where(kLanguageSection, name)));
            if (&identity != &english) {
                if (auto value = english.Value(kLanguageSection, name)) {
                    return std::string{Trimmed(*value)};
                }
            }
            return {};
        };
        table.header_.name = headerValue("Name");
        table.header_.nativeName = headerValue("NativeName");
        table.header_.code = Lowered(headerValue("Code"));
        table.header_.fonts = SplitList(headerValue("Fonts"));
        if (const auto status = headerValue("Status"); !status.empty()) {
            if (const auto parsed = ParseStatus(status)) {
                table.header_.status = *parsed;
            } else {
                census.rejected.push_back(std::format("{}: {} '{}' is neither draft nor reviewed",
                                                      identity.label,
                                                      Where(kLanguageSection, "Status"),
                                                      status));
            }
        }
        if (const auto rule = headerValue("PluralRule"); !rule.empty()) {
            if (const auto parsed = ParsePluralRule(rule)) {
                table.grammar_.pluralRule = *parsed;
            } else {
                census.rejected.push_back(std::format(
                    "{}: {} '{}' is not a plural rule", identity.label, Where(kLanguageSection, "PluralRule"), rule));
            }
        }

        // Grammar values fall back key by key, like templates: overlay, then
        // the language's file, then English.
        const std::array<const LoadedIni*, 3> order = {&overlay, &shipped, &english};
        const auto grammarValue = [&]<typename Parse>(std::string_view name, Parse parse) {
            for (const auto* source : order) {
                const auto value = source->Value(kGrammarSection, name);
                if (!value) {
                    continue;
                }
                if (auto parsed = parse(*value)) {
                    return parsed;
                }
                census.rejected.push_back(std::format(
                    "{}: {} '{}' is not a usable value", source->label, Where(kGrammarSection, name), *value));
            }
            return decltype(parse(std::string{})){};
        };
        const auto text = [](const std::string& value) { return std::optional<std::string>{Unquoted(value)}; };
        auto& grammar = table.grammar_;
        if (auto v = grammarValue("ListSeparator", text)) {
            grammar.listSeparator = *v;
        }
        if (auto v = grammarValue("ListPairSeparator", text)) {
            grammar.listPairSeparator = *v;
        }
        if (auto v = grammarValue("ListFinalSeparator", text)) {
            grammar.listFinalSeparator = *v;
        }
        if (auto v = grammarValue("PhraseTrailingPunctuation", text)) {
            grammar.phraseTrailingPunctuation = *v;
        }
        if (auto v = grammarValue("IndefiniteArticle", text)) {
            grammar.indefiniteArticle = *v;
        }
        if (auto v = grammarValue("IndefiniteArticleBeforeVowel", text)) {
            grammar.indefiniteArticleBeforeVowel = *v;
        }
        if (auto v = grammarValue("CapitalizeSentenceStart", ParseBool)) {
            grammar.capitalizeSentenceStart = *v;
        }
        if (auto v = grammarValue("PhraseLowercaseFirst", ParseBool)) {
            grammar.phraseLowercaseFirst = *v;
        }
        if (auto v = grammarValue("WordSeparated", ParseBool)) {
            grammar.wordSeparated = *v;
        }

        // [Matching]: open-ended, since which keys a check needs depends on
        // the language -- English rejects at the start, German requires at the
        // end. A check (or a plain list) is one unit, taken from the overlay
        // and the language's file when either says anything of it, and from
        // English only when neither does.
        {
            auto& rules = table.matching_;
            rules.options.wordSeparated = grammar.wordSeparated;
            const auto unitOf = [](std::string_view key) {
                const auto name = TextMatch::CheckName(key);
                return std::string{name.empty() ? key : name};
            };
            std::unordered_set<std::string> local;
            for (const auto* source : {&overlay, &shipped}) {
                for (const auto& [section, name] : Entries(*source)) {
                    if (SameName(section, kMatchingSection)) {
                        local.insert(unitOf(name));
                    }
                }
            }
            std::unordered_set<std::string> set;
            for (const auto* source : order) {
                for (const auto& [section, name] : Entries(*source)) {
                    if (!SameName(section, kMatchingSection) || set.contains(name)) {
                        continue;
                    }
                    if (source == &english && local.contains(unitOf(name))) {
                        continue;
                    }
                    if (const auto value = source->Value(section, name)) {
                        rules.Set(name, *value);
                        set.insert(name);
                    }
                }
            }
            for (const auto& [name, check] : rules.checks) {
                if (!check.disabled.empty()) {
                    census.disabled.push_back(std::format("{}: {}", name, check.disabled));
                }
            }
        }

        // [Prompt.Form]: open-ended by field, and never from English.
        for (const auto* source : {&overlay, &shipped}) {
            for (const auto& [section, name] : Entries(*source)) {
                if (!SameName(section, kPromptFormSection) || table.promptForms_.contains(Lowered(name))) {
                    continue;
                }
                if (const auto value = source->Value(section, name)) {
                    table.promptForms_.emplace(Lowered(name), std::string{Trimmed(*value)});
                }
            }
        }

        // Every declared key, from the first source that has it right.
        std::unordered_set<std::string> declared;
        for (const auto& key : keys) {
            declared.insert(Id(key.section, key.name));
            const auto signature = TextKeys::SignatureOf(key);
            bool found = false;
            for (const auto* source : order) {
                const auto value = source->Value(key.section, key.name);
                if (!value) {
                    if (source == &shipped && hasShipped) {
                        census.fellBack.push_back(
                            std::format("{}: missing from the language's file", Where(key.section, key.name)));
                    }
                    continue;
                }
                auto parsed = TextTemplate::Parse(*value);
                auto problems =
                    parsed.Ok() ? TextTemplate::Validate(parsed.message, signature) : std::move(parsed.problems);
                if (!problems.empty()) {
                    census.rejected.push_back(
                        std::format("{}: {}: {}", source->label, Where(key.section, key.name), problems.front()));
                    if (source != &english) {
                        census.fellBack.push_back(
                            std::format("{}: the {} text was rejected", Where(key.section, key.name), source->label));
                    }
                    continue;
                }
                table.messages_.emplace(Id(key.section, key.name), std::move(parsed.message));
                table.sources_.emplace(Id(key.section, key.name), *value);
                ++census.keys;
                if (source == &overlay) {
                    ++census.fromOverlay;
                } else if (source == &shipped) {
                    ++census.fromShipped;
                } else {
                    ++census.fromEnglish;
                }
                found = true;
                break;
            }
            if (!found) {
                census.unavailable.push_back(Where(key.section, key.name));
            }
        }

        // [Data.*]: open-ended, so not checked against any key list. Each
        // key is taken from the first of overlay, shipped file and English
        // that has it.
        const auto isData = [](std::string_view section) { return Lowered(section).starts_with(kDataPrefix); };
        for (const auto* source : order) {
            for (const auto& [section, name] : Entries(*source)) {
                if (!isData(section)) {
                    continue;
                }
                const auto id = Id(section, name);
                if (table.data_.contains(id)) {
                    continue;
                }
                if (const auto value = source->Value(section, name)) {
                    table.data_.emplace(id, std::string{Trimmed(*value)});
                    table.dataKeys_.emplace_back(section, name);
                }
            }
        }

        // Lines nothing reads.
        const auto known = [&](std::string_view section, std::string_view name) {
            if (isData(section) || SameName(section, kMatchingSection) || SameName(section, kPromptFormSection)) {
                return true;
            }
            if (SameName(section, kLanguageSection)) {
                return Contains(kHeaderKeys, name);
            }
            if (SameName(section, kGrammarSection)) {
                return Contains(kGrammarKeys, name);
            }
            return declared.contains(Id(section, name));
        };
        for (const auto* source : {&english, &shipped}) {
            for (const auto& [section, name] : Entries(*source)) {
                if (!known(section, name)) {
                    census.rejected.push_back(std::format(
                        "{}: {} is not a key this version of the plugin knows", source->label, Where(section, name)));
                }
            }
        }
        for (const auto& [section, name] : Entries(overlay)) {
            // The overlay changes words, not which language this is.
            if (SameName(section, kLanguageSection) || !known(section, name)) {
                census.overlayUnmatched.push_back(Where(section, name));
            }
        }
        return table;
    }

    const std::string* Table::PromptForm(std::string_view field) const
    {
        const auto it = promptForms_.find(Lowered(field));
        return it != promptForms_.end() ? &it->second : nullptr;
    }

    const TextTemplate::Message* Table::Find(const TextKeys::Key& key) const
    {
        const auto it = messages_.find(Id(key.section, key.name));
        return it != messages_.end() ? &it->second : nullptr;
    }

    const std::string* Table::Source(const TextKeys::Key& key) const
    {
        const auto it = sources_.find(Id(key.section, key.name));
        return it != sources_.end() ? &it->second : nullptr;
    }

    const std::string* Table::DataText(std::string_view section, std::string_view id) const
    {
        const auto it = data_.find(Id(section, id));
        return it != data_.end() ? &it->second : nullptr;
    }

    std::vector<std::string> Table::UnmatchedData(std::string_view section, std::span<const std::string> ids) const
    {
        std::unordered_set<std::string> loaded;
        for (const auto& id : ids) {
            loaded.insert(Id(section, id));
        }
        std::vector<std::string> out;
        for (const auto& [keySection, name] : dataKeys_) {
            if (SameName(keySection, section) && !loaded.contains(Id(keySection, name))) {
                out.push_back(Where(keySection, name));
            }
        }
        return out;
    }

    std::string Table::Render(const TextKeys::Key& key,
                              const TextTemplate::Arguments& arguments,
                              bool sentence,
                              std::function<std::string(std::string_view name)> missing) const
    {
        const auto* message = Find(key);
        if (message == nullptr) {
            return {};
        }
        TextTemplate::RenderOptions options;
        options.grammar = grammar_;
        options.missing = std::move(missing);
        options.sentence = sentence;
        return TextTemplate::Render(*message, arguments, options);
    }
} // namespace NarrativeEngine::LanguageTable
