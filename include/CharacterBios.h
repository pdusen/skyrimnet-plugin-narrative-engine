#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <CharacterIndex.h>

#include <RE/Skyrim.h>

// CharacterBios — finding an NPC's biography without asking SkyrimNet.
//
// ---------------------------------------------------------------------
// WHY NOT JUST ASK
//
// SkyrimNet exposes PublicGetBioTemplateName, which answers exactly this
// question, and it was the obvious way to do it. It is also the wrong
// one, because it only answers for actors SkyrimNet has ALREADY MET.
//
// The mapping lives in SkyrimNet's `uuid_mappings` table, which is
// created per save file and filled in as the player encounters people.
// A first run of this lookup over an 881-member population resolved 88
// of them: the table held 171 rows in total. Older, longer-played saves
// on the same machine held anywhere from 2 to 2087. The number says
// nothing about what data exists — only about where that playthrough
// has been. FormIDToUUID has the same gate, which is why plot casting
// was drawing 25 mastermind candidates to keep 5.
//
// The bios themselves have no such gate. They are files, they ship with
// the mod, and 842 of the 857 members of the plot population have one.
//
// ---------------------------------------------------------------------
// HOW THE FILENAME WORKS
//
// A bio is `prompts/characters/<slug>_<hex>.prompt`, where `<hex>` is
// the low 12 bits of the actor's REFERENCE form id — not the base NPC
// form. Checked against every `uuid_mappings` row across every save on
// the machine whose template names a real file: 642 matched, 0 differed.
//
// The base-form spelling is the near miss to watch for. Abelone's base
// is 08774F and her file is `abelone_750`; Aela's base is 01A696 and her
// file is `aela_the_huntress_697`. Both are base + 1, because the
// Creation Kit hands a new reference the next form id after the base it
// was made from — close enough to look right in a spot check and wrong
// for everyone the CK placed in a different order.
//
// GossipGraph carries a reference id for every participant (881 of 881
// on the run this was written against), so nothing here needs a running
// SkyrimNet or a save that has been anywhere.
//
// ---------------------------------------------------------------------
// RECOGNITION, NOT DERIVATION
//
// This does NOT construct a filename from a name and an id. That was
// tried: `slug(name)_<hex>` reproduces the real stem about 70% of the
// time and breaks on hyphens (`parvana_al-rihad`), on bracketed
// suffixes (`..._[riften_guard]_84F`), on zero-padding (`grelka_0B3`),
// and on people whose file is named for someone else entirely
// (Brand-Shei's is `brandish_DDC`).
//
// Instead it reads the directory and MATCHES. The suffix is a strong
// key on its own — 1,305 of 1,733 are unique to one file and no more
// than four files ever share one — so a name only has to break a tie
// among a handful, and the spelling of the slug stops mattering.
namespace NarrativeEngine::CharacterBios
{
    // What a lookup did, so a caller can log the difference between
    // "no file" and "would have had to guess".
    enum class Outcome : std::uint8_t
    {
        // Matched, and the name agreed with the slug.
        Confirmed,
        // Exactly one file carried the suffix and its slug bears no
        // resemblance to this NPC's name. Almost always a coincidence:
        // twelve bits of suffix across 2237 files collide readily, and
        // of 35 such matches in one session, 29 were a different
        // character outright -- the Arch-Mage of Winterhold carrying a
        // Tel Mithryn alchemist's life story.
        //
        // A file named for the same person under ANOTHER name does not
        // land here. That is what the affinity test is for, and the six
        // real cases it rescues are the reason it is loose: Brand-Shei's
        // file is "brandish", Siddgeir's is "jarl_siddgeir", the Voice
        // of Namira's is "namira".
        BySuffixAlone,
        // No file carries that suffix.
        NoFile,
        // Several files carry it and none matched the name. Refused
        // rather than guessed -- attaching the wrong biography to the
        // wrong person is invisible until it is embarrassing.
        Ambiguous
    };

    struct Lookup
    {
        Outcome outcome = Outcome::NoFile;
        std::string stem;
        // The file to actually read. Carried rather than rebuilt from
        // the stem, because the same stem can live in two directories
        // and only the catalogue knows which one won.
        std::filesystem::path path;
    };

