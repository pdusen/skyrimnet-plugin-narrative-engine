#include "GossipWorld.h"

// See GossipWorld.h for the shape and why it is shared.

namespace NarrativeEngine::Testing
{
    namespace
    {
        constexpr std::uint32_t kWhiterunHold = 0x00016BE4u;
        constexpr std::uint32_t kWhiterun = 0x00013163u;
        constexpr std::uint32_t kBanneredMare = 0x000165A8u;
        constexpr std::uint32_t kFalkreathHold = 0x0001680Fu;
        constexpr std::uint32_t kFalkreath = 0x00018A56u;
        constexpr std::uint32_t kDeadMansDrink = 0x00018A57u;

        constexpr std::uint32_t kHulda = 0x0001A67Au;
        constexpr std::uint32_t kSaadia = 0x0001A67Bu;
        constexpr std::uint32_t kYsolda = 0x0001A6A0u;
        constexpr std::uint32_t kValga = 0x0001A6A1u;

        constexpr std::uint32_t kCompanions = 0x0004D8ACu;
    } // namespace

    GossipWorld BuildGossipWorld(EngineMock& engine)
    {
        GossipWorld world;
        world.whiterunHold = kWhiterunHold;
        world.whiterun = kWhiterun;
        world.banneredMare = kBanneredMare;
        world.falkreathHold = kFalkreathHold;
        world.falkreath = kFalkreath;
        world.deadMansDrink = kDeadMansDrink;
        world.hulda = kHulda;
        world.saadia = kSaadia;
        world.ysolda = kYsolda;
        world.valga = kValga;
        world.companions = kCompanions;

        // The tier keywords, one per tier. A location may carry more than one
        // and the tiers then collapse onto it, which is real in the data — an
        // Orc stronghold is both a settlement and a household — but nothing
        // here needs that case.
        auto* holdKw = engine.AddKeyword("LocTypeHold");
        auto* townKw = engine.AddKeyword("LocTypeTown");
        auto* innKw = engine.AddKeyword("LocTypeInn");
        (void)holdKw;
        (void)townKw;
        (void)innKw;

        auto* whiterunHold =
            engine.AddLocation(kWhiterunHold, "Whiterun Hold", {"LocTypeHold"}, "WhiterunHoldLocation");
        auto* whiterun = engine.AddLocation(kWhiterun, "Whiterun", {"LocTypeTown"}, "WhiterunLocation");
        auto* banneredMare =
            engine.AddLocation(kBanneredMare, "The Bannered Mare", {"LocTypeInn"}, "WhiterunBanneredMareLocation");
        engine.SetLocationParent(whiterun, whiterunHold);
        engine.SetLocationParent(banneredMare, whiterun);

        auto* falkreathHold =
            engine.AddLocation(kFalkreathHold, "Falkreath Hold", {"LocTypeHold"}, "FalkreathHoldLocation");
        auto* falkreath = engine.AddLocation(kFalkreath, "Falkreath", {"LocTypeTown"}, "FalkreathLocation");
        auto* deadMansDrink =
            engine.AddLocation(kDeadMansDrink, "Dead Man's Drink", {"LocTypeInn"}, "FalkreathDeadMansDrinkLocation");
        engine.SetLocationParent(falkreath, falkreathHold);
        engine.SetLocationParent(deadMansDrink, falkreath);

        auto* hulda = engine.AddNPC(kHulda, "Hulda", /*female=*/true);
        auto* saadia = engine.AddNPC(kSaadia, "Saadia", /*female=*/true);
        auto* ysolda = engine.AddNPC(kYsolda, "Ysolda", /*female=*/true);
        auto* valga = engine.AddNPC(kValga, "Valga Vinicia", /*female=*/true);
        for (auto* npc : {hulda, saadia, ysolda, valga})
            npc->actorData.actorBaseFlags.set(RE::ACTOR_BASE_DATA::Flag::kUnique);

        // Residence rows are carried by the town but name the finer place, the
        // way a town's LCUN array names the houses inside it.
        engine.AddResident(whiterun, hulda, banneredMare);
        engine.AddResident(whiterun, saadia, banneredMare);
        engine.AddResident(whiterun, ysolda, whiterun);
        engine.AddResident(falkreath, valga, deadMansDrink);

        // A faction spanning the two holds, which is the channel that actually
        // carries a rumor across one in the vanilla data. Three members,
        // because a faction below the configured floor is not admitted as a
        // channel at all — a household by another name is not a organisation.
        // Hulda is deliberately left out of it, so there is a pair in this
        // world with nothing at all between them.
        auto* companions = engine.AddFaction(kCompanions, "CompanionsFaction");
        engine.JoinFaction(ysolda, companions);
        engine.JoinFaction(valga, companions);
        engine.JoinFaction(saadia, companions);

        return world;
    }
} // namespace NarrativeEngine::Testing
