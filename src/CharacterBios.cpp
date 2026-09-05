#include <CharacterBios.h>

#include <algorithm>

namespace NarrativeEngine::CharacterBios
{
    namespace
    {
        constexpr std::string_view kGenericSuffix = "_generic";

        // Pull one inja block's text out of a bio file, trimmed.
        //
        // A scanner rather than a template parser: these files are
        // generated to a fixed shape, the blocks do not nest, and the
        // text is either headed in a prompt or reduced to a bag of
        // words. Neither needs the template semantics.
        [[nodiscard]] std::string Block(std::string_view file, std::string_view name)
        {
            const std::string opener{"{% block " + std::string{name} + " %}"};
            const auto start = file.find(opener);
            if (start == std::string_view::npos) {
                return {};
            }
            const auto from = start + opener.size();
            const auto end = file.find("{% endblock %}", from);
            if (end == std::string_view::npos) {
                return {};
            }
            auto text = file.substr(from, end - from);
            constexpr std::string_view kSpace{" \t\r\n"};
            const auto first = text.find_first_not_of(kSpace);
            if (first == std::string_view::npos) {
                return {};
            }
            text.remove_prefix(first);
            text.remove_suffix(text.size() - text.find_last_not_of(kSpace) - 1);
            return std::string{text};
        }

        [[nodiscard]] bool IsHexDigit(char c) noexcept
        {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        }

        // Split "aela_the_huntress_697" into "aela_the_huntress" and
        // 0x697. Fails on a stem whose last underscore-separated piece
        // is not 1..4 hex digits, which is what excludes `_generic` and
        // anything hand-added to the folder.
        [[nodiscard]] bool SplitStem(std::string_view stem, std::string_view& slug, std::uint16_t& suffix)
        {
            const auto cut = stem.rfind('_');
            if (cut == std::string_view::npos || cut + 1 >= stem.size()) {
                return false;
            }
            const auto tail = stem.substr(cut + 1);
            if (tail.size() > 4 || !std::all_of(tail.begin(), tail.end(), IsHexDigit)) {
                return false;
            }
            std::uint32_t value = 0;
            for (const char c : tail) {
                const auto digit = (c >= '0' && c <= '9')   ? c - '0'
                                   : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                                                            : c - 'A' + 10;
                value = value * 16 + static_cast<std::uint32_t>(digit);
            }
            slug = stem.substr(0, cut);
            // The low 12 bits are what the filename carries. A four-digit
            // suffix is masked to the same width rather than rejected.
            suffix = static_cast<std::uint16_t>(value & 0xFFF);
            return true;
        }

        // Do these two names plausibly denote the same person?
        //
        // Deliberately loose in one direction and strict in the other.
        // A prefix match catches "Aela" against "aela_the_huntress" and
        // "Captain Veleth" against "veleth", which are the common
        // shapes; it will not match two unrelated names, which is the
        // failure that matters.
        [[nodiscard]] bool NamesAgree(std::string_view a, std::string_view b)
        {
            if (a.empty() || b.empty()) {
                return false;
            }
            if (a == b) {
                return true;
            }
            constexpr std::size_t kMinPrefix = 6;
            const auto shortest = std::min(a.size(), b.size());
            if (shortest < kMinPrefix) {
                return false;
            }
            return a.compare(0, shortest, b, 0, shortest) == 0;
        }
    } // namespace

    std::string Normalise(std::string_view text)
    {
        std::string out;
        out.reserve(text.size());
        for (const char raw : text) {
            const auto c = static_cast<unsigned char>(raw);
            if (c >= 'a' && c <= 'z') {
                out.push_back(static_cast<char>(c));
            } else if (c >= 'A' && c <= 'Z') {
                out.push_back(static_cast<char>(c - 'A' + 'a'));
            } else if (c >= '0' && c <= '9') {
                out.push_back(static_cast<char>(c));
            }
        }
        return out;
    }