    class Catalog
    {
    public:
        // Read every `*.prompt` in `directory` and index it. Returns how
        // many carried a usable `<slug>_<hex>` name; `_generic` files
        // and anything else are counted but not indexed, because they
        // belong to no specific actor.
        //
        // FIRST SCAN WINS. A stem already in the catalogue is skipped,
        // so scanning the save-specific directory before the shared one
        // gives the save's own copy priority -- which is the whole point
        // of there being two. Scanning them the other way round would
        // silently serve the default to every save that has edited a
        // profile.
        std::size_t Scan(const std::filesystem::path& directory);

        // Index one already-known stem, with the file it came from.
        // Pure, and the seam a probe uses to build a catalog without a
        // filesystem.
        void Add(std::string_view stem, const std::filesystem::path& path = {});

        // `actorRef` is a REFERENCE form id -- GossipGraph::ActorRefFor,
        // never the base NPC form. `name` is the NPC's display name and
        // is used only to break ties.
        [[nodiscard]] Lookup Find(RE::FormID actorRef, std::string_view name) const;

        [[nodiscard]] std::size_t Size() const noexcept
        {
            return _count;
        }
        [[nodiscard]] std::size_t GenericCount() const noexcept
        {
            return _generic;
        }

    private:
        struct Entry
        {
            std::string stem;
            std::string slug; // normalised, for comparison only
            std::filesystem::path path;
        };

        std::unordered_map<std::uint16_t, std::vector<Entry>> _bySuffix;
        // Every stem taken, so a later Scan cannot shadow an earlier one.
        std::unordered_set<std::string> _seen;
        std::size_t _count = 0;
        std::size_t _generic = 0;
    };

    // Lowercase, strip everything that is not a letter or digit. Applied
    // to both sides of a tie-break so that "Brand-Shei", "brand_shei"
    // and "BrandShei" all compare equal.
    [[nodiscard]] std::string Normalise(std::string_view text);

    // One biography, block by block.
    //
    // Kept as separate fields rather than one string because the prompts
    // render them under their own headings -- a scheme prompt wants the
    // occupation set apart from the personality, not both glommed
    // together into a wall of text. The retrieval index wants the
    // opposite, which is what Prose() is for.
    //
    // `appearance` and `speech_style` are deliberately absent. They say
    // how somebody looks and sounds, which answers neither "what would
    // this person scheme for" nor "who should carry this out".
    struct Bio
    {
        std::string summary;
        std::string background;
        std::string personality;
        std::string aspirations;
        std::string relationships;
        std::string occupation;
        std::string skills;

        [[nodiscard]] bool Empty() const noexcept;

        // Every block, concatenated. What the LEXICAL index is built
        // from, where the headings are meaningless and only the words
        // matter.
        [[nodiscard]] std::string Prose() const;

        // The blocks individually, in a fixed order, skipping none.
        //
        // What the SEMANTIC index is built from, one vector each. The
        // embedding model truncates at about ninety words, so a
        // concatenated bio would embed as its summary and nothing else;
        // separately, each block fits and each is a different thing to
        // match against.
        [[nodiscard]] std::array<const std::string*, 7> Blocks() const;
    };

    // Pull the blocks out of a bio file's text.
    [[nodiscard]] Bio ParseBio(std::string_view file);

    // --- What other people say your job is --------------------------------
    //
    // A relationships block is a bulleted table, one bullet per person,
    // and everything after the name at the head of a bullet describes
    // THAT person. So this line in Laila Law-Giver's biography
    //
    //     - Anuriel (Steward): Trusts completely, unaware Anuriel...
    //
    // is Laila's document asserting a fact about ANURIEL: she is the
    // steward. Collected across the corpus, those assertions answer a
    // question no single biography does well -- see kAttributionWeight
    // for the failure this exists to fix and the numbers behind it.
    //
    // Two forms, because the corpus writes both. The role is
    // parenthesised in about a third of files and otherwise leads the
    // description ("- Galdrus Hlervu: Attendant and apprentice whom
    // Othreloth strictly mentors"), so both are read.

    // One bullet: who it is about, and what it says their role is.
    struct Attribution
    {
        // As written, for the caller to resolve -- "Anuriel", "Jarl
        // Laila Law-Giver". Pure code cannot know who that is.
        std::string name;
        // The parenthetical, if there was one, plus the head of the
        // description. Only the HEAD: past a few words a bullet stops
        // describing the person and starts describing how the biography's
        // owner feels about them.
        std::string role;
    };

