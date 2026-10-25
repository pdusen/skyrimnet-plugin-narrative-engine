#include <TextTemplate.h>

#include <TextUtf8.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <format>
#include <iterator>

namespace NarrativeEngine::TextTemplate
{
    namespace
    {
        constexpr std::array<std::string_view, 6> kPluralKeys = {"zero", "one", "two", "few", "many", "other"};

        [[nodiscard]] bool IsNameStart(char c) noexcept
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        }

        [[nodiscard]] bool IsNameChar(char c) noexcept
        {
            return IsNameStart(c) || (c >= '0' && c <= '9');
        }

        [[nodiscard]] bool IsAsciiSpace(char c) noexcept
        {
            return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
        }

        [[nodiscard]] bool IsSpace(char32_t c) noexcept
        {
            return (c < 0x80 && IsAsciiSpace(static_cast<char>(c))) || c == 0x00A0 || (c >= 0x2000 && c <= 0x200A)
                   || c == 0x202F || c == 0x205F || c == 0x3000;
        }

        [[nodiscard]] std::string_view CategoryKey(PluralCategory category) noexcept
        {
            return kPluralKeys[static_cast<std::size_t>(category)];
        }

        [[nodiscard]] std::string_view SexKey(Sex sex) noexcept
        {
            switch (sex) {
            case Sex::Male:
                return "male";
            case Sex::Female:
                return "female";
            case Sex::Other:
                break;
            }
            return "other";
        }

        [[nodiscard]] std::string_view FormatName(Placeholder::Format format) noexcept
        {
            switch (format) {
            case Placeholder::Format::Simple:
                return "simple";
            case Placeholder::Format::Select:
                return "select";
            case Placeholder::Format::Plural:
                return "plural";
            case Placeholder::Format::List:
                return "list";
            case Placeholder::Format::Phrase:
                return "phrase";
            }
            return "simple";
        }

        [[nodiscard]] std::string_view KindName(Kind kind) noexcept
        {
            switch (kind) {
            case Kind::Text:
                return "text";
            case Kind::Person:
                return "person";
            case Kind::Number:
                return "number";
            case Kind::List:
                return "list";
            }
            return "text";
        }

        // A recursive-descent reader over one template. Every problem is
        // recorded with its byte offset; reading stops at the first, since
        // whatever follows a malformed placeholder cannot be trusted.
        class Reader
        {
        public:
            explicit Reader(std::string_view text) : text_(text) {}

            Parsed Read()
            {
                Parsed out;
                out.message = ReadMessage(false, false);
                if (problem_.empty() && at_ < text_.size()) {
                    Fail(std::format("a '}}' that closes nothing at {}", at_));
                }
                if (!problem_.empty()) {
                    out.message.clear();
                    out.problems.push_back(problem_);
                }
                return out;
            }

        private:
            void Fail(std::string why)
            {
                if (problem_.empty()) {
                    problem_ = std::move(why);
                }
            }

            [[nodiscard]] bool Failed() const noexcept
            {
                return !problem_.empty();
            }

            void SkipSpace()
            {
                while (at_ < text_.size() && IsAsciiSpace(text_[at_])) {
                    ++at_;
                }
            }

            // Text, placeholders and (inside a plural) `#`, up to the end of
            // the template or, when `nested`, up to the '}' closing a branch,
            // which is left for the caller to consume.
            Message ReadMessage(bool nested, bool inPlural)
            {
                Message out;
                std::string text;
                const auto flush = [&] {
                    if (!text.empty()) {
                        out.push_back(Node{std::move(text)});
                        text.clear();
                    }
                };
                while (at_ < text_.size() && !Failed()) {
                    const char c = text_[at_];
                    if (c == '}') {
                        if (!nested) {
                            break;
                        }
                        flush();
                        return out;
                    }
                    if (c == '{') {
                        flush();
                        auto placeholder = ReadPlaceholder();
                        if (!Failed()) {
                            out.push_back(Node{std::move(placeholder)});
                        }
                        continue;
                    }
                    if (c == '#' && inPlural) {
                        flush();
                        out.push_back(Node{PluralNumber{}});
                        ++at_;
                        continue;
                    }
                    text.push_back(c);
                    ++at_;
                }
                if (nested && !Failed()) {
                    Fail("a branch that is never closed");
                }
                flush();
                return out;
            }

            std::string ReadName()
            {
                const auto start = at_;
                if (at_ < text_.size() && IsNameStart(text_[at_])) {
                    ++at_;
                    while (at_ < text_.size() && IsNameChar(text_[at_])) {
                        ++at_;
                    }
                }
                return std::string{text_.substr(start, at_ - start)};
            }