    std::size_t NameMatchLength(std::string_view normalisedName, std::string_view normalisedText)
    {
        if (normalisedName.empty() || normalisedText.empty()) {
            return 0;
        }
        if (normalisedName == normalisedText) {
            return normalisedName.size();
        }
        constexpr std::size_t kMinEmbedded = 5;
        if (normalisedName.size() < kMinEmbedded) {
            return 0;
        }

        // And it has to be at the FRONT. A target that names somebody
        // leads with them -- the prompt asks for the name and then their
        // occupation -- while a target that merely mentions somebody
        // buries them mid-sentence: "a courier who delivers General
        // Tullius's dispatches" wants a courier, not the general, and
        // without this it resolved to Tullius.
        //
        // Eight characters of slack rather than none, because a name is
        // often introduced by a title and normalisation has already
        // closed up the space after it. That covers the short ones a
        // schemer would actually use; a longer title simply falls
        // through to the description search, which is the safe
        // direction to fail in.
        //
        // Tuned over the real population: all 826 display names still
        // resolve to themselves, every name-plus-occupation form
        // resolves correctly, and every description that mentions
        // somebody in passing correctly resolves to nobody. At sixteen
        // the passing mentions start being taken as targets.
        constexpr std::size_t kMaxNameStart = 8;
        const auto at = normalisedText.find(normalisedName);
        return (at != std::string_view::npos && at <= kMaxNameStart) ? normalisedName.size() : 0;
    }

    void Catalog::Add(std::string_view stem, const std::filesystem::path& path)
    {
        if (stem.ends_with(kGenericSuffix)) {
            ++_generic;
            return;
        }
        std::string_view slug;
        std::uint16_t suffix = 0;
        if (!SplitStem(stem, slug, suffix)) {
            return;
        }
        // Already claimed by an earlier Scan, which outranks this one.
        if (!_seen.emplace(stem).second) {
            return;
        }
        _bySuffix[suffix].push_back(Entry{std::string{stem}, Normalise(slug), path});
        ++_count;
    }

    std::size_t Catalog::Scan(const std::filesystem::path& directory)
    {
        std::error_code ec;
        if (!std::filesystem::is_directory(directory, ec)) {
            return 0;
        }
        const auto before = _count;
        for (const auto& entry : std::filesystem::directory_iterator{directory, ec}) {
            if (ec) {
                break;
            }
            if (!entry.is_regular_file(ec) || entry.path().extension() != ".prompt") {
                continue;
            }
            Add(entry.path().stem().string(), entry.path());
        }
        return _count - before;
    }

    Lookup Catalog::Find(RE::FormID actorRef, std::string_view name) const
    {
        const auto found = _bySuffix.find(static_cast<std::uint16_t>(actorRef & 0xFFF));
        if (found == _bySuffix.end() || found->second.empty()) {
            return Lookup{Outcome::NoFile, {}, {}};
        }
        const auto& candidates = found->second;
        const auto wanted = Normalise(name);

        if (candidates.size() == 1) {
            const auto& only = candidates.front();
            return Lookup{
                NamesAgree(wanted, only.slug) ? Outcome::Confirmed : Outcome::BySuffixAlone, only.stem, only.path};
        }

        // More than one file shares the suffix, so the name has to
        // decide. If it cannot, refuse: one of these biographies belongs
        // to somebody else, and picking the first would be picking at
        // random.
        for (const auto& candidate : candidates) {
            if (NamesAgree(wanted, candidate.slug)) {
                return Lookup{Outcome::Confirmed, candidate.stem, candidate.path};
            }
        }
        return Lookup{Outcome::Ambiguous, {}, {}};
    }

    bool Bio::Empty() const noexcept
    {
        return summary.empty() && background.empty() && personality.empty() && aspirations.empty()
               && relationships.empty() && occupation.empty() && skills.empty();
    }

    std::string Bio::Prose() const
    {
        std::string out;
        out.reserve(summary.size() + background.size() + personality.size() + aspirations.size() + relationships.size()
                    + occupation.size() + skills.size() + 8);
        for (const auto* block :
             {&summary, &background, &personality, &aspirations, &relationships, &occupation, &skills}) {
            if (!block->empty()) {
                out.append(*block);
                out.push_back('\n');
            }
        }
        return out;
    }

    Bio ParseBio(std::string_view file)
    {
        Bio bio;
        bio.summary = Block(file, "summary");
        bio.background = Block(file, "background");
        bio.personality = Block(file, "personality");
        bio.aspirations = Block(file, "aspirations");
        bio.relationships = Block(file, "relationships");
        bio.occupation = Block(file, "occupation");
        bio.skills = Block(file, "skills");
        return bio;
    }
} // namespace NarrativeEngine::CharacterBios