    // Pure. Reads a relationships block into its bullets, skipping any
    // line that is not one.
    [[nodiscard]] std::vector<Attribution> ParseRelationships(std::string_view block);

    // --- The session catalogue --------------------------------------------
    //
    // One scan, one set of reads, held for the session. Roughly 2 MB for
    // a full population, which buys a lock-free read from the plot
    // worker afterwards -- PlotPopulation's arrangement, for the same
    // reason: a lazily-filled cache would need a mutex on every access
    // to answer a question whose answer never changes.

    // Main thread, at kNewGame / kPostLoadGame.
    //
    // PER SAVE, NOT PER SESSION. SkyrimNet lets a save carry its own
    // edited copy of a character profile under
    // `prompts/_saves/<save id>/characters/`, so two saves of the same
    // playthrough can legitimately disagree about who somebody is.
    //
    // Not at kDataLoaded, where this used to run: there is no save yet,
    // so there is no save id to scan for.
    void OnSessionStart();

    // Main thread, from the serialization revert callback.
    //
    // Drops everything the outgoing save loaded. Holding it would leak
    // one character's version of the world into another's, and silently
    // -- a stale biography still reads as a biography, so nothing
    // downstream would ever report a problem.
    //
    // Revert rather than the load path, because revert also fires on
    // QUIT TO MAIN MENU, where nothing loads afterwards. Purging only on
    // load would leave the previous character's profiles resident until
    // the player happened to start another game.
    void OnRevert();

    // Does this NPC have a biography we can actually read? The
    // replacement for "does SkyrimNet happen to know them".
    [[nodiscard]] bool Has(RE::FormID npc);

    // Keyed on the BASE NPC form, the way the rest of the plugin refers
    // to people; the reference-id crossing happens inside. Returns an
    // empty Bio for anyone without one, so a caller may render it
    // unconditionally and get empty sections rather than a crash.
    [[nodiscard]] const Bio& For(RE::FormID npc);

    // Every loaded biography, indexed for retrieval by description.
    //
    // Same lifecycle as the bios themselves -- built in OnSessionStart,
    // dropped in OnRevert -- because it is derived from them and a
    // stale index would answer with the previous save's people.
    //
    // Empty until OnSessionStart runs, and querying an unbuilt index
    // returns nothing rather than asserting.
    //
    // The LEXICAL half only. Prefer Rank and Scorer below, which fuse
    // it with the semantic half; this is exposed for the cases that
    // genuinely want words rather than meaning.
    [[nodiscard]] const CharacterIndex::Index& Search();

    // One candidate, fused.
    struct Ranked
    {
        RE::FormID character = 0;

        // What the ranking is ordered by, 0..1. Not comparable between
        // queries -- both halves are normalised against the best in
        // THIS result set, which is the only way an unbounded BM25
        // score and a bounded cosine can be added at all.
        float fused = 0.0f;

        // The two halves that produced it, kept so a log can say WHICH
        // one found somebody. They disagree often, and when a match
        // looks wrong the first question is always whether the words or
        // the meaning put it there.
        float lexical = 0.0f;
        float semantic = 0.0f;

        // The third half, so to speak: how well what OTHER biographies
        // say this person's role is answers the query. See
        // kAttributionWeight.
        float attributed = 0.0f;

        // Carried through from the lexical half, because a ratio is the
        // wrong test on a short query and callers still need the counts.
        std::uint32_t matched = 0;
        std::uint32_t asked = 0;
        float coverage = 0.0f;
    };

    // A 64-bit digest of every biography that went into the vectors.
    //
    // Exposed for the cache: the vectors on disk are only valid for the
    // prose they were made from, and a save-specific profile edited
    // between sessions has to invalidate them. Comparing the digest is
    // how that is noticed without re-embedding to find out.
    [[nodiscard]] std::uint64_t CorpusDigest();