            // A branch key: letters, digits and underscores.
            std::string ReadKey()
            {
                const auto start = at_;
                while (at_ < text_.size() && IsNameChar(text_[at_])) {
                    ++at_;
                }
                return std::string{text_.substr(start, at_ - start)};
            }

            bool Expect(char c, std::string_view what)
            {
                SkipSpace();
                if (at_ < text_.size() && text_[at_] == c) {
                    ++at_;
                    return true;
                }
                Fail(std::format("expected {} at {}", what, at_));
                return false;
            }

            Placeholder ReadPlaceholder()
            {
                const auto open = at_;
                ++at_; // '{'
                Placeholder out;
                SkipSpace();
                out.name = ReadName();
                if (out.name.empty()) {
                    Fail(std::format("a '{{' at {} that opens no placeholder", open));
                    return out;
                }
                SkipSpace();
                if (at_ < text_.size() && text_[at_] == '}') {
                    ++at_;
                    return out;
                }
                if (!Expect(',', "',' or '}'")) {
                    return out;
                }
                SkipSpace();
                const auto format = ReadName();
                if (format == "list") {
                    out.format = Placeholder::Format::List;
                } else if (format == "phrase") {
                    out.format = Placeholder::Format::Phrase;
                } else if (format == "select") {
                    out.format = Placeholder::Format::Select;
                } else if (format == "plural") {
                    out.format = Placeholder::Format::Plural;
                } else {
                    Fail(std::format("'{}' is not a format, in the placeholder at {}", format, open));
                    return out;
                }
                if (out.format == Placeholder::Format::List || out.format == Placeholder::Format::Phrase) {
                    Expect('}', "'}'");
                    return out;
                }
                if (!Expect(',', "',' before the branches")) {
                    return out;
                }
                while (!Failed()) {
                    SkipSpace();
                    if (at_ < text_.size() && text_[at_] == '}') {
                        ++at_;
                        break;
                    }
                    Branch branch;
                    branch.key = ReadKey();
                    if (branch.key.empty()) {
                        Fail(std::format("expected a branch key or '}}' at {}", at_));
                        break;
                    }
                    if (!Expect('{', "'{' opening a branch")) {
                        break;
                    }
                    branch.message = ReadMessage(true, out.format == Placeholder::Format::Plural);
                    if (!Failed()) {
                        ++at_; // the branch's '}'
                        out.branches.push_back(std::move(branch));
                    }
                }
                if (!Failed() && out.branches.empty()) {
                    Fail(std::format("a {} with no branches, in the placeholder at {}", format, open));
                }
                return out;
            }

            std::string_view text_;
            std::size_t at_ = 0;
            std::string problem_;
        };

        [[nodiscard]] const Message* BranchFor(const Placeholder& placeholder, std::string_view key) noexcept
        {
            const Message* other = nullptr;
            for (const auto& branch : placeholder.branches) {
                if (branch.key == key) {
                    return &branch.message;
                }
                if (branch.key == "other") {
                    other = &branch.message;
                }
            }
            return other;
        }

        void ValidateInto(const Message& message, const Signature& signature, std::vector<std::string>& out)
        {
            for (const auto& node : message) {
                const auto* placeholder = std::get_if<Placeholder>(&node.value);
                if (placeholder == nullptr) {
                    continue;
                }
                const auto parameter = std::ranges::find(signature, placeholder->name, &Parameter::name);
                if (parameter == signature.end()) {
                    out.push_back(std::format("'{}' is not a placeholder this text takes", placeholder->name));
                } else {
                    bool fits = true;
                    switch (placeholder->format) {
                    case Placeholder::Format::Simple:
                        break;
                    case Placeholder::Format::Select:
                        fits = parameter->kind == Kind::Text || parameter->kind == Kind::Person;
                        break;
                    case Placeholder::Format::Plural:
                        fits = parameter->kind == Kind::Number;
                        break;
                    case Placeholder::Format::List:
                        fits = parameter->kind == Kind::List;
                        break;
                    case Placeholder::Format::Phrase:
                        fits = parameter->kind == Kind::Text;
                        break;
                    }
                    if (!fits) {
                        out.push_back(std::format("'{}' is a {}, and cannot take '{}'",
                                                  placeholder->name,
                                                  KindName(parameter->kind),
                                                  FormatName(placeholder->format)));
                    }
                }
                if (placeholder->format == Placeholder::Format::Select
                    || placeholder->format == Placeholder::Format::Plural) {
                    std::vector<std::string_view> seen;
                    for (const auto& branch : placeholder->branches) {
                        if (std::ranges::find(seen, branch.key) != seen.end()) {
                            out.push_back(std::format("'{}' has two '{}' branches", placeholder->name, branch.key));
                        }
                        seen.push_back(branch.key);
                        if (placeholder->format == Placeholder::Format::Plural
                            && std::ranges::find(kPluralKeys, branch.key) == kPluralKeys.end()) {
                            out.push_back(
                                std::format("'{}' is not a plural category, in '{}'", branch.key, placeholder->name));
                        }
                        ValidateInto(branch.message, signature, out);
                    }
                    if (std::ranges::find(seen, std::string_view{"other"}) == seen.end()) {
                        out.push_back(std::format("'{}' has no 'other' branch", placeholder->name));
                    }
                }
            }
        }

