#include <CharacterBios.h>

#include <algorithm>
#include <cctype>
#include <unordered_set>

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

        // How strongly do these two names denote the same person?
        //
        // 0 is no resemblance. Higher is a better match, so a caller
        // choosing between candidates can prefer the closest rather than
        // the first, which matters because more than one file can carry
        // the same suffix.
        //
        // WHY THIS IS LOOSE. A bio file is often not named for the
        // display name the game shows. Measured over the 35 real
        // suffix-only collisions from one session, the file said
        // "brandish" for Brand-Shei, "jarl_siddgeir" for Siddgeir,
        // "namira" for the Voice of Namira, "bujold_the_unworthy" for
        // Bujold the Intrepid, and "arniel_s_shade" for Arniel Gane.
        // All five are the same person under another name, and a strict
        // test threw all five away.
        //
        // WHY IT IS STILL SAFE. The same 35 cases contain 29 genuine
        // collisions -- Savos Aren against "elynea_mothren", Miraak
        // against "lemkil" -- and the separation is not close. Every
        // true pair shares a five-character prefix or contains the
        // other outright; every false pair shares at most ONE leading
        // character. There is nothing in the gap, so the thresholds
        // below are not tuned to a boundary, they sit in a chasm.
        enum class Affinity : std::uint8_t
        {
            None = 0,
            Prefix = 1,    // a five-character head in common
            Contained = 2, // one name is the whole of the other
            Exact = 3
        };

        [[nodiscard]] Affinity NameAffinity(std::string_view a, std::string_view b)
        {
            if (a.empty() || b.empty()) {
                return Affinity::None;
            }
            if (a == b) {
                return Affinity::Exact;
            }

            // A title or an epithet wrapped around the name: "Siddgeir"
            // inside "jarl_siddgeir", "namira" inside "voice_of_namira".
            // Length-guarded, because a three-letter name is a substring
            // of a great deal of ordinary text.
            constexpr std::size_t kMinShared = 5;
            const auto shortest = std::min(a.size(), b.size());
            if (shortest >= kMinShared
                && (a.find(b) != std::string_view::npos || b.find(a) != std::string_view::npos)) {
                return Affinity::Contained;
            }

            // A shared head: "brandish" against "brandshei", "bujold the
            // unworthy" against "bujold the intrepid". Same guard.
            std::size_t shared = 0;
            while (shared < shortest && a[shared] == b[shared]) {
                ++shared;
            }
            return shared >= kMinShared ? Affinity::Prefix : Affinity::None;
        }

        // What tells a relational clause from an ordinary one.
        //
        // CLOSED LISTS, and deliberately so. Every entry can only
        // introduce a second party; none of them can head a description
        // of what somebody does. That is the whole safety property --
        // "runs", "tends", "buys" and "works" are not here, so an
        // ordinary role query never reaches any of this.

        // The referent is an ENEMY, so neither they nor their kin can
        // be the answer.
        //
        // `disgruntled with` and `dislikes` were added from observed
        // output rather than invented: both turned up in role queries
        // the model wrote during a twenty-tick run, alongside markers
        // already listed here.
        constexpr const char* kAntagonismMarkers[]{"against",          "resents",
                                                   "resentful of",     "envies",
                                                   "envious of",       "disapproves of",
                                                   "opposes",          "opposed to",
                                                   "distrusts",        "hates",
                                                   "rival of",         "rivals",
                                                   "defies",           "betrayed by",
                                                   "wronged by",       "jealous of",
                                                   "hostile to",       "blames",
                                                   "informs on",       "spies on",
                                                   "undermines",       "conspires against",
                                                   "disgruntled with", "dislikes"};

        // The referent is a MASTER, so only they are excluded. Their
        // household and their family are exactly who the query wants.
        //
        // "serves" is in here despite heading a job as often as an
        // allegiance ("serves the Jarl", "serves drinks"), because
        // Referents demands a name belonging to one person before it
        // acts on any of these -- and "drinks at the Bannered Mare"
        // names nobody. Leaving it out cost a real target: a step
        // wanting "a Silver-Blood housecarl who serves Thongvor"
        // resolved to Thongvor himself.
        constexpr const char* kAllegianceMarkers[]{"serves",
                                                   "serving",
                                                   "works for",
                                                   "working for",
                                                   "loyal to",
                                                   "sworn to",
                                                   "beholden to",
                                                   "answers to",
                                                   "reports to",
                                                   "in debt to",
                                                   "in the service of",
                                                   "retainer of",
                                                   "apprenticed to"};

        // How much of a bullet's description counts as the role.
        //
        // "Attendant and apprentice whom Othreloth strictly mentors" puts
        // the job first and the biographer's opinion after it, which is
        // the usual shape. Six words keeps the first and mostly drops the
        // second; a whole description pulls in how the owner FEELS about
        // the person, which answers no role query.
        constexpr std::size_t kRoleLeadWords = 6;

        // The first `limit` words of a text, punctuation flattened to
        // spaces.
        [[nodiscard]] std::string LeadWords(std::string_view text, std::size_t limit)
        {
            std::string out;
            std::size_t words = 0;
            bool inWord = false;
            for (const char c : text) {
                const bool alpha = std::isalpha(static_cast<unsigned char>(c)) != 0;
                if (alpha && !inWord) {
                    if (++words > limit) {
                        break;
                    }
                    inWord = true;
                } else if (!alpha) {
                    inWord = false;
                }
                out.push_back(alpha ? c : ' ');
            }
            return out;
        }

        // Words that look like a proper noun and name a PEOPLE rather
        // than a person.
        //
        // A faction is a legitimate referent -- striking Thalmor for "a
        // spy against the Thalmor" is right, and they are a group anyone
        // can be counted out of. A RACE is not: everybody has one, so
        // "a Reachman prisoner with a grudge against Nords" struck three
        // arbitrary Nords off a query that was not about them. Observed
        // in a run, harmless there only because one candidate was
        // eligible at all.
        //
        // Stored as ProperWords leaves them -- normalised, and
        // de-pluralised where the plural is regular. The irregulars are
        // spelled out; the list is the common races of Tamriel and is
        // not meant to be exhaustive, because a race it misses costs one
        // spurious demotion rather than a wrong answer.
        const std::unordered_set<std::string> kRaceWords{"nord",     "breton",   "imperial", "redguard", "altmer",
                                                         "dunmer",   "bosmer",   "orsimer",  "khajiit",  "argonian",
                                                         "reachman", "reachmen", "falmer",   "dwemer",   "orcs",
                                                         "elves",    "nordic",   "mer",      "manmer",   "snowelve"};

        // The words of a phrase that name somebody: capitalised, four
        // letters or more, normalised and de-pluralised so they compare
        // against a display name. "the Gray-Manes" gives {"graymane"}.
        //
        // Capitalisation is the test because it is the one the prose
        // itself provides. Four letters because shorter ones collide
        // with ordinary words once the case is gone.
        [[nodiscard]] std::vector<std::string> ProperWords(std::string_view phrase)
        {
            std::vector<std::string> out;
            std::string word;
            for (std::size_t i = 0; i <= phrase.size(); ++i) {
                const char c = i < phrase.size() ? phrase[i] : ' ';
                const auto u = static_cast<unsigned char>(c);
                if (std::isalnum(u) != 0 || c == '-' || c == '\'') {
                    word.push_back(c);
                    continue;
                }
                if (!word.empty() && std::isupper(static_cast<unsigned char>(word.front())) != 0) {
                    auto normalised = Normalise(word);
                    // A plural names the family rather than the person:
                    // "the Gray-Manes" has to match "Gray-Mane". Guarded
                    // by length so a four-letter name ending in s keeps
                    // its last letter.
                    if (normalised.size() > 4 && normalised.back() == 's') {
                        normalised.pop_back();
                    }
                    if (normalised.size() >= 4) {
                        out.push_back(std::move(normalised));
                    }
                }
                word.clear();
            }
            return out;
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
            const auto affinity = NameAffinity(wanted, only.slug);
            return Lookup{
                affinity != Affinity::None ? Outcome::Confirmed : Outcome::BySuffixAlone, only.stem, only.path};
        }

        // More than one file shares the suffix, so the name decides --
        // and it picks the CLOSEST, not the first. With a graded test
        // several candidates can agree, and taking whichever the
        // directory happened to list first would settle by filename
        // order. If none agrees at all, refuse: one of these
        // biographies belongs to somebody else.
        const Entry* best = nullptr;
        auto bestAffinity = Affinity::None;
        for (const auto& candidate : candidates) {
            const auto affinity = NameAffinity(wanted, candidate.slug);
            if (affinity > bestAffinity) {
                best = &candidate;
                bestAffinity = affinity;
            }
        }
        if (best != nullptr) {
            return Lookup{Outcome::Confirmed, best->stem, best->path};
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

    std::array<const std::string*, 7> Bio::Blocks() const
    {
        return {&summary, &background, &personality, &aspirations, &relationships, &occupation, &skills};
    }

    Relation RelationOf(std::string_view query)
    {
        Relation relation;
        std::string lowered;
        lowered.reserve(query.size());
        for (const char c : query) {
            lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }

        // Earliest marker wins, whichever list it came from: a query
        // saying both things says the first one about its subject.
        std::size_t at = std::string::npos;
        std::size_t after = 0;
        auto kind = Relation::Kind::None;
        const auto scan = [&](const auto& markers, Relation::Kind which) {
            for (const auto* marker : markers) {
                const auto found = lowered.find(marker);
                if (found != std::string::npos && found < at) {
                    at = found;
                    after = found + std::char_traits<char>::length(marker);
                    kind = which;
                }
            }
        };
        scan(kAntagonismMarkers, Relation::Kind::Antagonism);
        scan(kAllegianceMarkers, Relation::Kind::Allegiance);
        if (at == std::string::npos) {
            return relation;
        }

        // Up to the next comma, where a second clause would begin.
        auto stop = query.find(',', after);
        if (stop == std::string_view::npos) {
            stop = query.size();
        }
        std::string phrase{query.substr(after, stop - after)};

        // A marker with no proper noun after it introduces nobody: "who
        // fears the cold" is not a query about a second party.
        relation.names = ProperWords(phrase);
        if (relation.names.empty()) {
            return relation;
        }
        // Nor does a marker followed only by a RACE. "against Nords"
        // names four million people and excludes none of them; a phrase
        // that also names somewhere or somebody is still a referent, so
        // this refuses only the ones that are nothing but race.
        if (std::all_of(relation.names.begin(), relation.names.end(), [](const std::string& word) {
                return kRaceWords.count(word) != 0;
            })) {
            relation.names.clear();
            return relation;
        }
        relation.referent = std::move(phrase);
        relation.kind = kind;
        return relation;
    }

    std::vector<Attribution> ParseRelationships(std::string_view block)
    {
        std::vector<Attribution> out;
        std::size_t at = 0;
        while (at < block.size()) {
            auto start = block.find("- ", at);
            if (start == std::string_view::npos) {
                break;
            }
            // A bullet BEGINS a line. Anywhere else, "- " is a dash in
            // the middle of a sentence.
            if (start != 0 && block[start - 1] != '\n') {
                at = start + 2;
                continue;
            }
            start += 2;
            auto stop = block.find('\n', start);
            if (stop == std::string_view::npos) {
                stop = block.size();
            }
            const auto line = block.substr(start, stop - start);
            at = stop + 1;

            // "Name (Role): description" or "Name: description". The
            // colon is what separates who the bullet is about from what
            // it says, so a line without one is not a bullet of this
            // shape and is left alone.
            const auto colon = line.find(':');
            if (colon == std::string_view::npos) {
                continue;
            }
            const auto open = line.find('(');
            const auto close = line.find(')');
            const bool parenthesised =
                open != std::string_view::npos && close != std::string_view::npos && close > open && open < colon;

            Attribution entry;
            entry.name = std::string{line.substr(0, parenthesised ? open : colon)};
            if (parenthesised) {
                entry.role = std::string{line.substr(open + 1, close - open - 1)};
                entry.role.push_back(' ');
            }
            entry.role += LeadWords(line.substr(colon + 1), kRoleLeadWords);
            if (entry.name.empty() || entry.role.find_first_not_of(' ') == std::string::npos) {
                continue;
            }
            out.push_back(std::move(entry));
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