    // How much of the fused score is lexical. The rest is semantic.
    //
    // MEASURED, not chosen. Twenty queries with one known-correct answer
    // each, swept from pure cosine to pure BM25:
    //
    //   w=0.0  top-1  9   MRR 0.619     (cosine alone)
    //   w=0.2  top-1 14   MRR 0.764
    //   w=0.5  top-1 14   MRR 0.783     <-- here
    //   w=0.8  top-1 12   MRR 0.707
    //   w=1.0  top-1 11   MRR 0.658     (BM25 alone)
    //
    // Fusion beats both parents, which was the thing that had to be true
    // and was not guaranteed. 0.5 is taken over the equally-scoring 0.2
    // because it wins on top-3, top-10 and MRR, and because it sits in
    // the middle of a plateau running from 0.2 to 0.6 rather than on its
    // edge -- twenty queries is a small enough sample that a single-point
    // optimum would be fitted rather than found.
    //
    // Reciprocal rank fusion was tried too and lost (best k gave MRR
    // 0.715). RRF discards score MAGNITUDE and keeps only position, and
    // magnitude is real information here: a bio that matches every word
    // of a query is more than "first".
    inline constexpr float kLexicalWeight = 0.5f;

    // How much of what OTHER biographies say about somebody is added to
    // the score their own biography earned.
    //
    // ---------------------------------------------------------------------
    // THE FAILURE THIS FIXES
    //
    // A query for a subordinate retrieves the person who runs the place,
    // because the boss's biography is the best single document about
    // everyone who works for them. Laila Law-Giver's lists her whole
    // court, so it carries "steward" AND is saturated in Riften;
    // Anuriel's own biography says she is the steward but is one
    // person's life story. "A steward in Riften" duly returned the JARL,
    // and "a temple attendant" returned the HEAD PRIEST.
    //
    // ---------------------------------------------------------------------
    // TWO THINGS THAT DID NOT WORK, because the mistake they share is
    // worth not repeating
    //
    // PROMINENCE -- push down whoever the corpus mentions most. Measured
    // over seven weights: flipped 0 of 2 every time while the controls
    // fell from 6/8 to 1/8. There is no gap to exploit; a head priest
    // appears in three biographies and his own attendant in two.
    //
    // PROXIMITY -- take the name standing next to the role word in the
    // winning document. The sentence is "Unmid Snow-Shod as housecarl,
    // Anuriel as steward, and Wylandriah as court wizard", so all three
    // promote equally and the tie broke to the housecarl. Added instead
    // of inherited, it lifted Laila exactly as much as Anuriel, because
    // Anuriel's biography says "steward ... serving Jarl Laila
    // Law-Giver" too.
    //
    // Both read an UNDIRECTED signal for a DIRECTED fact. A bullet is
    // directed: the role in Anuriel's bullet is Anuriel's, and the
    // reciprocal bullet in her biography calls Laila "Employer", never
    // "steward". So a steward query lifts Anuriel and leaves Laila where
    // she was, while a Jarl query still finds Laila.
    //
    // ---------------------------------------------------------------------
    // 0.45 IS MEASURED. Two seniority failures with answers checkable
    // from the corpus itself -- Anuriel is "the Bosmer steward of
    // Riften", Galdrus Hlervu is "attendant to Elder Othreloth" -- and
    // eight ordinary queries, three of which ask those same
    // organisations for their HEADS and so would break first:
    //
    //   w=0      flipped 0/2   held 6/8   (before any of this)
    //   w=0.20   flipped 1/2   held 8/8
    //   w=0.30   flipped 2/2   held 7/8
    //   w=0.45   flipped 2/2   held 7/8   <-- here
    //   w=0.60   flipped 2/2   held 7/8
    //   w=0.80   flipped 2/2   held 5/8
    //
    // Case by case it is strictly better than 0: three answers improve,
    // seven are unchanged, none get worse. The one query still not
    // landing at rank 1 sits at rank 2, which is where it sat before --
    // only the identity of the wrong top result changed.
    //
    // 0.45 is the middle of the 0.30-0.60 plateau rather than an edge of
    // it, for the same reason kLexicalWeight is: ten cases is a small
    // enough sample that a single-point optimum would be fitted rather
    // than found. Past 0.80 what people say about somebody starts
    // outvoting their own biography.
    //
    // Applied in Rank and NOT in Scorer, which weights its halves raw --
    // a weight measured against normalised scores would mean something
    // else there. The agent path keeps its old behaviour until that is
    // measured on its own terms.
    inline constexpr float kAttributionWeight = 0.45f;

    // Rank the population against a description, best first.
    //
    // At most `limit` results. Falls back to the lexical half alone when
    // the embedding model is unavailable, which is a real degradation
    // rather than an error -- the model is downloaded at runtime and a
    // fresh install may not have it.
    [[nodiscard]] std::vector<Ranked> Rank(std::string_view query, std::size_t limit);