        void NamesInto(const Message& message, std::vector<std::string>& out)
        {
            for (const auto& node : message) {
                const auto* placeholder = std::get_if<Placeholder>(&node.value);
                if (placeholder == nullptr) {
                    continue;
                }
                if (std::ranges::find(out, placeholder->name) == out.end()) {
                    out.push_back(placeholder->name);
                }
                for (const auto& branch : placeholder->branches) {
                    NamesInto(branch.message, out);
                }
            }
        }

        void ImpliedInto(const Message& message, Signature& out)
        {
            for (const auto& node : message) {
                const auto* placeholder = std::get_if<Placeholder>(&node.value);
                if (placeholder == nullptr) {
                    continue;
                }
                auto parameter = std::ranges::find(out, placeholder->name, &Parameter::name);
                if (parameter == out.end()) {
                    out.push_back(Parameter{placeholder->name, Kind::Text});
                    parameter = std::prev(out.end());
                }
                if (placeholder->format == Placeholder::Format::Plural) {
                    parameter->kind = Kind::Number;
                } else if (placeholder->format == Placeholder::Format::List && parameter->kind != Kind::Number) {
                    parameter->kind = Kind::List;
                }
                for (const auto& branch : placeholder->branches) {
                    ImpliedInto(branch.message, out);
                }
            }
        }

        class Renderer
        {
        public:
            Renderer(const Arguments& arguments, const RenderOptions& options)
                : arguments_(arguments), options_(options)
            {}

            void RenderInto(const Message& message, std::string& out, const std::int64_t* number)
            {
                for (const auto& node : message) {
                    if (const auto* text = std::get_if<std::string>(&node.value)) {
                        out.append(*text);
                    } else if (std::holds_alternative<PluralNumber>(node.value)) {
                        if (number != nullptr) {
                            out.append(std::to_string(*number));
                        }
                    } else {
                        RenderPlaceholder(std::get<Placeholder>(node.value), out, number);
                    }
                }
            }

        private:
            std::string Missing(std::string_view name) const
            {
                return options_.missing ? options_.missing(name) : std::string{};
            }

            std::string AsText(const Value& value) const
            {
                if (const auto* text = std::get_if<std::string>(&value)) {
                    return *text;
                }
                if (const auto* person = std::get_if<Person>(&value)) {
                    return person->name;
                }
                if (const auto* number = std::get_if<std::int64_t>(&value)) {
                    return std::to_string(*number);
                }
                return JoinList(std::get<List>(value), options_.grammar);
            }

            void RenderPlaceholder(const Placeholder& placeholder, std::string& out, const std::int64_t* number)
            {
                const auto* value = arguments_.Find(placeholder.name);
                switch (placeholder.format) {
                case Placeholder::Format::Simple:
                case Placeholder::Format::List:
                    out.append(value != nullptr ? AsText(*value) : Missing(placeholder.name));
                    return;
                case Placeholder::Format::Phrase:
                    if (value == nullptr) {
                        out.append(Missing(placeholder.name));
                    } else if (const auto* text = std::get_if<std::string>(value)) {
                        out.append(Phrase(*text, options_.grammar));
                    } else {
                        out.append(AsText(*value));
                    }
                    return;
                case Placeholder::Format::Select: {
                    std::string key = "other";
                    if (value != nullptr) {
                        if (const auto* person = std::get_if<Person>(value)) {
                            key = SexKey(person->sex);
                        } else {
                            key = AsText(*value);
                        }
                    }
                    if (const auto* branch = BranchFor(placeholder, key)) {
                        RenderInto(*branch, out, number);
                    }
                    return;
                }
                case Placeholder::Format::Plural: {
                    const auto* n = value != nullptr ? std::get_if<std::int64_t>(value) : nullptr;
                    const auto key =
                        n != nullptr ? CategoryKey(Plural(options_.grammar.pluralRule, *n)) : std::string_view{"other"};
                    if (const auto* branch = BranchFor(placeholder, key)) {
                        RenderInto(*branch, out, n);
                    }
                    return;
                }
                }
            }

