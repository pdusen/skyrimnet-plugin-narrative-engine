#pragma once

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
        // Exactly one file carried the suffix, and its slug does NOT
        // look like this NPC's name. Accepted, because a legitimately
        // differently-named file is a real case, but counted separately:
        // a coincidental collision would land here too.
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

        // Every block, concatenated. What the retrieval index is built
        // from, where the headings are meaningless and only the words
        // matter.
        [[nodiscard]] std::string Prose() const;
    };

    // Pull the blocks out of a bio file's text.
    [[nodiscard]] Bio ParseBio(std::string_view file);

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
    [[nodiscard]] const CharacterIndex::Index& Search();

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

} // namespace NarrativeEngine::CharacterBios