    // Scores many candidates against ONE description, fused the same
    // way. For a caller that has already decided who is eligible and
    // only needs them ordered -- the casting ladder's shape.
    //
    // An OBJECT rather than a function because the query is embedded
    // once, in the constructor. A forward pass is about a millisecond,
    // and the ladder asks about every eligible candidate in turn, so a
    // per-call Embed would re-encode identical text a dozen times for
    // one step and put that on the plot worker's critical path.
    //
    // Scores are NOT comparable to Rank()'s: with candidates arriving
    // one at a time there is no result set to normalise against, so both
    // halves are weighted raw. Comparable BETWEEN candidates of one
    // query, which is all an ordering needs.
    class Scorer
    {
    public:
        explicit Scorer(std::string_view query);

        [[nodiscard]] float operator()(RE::FormID npc) const;

        // Whether the semantic half is actually contributing. False when
        // the model is absent or the query embedded to nothing, in which
        // case this is pure BM25 -- worth logging, because it is the
        // difference between two quite different rankings.
        [[nodiscard]] bool Semantic() const noexcept
        {
            return !_embedded.empty();
        }

    private:
        std::string _query;
        std::vector<float> _embedded;
    };

    // Does `normalisedText` name the person whose normalised display
    // name is `normalisedName`? Returns the length of the match, or 0.
    //
    // Length rather than a bool so a caller comparing several names can
    // keep the LONGEST, which is how a string naming somebody's steward
    // resolves to the steward rather than to their employer.
    //
    // An exact match always counts. A name found INSIDE a longer string
    // must be at least five characters, because a short one is far more
    // likely to be a coincidence of letters than a reference to that
    // person -- normalisation strips spaces, so a three-letter name is
    // a substring of a great deal of ordinary prose -- and it must
    // appear at the FRONT, because a string that names somebody leads
    // with them while a string that merely mentions them does not.
    [[nodiscard]] std::size_t NameMatchLength(std::string_view normalisedName, std::string_view normalisedText);

    // Which population member does this text NAME, if any?
    //
    // Answers "who is this about" rather than "whose biography mentions
    // this", which is the question BM25 cannot be made to answer. A name
    // occurs in the biography of everyone who knows that person, usually
    // more densely than in their own -- a servant mentions their master
    // constantly, their master mentions himself once -- and length
    // normalisation then favours the shorter document. Measured on the
    // real corpus, searching for "Erikur" returned his housekeeper
    // first and him third; "Falk Firebeard" put him fifth behind four
    // people who merely know him.
    //
    // So a name is matched against WHO A BIOGRAPHY IS FOR, by display
    // name, which is exact and cannot be outvoted.
    //
    // `text` is the whole string the step asked for, which the prompt
    // requires to carry an occupation alongside any name -- so this
    // looks for a known name INSIDE it rather than comparing wholesale.
    // The longest match wins, so a string naming somebody's steward
    // resolves to the steward rather than to their employer.
    //
    // A WORD of a name counts too, when it belongs to exactly one
    // person and the text capitalises it. The whole display name is
    // often not what a prompt writes: "Arch-Mage Aren, the leader of
    // the College" names Savos Aren and contains neither "Savos" nor
    // "Savos Aren", so the whole-name pass missed it and the prose
    // search answered with Ancano -- the Thalmor agent who spends his
    // biography talking about the man, and who covers every word of
    // that query.
    //
    // Two guards make this safe. The word must be UNIQUE to one member,
    // which is what stops a family name resolving: 94% of name words
    // are unique, and `briar`, `gray`, `mane` and `born` are among the
    // 6% that are not, so the Black-Briars and the Gray-Manes correctly
    // resolve to nobody. And it must be CAPITALISED where it appears,
    // which is what separates a person called Hunter from a step asking
    // for a hunter.
    //
    // Returns 0 when the text names nobody, which is the normal case:
    // most targets are descriptions, and those belong to the search.
    [[nodiscard]] RE::FormID FindByName(std::string_view text);