            const Arguments& arguments_;
            const RenderOptions& options_;
        };
    } // namespace

    PluralCategory Plural(PluralRule rule, std::int64_t n) noexcept
    {
        const auto abs = n < 0 ? -n : n;
        const auto mod10 = abs % 10;
        const auto mod100 = abs % 100;
        const bool million = abs != 0 && abs % 1'000'000 == 0;
        switch (rule) {
        case PluralRule::None:
            return PluralCategory::Other;
        case PluralRule::OneOther:
            return abs == 1 ? PluralCategory::One : PluralCategory::Other;
        case PluralRule::Spanish:
            return abs == 1 ? PluralCategory::One : million ? PluralCategory::Many : PluralCategory::Other;
        case PluralRule::French:
            return abs == 0 || abs == 1 ? PluralCategory::One : million ? PluralCategory::Many : PluralCategory::Other;
        case PluralRule::Russian:
            if (mod10 == 1 && mod100 != 11) {
                return PluralCategory::One;
            }
            if (mod10 >= 2 && mod10 <= 4 && (mod100 < 12 || mod100 > 14)) {
                return PluralCategory::Few;
            }
            return PluralCategory::Many;
        case PluralRule::Polish:
            if (abs == 1) {
                return PluralCategory::One;
            }
            if (mod10 >= 2 && mod10 <= 4 && (mod100 < 12 || mod100 > 14)) {
                return PluralCategory::Few;
            }
            return PluralCategory::Many;
        }
        return PluralCategory::Other;
    }

    Parsed Parse(std::string_view text)
    {
        return Reader{text}.Read();
    }

    std::vector<std::string> Validate(const Message& message, const Signature& signature)
    {
        std::vector<std::string> out;
        ValidateInto(message, signature, out);
        return out;
    }

    std::vector<std::string> PlaceholderNames(const Message& message)
    {
        std::vector<std::string> out;
        NamesInto(message, out);
        return out;
    }

    Signature ImpliedSignature(const Message& message)
    {
        Signature out;
        ImpliedInto(message, out);
        return out;
    }

    Arguments& Arguments::Set(std::string name, Value value)
    {
        for (auto& [existing, held] : values_) {
            if (existing == name) {
                held = std::move(value);
                return *this;
            }
        }
        values_.emplace_back(std::move(name), std::move(value));
        return *this;
    }

    const Value* Arguments::Find(std::string_view name) const noexcept
    {
        for (const auto& [existing, held] : values_) {
            if (existing == name) {
                return &held;
            }
        }
        return nullptr;
    }

    std::string Render(const Message& message, const Arguments& arguments, const RenderOptions& options)
    {
        std::string out;
        Renderer{arguments, options}.RenderInto(message, out, nullptr);
        if (options.sentence && options.grammar.capitalizeSentenceStart) {
            out = TextUtf8::UppercaseFirst(out);
        }
        return out;
    }

    std::string Phrase(std::string_view text, const Grammar& grammar)
    {
        const auto trims = [&](char32_t c) {
            if (IsSpace(c)) {
                return true;
            }
            const std::string_view set = grammar.phraseTrailingPunctuation;
            for (std::size_t at = 0; at < set.size();) {
                const auto decoded = TextUtf8::DecodeAt(set, at);
                if (decoded.codePoint == c) {
                    return true;
                }
                at += decoded.length;
            }
            return false;
        };

        // Trailing: walk forward, remembering where the last code point that
        // stays ends. Leading: whitespace only.
        std::size_t start = 0;
        while (start < text.size()) {
            const auto decoded = TextUtf8::DecodeAt(text, start);
            if (!IsSpace(decoded.codePoint)) {
                break;
            }
            start += decoded.length;
        }
        std::size_t end = start;
        for (std::size_t at = start; at < text.size();) {
            const auto decoded = TextUtf8::DecodeAt(text, at);
            at += decoded.length;
            if (!trims(decoded.codePoint)) {
                end = at;
            }
        }
        const auto trimmed = text.substr(start, end - start);
        return grammar.phraseLowercaseFirst ? TextUtf8::LowercaseFirst(trimmed) : std::string{trimmed};
    }

    std::string JoinList(const List& items, const Grammar& grammar)
    {
        std::string out;
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (i > 0) {
                if (items.size() == 2) {
                    out.append(grammar.listPairSeparator);
                } else if (i + 1 == items.size()) {
                    out.append(grammar.listFinalSeparator);
                } else {
                    out.append(grammar.listSeparator);
                }
            }
            out.append(items[i]);
        }
        return out;
    }
} // namespace NarrativeEngine::TextTemplate
