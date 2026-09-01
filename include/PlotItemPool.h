#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <RE/Skyrim.h>

// PlotItemPool — the objects an `Acquire` step is allowed to be about.
//
// The model does not get to invent the noun. Left to itself it will name
// "the Jarl's signet ring", which is a perfectly good story beat and not
// a record that exists; a step built on one can never be completed,
// handed to the player, or written into a memory that says anything
// true. So it is handed a MENU of resolved forms and picks an index off
// it, and everything on that menu was looked up at load.
//
// Curated rather than generated for a second reason: Skyrim has
// thousands of takeable objects and almost none of them are worth
// scheming over. A scheme about acquiring a wooden bowl is not a scheme.
// What belongs in the pool is anything whose loss or possession could
// plausibly matter to somebody, which is a judgement no filter makes.
//
// See statics/SKSE/Plugins/NarrativeEngine/PlotItems.ini for the file,
// and docs/implementation/PHASE_14_FACTION_PLOTS.md step 15.
namespace NarrativeEngine::PlotItemPool
{
    // What KIND of thing this is, handed to the model as context so a
    // blackmail scheme reaches for a document and a smuggling one
    // reaches for skooma, rather than either reaching for a garnet.
    //
    // Not a rule the simulation enforces. Nothing rejects an Acquire
    // step for wanting the wrong category; the categories exist to make
    // the sensible choice the easy one.
    enum class Category : std::uint8_t
    {
        Valuable,   // portable wealth -- worth taking, worth missing
        Document,   // paper that proves something
        Contraband, // illegal to hold, worse to be caught holding
        Drink,      // what gets poisoned, watered, stolen or switched

        Count
    };

    [[nodiscard]] std::string_view CategoryId(Category c) noexcept;
    [[nodiscard]] bool ParseCategory(std::string_view id, Category& out) noexcept;

    struct Item
    {
        RE::FormID form = 0;
        // Written the way somebody would say it out loud, lower case and
        // with its article, because it is dropped into a sentence:
        // "acquire a bottle of Black-Briar mead".
        std::string displayName;
        Category category = Category::Valuable;
    };

    // Load at kDataLoaded, after the data handler is up. Idempotent.
    //
    // An EditorID that does not resolve costs that LINE, named in the
    // log, and every other line still loads -- a mod that removes one
    // object must not cost the pool. A missing file leaves the pool
    // empty, which makes `Acquire` steps vaguer rather than broken.
    void Load();

    [[nodiscard]] bool IsLoaded();

    // Stable for the session once loaded, so the plot worker may read it
    // with no lock. Order is the file's order, which is what makes a
    // menu index mean the same thing twice.
    [[nodiscard]] const std::vector<Item>& Items();

    // --- The parse, before anything is resolved -------------------------
    //
    // Split from resolution for the reason PlotFactionRoster documents:
    // every malformed-input rule lives in the parse, and none of it can
    // be exercised if reaching it needs a running game.

    struct RawItem
    {
        std::string editorId;
        std::string displayName;
        Category category = Category::Valuable;
    };

    struct ParseReport
    {
        std::vector<RawItem> items;
        // One line per rejected line, each naming what and why. Kept as
        // data rather than logged from inside so a probe can assert on
        // the reason rather than only the count.
        std::vector<std::string> warnings;
    };

    // Pure. `iniText` is the file's contents, not a path.
    [[nodiscard]] ParseReport ParseItems(const std::string& iniText);
} // namespace NarrativeEngine::PlotItemPool