    // --- Queries defined against somebody ---------------------------------
    //
    // A role query that describes its subject by who they are AGAINST
    // retrieves the person they are against, because that person's
    // biography is where those words live. Measured on the real corpus,
    // the referent was the top result for three of five such queries and
    // in the top six for the other two:
    //
    //   "a student at the College who has spoken against the Arch-Mage"
    //        -> Savos Aren, who IS the Arch-Mage
    //   "a Whiterun city guard who resents the Gray-Manes"
    //        -> Olfina Gray-Mane, who IS a Gray-Mane
    //
    // The obvious fix -- split the query at "who" and keep the front
    // half -- does not work, because it cannot tell a relational clause
    // from an ordinary one. "who runs a potion shop in Whiterun" is
    // capitalised and clause-shaped too, and splitting on it dropped
    // Arcadia from rank 1 to rank 58 and Hulda from 2 to 541.
    //
    // What separates them is the RELATION, so everything here is gated
    // on a marker: a closed list of verbs and phrases that can only
    // introduce a second party. Over six relational queries and nine
    // controls, including three that name a family and still want a
    // member of it, the gate fired on 6 of 6 and 0 of 9.
    //
    // The markers do NOT all mean the same thing, which a first pass
    // treated them as doing and an in-game run disproved from both
    // sides at once:
    //
    //   "a Silver-Blood housecarl who serves Thongvor" resolved to
    //   Thongvor Silver-Blood, in a plot aimed at him -- because
    //   `serves` had been left out of the list as too ambiguous.
    //
    //   "a Nord of Whiterun fiercely loyal to the Gray-Mane family"
    //   would have had every Gray-Mane struck from it, leaving the
    //   Battle-Borns -- the family they feud with -- as the top three.
    //
    // So the list is in two halves. See Kind.

    // What a query says about a second party.
    struct Relation
    {
        enum class Kind : std::uint8_t
        {
            // No marker, or a marker introducing nobody.
            None,
            // "resents the Gray-Manes". The referent AND their kin are
            // out: nobody who resents a family belongs to it.
            Antagonism,
            // "serves Thongvor". The referent alone is out -- nobody
            // serves themselves -- but their kin are fair game, and
            // striking them is how a loyalty query loses its answer.
            Allegiance
        };

        Kind kind = Kind::None;

        // The phrase naming them, taken from just after the marker.
        // Empty when the query is not relational at all, which is the
        // normal case.
        std::string referent;

        // Its capitalised words, normalised and de-pluralised, so they
        // compare against display names: "the Gray-Manes" gives
        // {"graymane"}.
        std::vector<std::string> names;

        [[nodiscard]] bool Any() const noexcept
        {
            return kind != Kind::None;
        }
    };

    // Pure. Reads a query and reports the second party it defines its
    // subject against, if there is one.
    //
    // A marker alone is not enough -- the phrase after it must contain a
    // proper noun. "who fears the cold" has a marker and names nobody,
    // so it is not relational and nothing happens to it.
    [[nodiscard]] Relation RelationOf(std::string_view query);

    // Who a relational query must NOT answer with.
    //
    // ANTAGONISM is answered two ways at once, because each misses what
    // the other catches:
    //
    //   BY RANK  whoever the referent phrase alone retrieves is who it
    //            is about. This is what catches a TITLE -- "the
    //            Arch-Mage" is nobody's name, so no name rule can find
    //            Savos Aren, but a search for it returns him first.
    //   BY NAME  everyone whose display name carries a word of the
    //            referent. This is what catches a FAMILY -- there are
    //            five Gray-Manes and a rank cutoff of three reaches
    //            three of them.
    //
    // ALLEGIANCE is answered by FindByName and nothing else, so it
    // strikes ONE person or nobody at all. Both halves above are wrong
    // here. The kin sweep would take the family a loyalty query is
    // asking about, and the rank sweep would take their household: a
    // servant's biography names their master constantly and the master's
    // names himself once, so ranking "Thongvor" returns his housecarls
    // alongside him -- exactly the people the step wanted. Requiring a
    // name that belongs to ONE person is also what keeps `serves` off
    // "serves drinks at the Bannered Mare", which is why that marker can
    // be in the list at all.
    //
    // Measured over six relational queries and nine controls: by rank
    // alone beat the referent in 4 of 6, by name alone in 4 of 6, and
    // the split above in 6 of 6 while leaving every control's ranking
    // EXACTLY as it was without any of this.
    //
    // Empty for a query that is not relational, so a caller may apply it
    // unconditionally.
    [[nodiscard]] std::unordered_set<RE::FormID> Referents(std::string_view query);

} // namespace NarrativeEngine::CharacterBios
