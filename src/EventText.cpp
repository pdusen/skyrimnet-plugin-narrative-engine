#include <EventText.h>

#include <JsonUtils.h>

#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace NarrativeEngine::EventText
{
    namespace
    {
        using JsonUtils::StringOr;
        using LanguageTable::Table;
        using TextKeys::Key;
        using TextKeys::Param;
        using TextTemplate::Arguments;
        using TextTemplate::Kind;

        constexpr std::array kLineParams{Param{"age"}, Param{"body"}};
        constexpr std::array kIdleParams{Param{"duration"}};
        constexpr std::array kCountParams{Param{"n", Kind::Number}};
        constexpr Key kLine{"Event", "line", kLineParams};
        constexpr Key kIdle{"Event", "idle", kIdleParams};
        constexpr Key kIdleLong{"Event", "idle_long", {}};

        // Each time section holds one key per unit, by the unit's token.
        constexpr std::array<std::string_view, 5> kUnits{"moment", "minute", "hour", "day", "week"};
        constexpr std::array kAges{Key{"Event.Age", "moment", {}},
                                   Key{"Event.Age", "minute", kCountParams},
                                   Key{"Event.Age", "hour", kCountParams},
                                   Key{"Event.Age", "day", kCountParams},
                                   Key{"Event.Age", "week", kCountParams}};
        constexpr Key kAgeUnknown{"Event.Age", "unknown", {}};
        constexpr std::array kDurations{Key{"Event.Duration", "moment", {}},
                                        Key{"Event.Duration", "minute", kCountParams},
                                        Key{"Event.Duration", "hour", kCountParams},
                                        Key{"Event.Duration", "day", kCountParams},
                                        Key{"Event.Duration", "week", kCountParams}};

        constexpr std::array kDialogueParams{Param{"speaker"}, Param{"listener"}, Param{"dialogue"}};
        constexpr std::array kGamemasterParams{Param{"speaker"}, Param{"target"}, Param{"topic"}, Param{"dialogue"}};
        constexpr std::array kThoughtsParams{Param{"name"}, Param{"thoughts"}};
        constexpr std::array kDeathParams{Param{"killer"}, Param{"victim"}};
        constexpr std::array kBookParams{Param{"reader"}, Param{"title"}};
        constexpr std::array kBookTextParams{Param{"line"}, Param{"body"}};
        constexpr Key kDialogue{"Event.SkyrimNet", "dialogue", kDialogueParams};
        constexpr Key kDialogueTo{"Event.SkyrimNet", "dialogue.to", kDialogueParams};
        constexpr Key kPlayerDialogue{"Event.SkyrimNet", "player_dialogue", kDialogueParams};
        constexpr Key kPlayerDialogueTo{"Event.SkyrimNet", "player_dialogue.to", kDialogueParams};
        constexpr Key kGamemaster{"Event.SkyrimNet", "gamemaster_dialogue", kGamemasterParams};
        constexpr Key kThoughts{"Event.SkyrimNet", "npc_thoughts", kThoughtsParams};
        constexpr Key kDeath{"Event.SkyrimNet", "death", kDeathParams};
        constexpr Key kBookRead{"Event.SkyrimNet", "book_read", kBookParams};
        constexpr Key kBookUntitled{"Event.SkyrimNet", "book_read.untitled", kBookParams};
        constexpr Key kBookWithText{"Event.SkyrimNet", "book_read.with_text", kBookTextParams};
        constexpr Key kUnknownReader{"Event.SkyrimNet", "unknown.reader", {}};
        constexpr Key kNoData{"Event.SkyrimNet", "no_data", {}};

        constexpr std::array kActorParams{Param{"actor"}};
        constexpr std::array kBlowParams{Param{"actor"}, Param{"target"}};
        constexpr std::array kDamageParams{Param{"target"}, Param{"source"}};
        constexpr std::array kFightsParams{Param{"fights"}};
        constexpr std::array kJoinParams{Param{"first"}, Param{"then"}};
        constexpr Key kCombatStart{"Event.Combat", "combat_start", kActorParams};
        constexpr Key kCombatEnd{"Event.Combat", "combat_end", kActorParams};
        constexpr Key kCollapse{"Event.Combat", "collapse", kActorParams};
        constexpr Key kRegainFooting{"Event.Combat", "regain_footing", kActorParams};
        constexpr Key kStrike{"Event.Combat", "strike", kBlowParams};
        constexpr Key kAttack{"Event.Combat", "attack", kBlowParams};
        constexpr Key kTrade{"Event.Combat", "trade", kBlowParams};
        constexpr Key kDamage{"Event.Combat", "damage", kDamageParams};
        constexpr Key kDamageFrom{"Event.Combat", "damage.from", kDamageParams};
        constexpr Key kDamageRepeated{"Event.Combat", "damage.repeated", kDamageParams};
        constexpr Key kDamageRepeatedFrom{"Event.Combat", "damage.repeated.from", kDamageParams};
        constexpr Key kSourcePoison{"Event.Combat", "source.poison", {}};
        constexpr Key kSourceSpellTrap{"Event.Combat", "source.spell_trap", {}};
        constexpr Key kSourceTrap{"Event.Combat", "source.trap", {}};
        constexpr Key kSeveral{"Event.Combat", "several", kFightsParams};
        constexpr Key kOthers{"Event.Combat", "others", kFightsParams};
        constexpr Key kJoin{"Event.Combat", "join", kJoinParams};

        // One key per weather `change`, by its slug; the last is the
        // fallback for one this table does not know.
        constexpr std::array kWeather{Key{"Event.Weather", "blizzard_start", {}},
                                      Key{"Event.Weather", "storm_intensify", {}},
                                      Key{"Event.Weather", "storm_subside", {}},
                                      Key{"Event.Weather", "storm_start", {}},
                                      Key{"Event.Weather", "rain_start", {}},
                                      Key{"Event.Weather", "snow_start", {}},
                                      Key{"Event.Weather", "rain_stop", {}},
                                      Key{"Event.Weather", "snow_stop", {}},
                                      Key{"Event.Weather", "clear_start", {}},
                                      Key{"Event.Weather", "cloudy_start", {}},
                                      Key{"Event.Weather", "weather_shift", {}}};

        constexpr std::array kTravelParams{Param{"party", Kind::List},
                                           Param{"count", Kind::Number},
                                           Param{"place"},
                                           Param{"from"},
                                           Param{"to"},
                                           Param{"origin"},
                                           Param{"hold"},
                                           Param{"arrival"},
                                           Param{"places", Kind::List}};
        constexpr Key kEnteredLocation{"Event.Travel", "entered_location", kTravelParams};
        constexpr Key kEnteredInterior{"Event.Travel", "entered_location.interior", kTravelParams};
        constexpr Key kLeftLocation{"Event.Travel", "left_location", kTravelParams};
        constexpr Key kLeftInterior{"Event.Travel", "left_location.interior", kTravelParams};
        constexpr Key kCrossedHolds{"Event.Travel", "crossed_holds", kTravelParams};
        constexpr Key kWilderness{"Event.Travel", "entered_wilderness", kTravelParams};
        constexpr Key kWildernessExited{"Event.Travel", "entered_wilderness.exited", kTravelParams};
        constexpr Key kFastTravel{"Event.Travel", "fast_travel_arrived", kTravelParams};
        constexpr Key kFastTravelInterior{"Event.Travel", "fast_travel_arrived.interior", kTravelParams};
        constexpr Key kFastTravelNowhere{"Event.Travel", "fast_travel_arrived.nowhere", kTravelParams};
        constexpr Key kFastTravelInteriorNowhere{"Event.Travel", "fast_travel_arrived.interior.nowhere", kTravelParams};
        constexpr Key kJourney{"Event.Travel", "journey", kTravelParams};
        constexpr Key kJourneyTo{"Event.Travel", "journey.to", kTravelParams};
        constexpr Key kJourneyFrom{"Event.Travel", "journey.from", kTravelParams};
        constexpr Key kJourneyNowhere{"Event.Travel", "journey.nowhere", kTravelParams};
        constexpr Key kVisit{"Event.Travel", "visit", kTravelParams};
        constexpr Key kVisitNowhere{"Event.Travel", "visit.nowhere", kTravelParams};
        constexpr Key kTravelUnknown{"Event.Travel", "unknown", kTravelParams};
        constexpr Key kUnknownPlayer{"Event.Travel", "unknown.player", {}};

        bool Flag(const nlohmann::json& obj, std::string_view key)
        {
            if (!obj.is_object()) {
                return false;
            }
            const auto it = obj.find(key);
            return it != obj.end() && it->is_boolean() && it->get<bool>();
        }

        const nlohmann::json& Field(const nlohmann::json& obj, std::string_view key)
        {
            static const nlohmann::json kNull;
            if (!obj.is_object()) {
                return kNull;
            }
            const auto it = obj.find(key);
            return it == obj.end() ? kNull : *it;
        }

        TextTemplate::List Strings(const nlohmann::json& array)
        {
            TextTemplate::List out;
            if (!array.is_array()) {
                return out;
            }
            for (const auto& item : array) {
                out.push_back(item.is_string() ? item.get<std::string>() : std::string{});
            }
            return out;
        }

        // The key for an `age` bucket's unit in one of the two time
        // sections, or null for a unit it does not know.
        const Key* UnitKey(const std::array<Key, 5>& keys, const nlohmann::json& age)
        {
            const std::string unit = StringOr(age, "unit");
            for (std::size_t i = 0; i < kUnits.size(); ++i) {
                if (unit == kUnits[i]) {
                    return &keys[i];
                }
            }
            return nullptr;
        }

        std::string Counted(const Table& table, const Key* key, const nlohmann::json& age)
        {
            if (key == nullptr) {
                return {};
            }
            Arguments args;
            args.Set("n", static_cast<std::int64_t>(JsonUtils::NumberOr(age, "value")));
            return table.Render(*key, args);
        }

        // Every field of `line` a template may name, as text.
        Arguments Fields(const nlohmann::json& line)
        {
            Arguments args;
            if (!line.is_object()) {
                return args;
            }
            for (const auto& [name, value] : line.items()) {
                if (value.is_string()) {
                    args.Set(name, value.get<std::string>());
                }
            }
            return args;
        }

        std::string SourceWords(const Table& table, const nlohmann::json& hit)
        {
            const std::string source = StringOr(hit, "source");
            if (source == "poison") {
                return table.Render(kSourcePoison, {});
            }
            if (source == "spell_trap") {
                return table.Render(kSourceSpellTrap, {});
            }
            if (source == "trap") {
                return table.Render(kSourceTrap, {});
            }
            // An activator's name, or, in a condensed run, an actor whose blow
            // had no one named to receive it.
            if (source == "activator" || source == "actor") {
                return StringOr(hit, "actor");
            }
            return {};
        }

        std::string Damage(const Table& table, const nlohmann::json& d, bool repeated)
        {
            const std::string from = SourceWords(table, d);
            Arguments args;
            args.Set("target", StringOr(d, "target")).Set("source", from);
            if (repeated) {
                return table.Render(from.empty() ? kDamageRepeated : kDamageRepeatedFrom, args);
            }
            return table.Render(from.empty() ? kDamage : kDamageFrom, args);
        }

        std::string Fight(const Table& table, const nlohmann::json& f)
        {
            const std::string shape = StringOr(f, "shape");
            Arguments args;
            args.Set("actor", StringOr(f, "aggressor")).Set("target", StringOr(f, "victim"));
            if (shape == "strike") {
                return table.Render(kStrike, args);
            }
            if (shape == "trade") {
                return table.Render(kTrade, args);
            }
            return table.Render(kAttack, args);
        }

        std::string Join(const Table& table, std::string first, std::string then)
        {
            if (first.empty()) {
                return then;
            }
            Arguments args;
            args.Set("first", std::move(first)).Set("then", std::move(then));
            return table.Render(kJoin, args);
        }

        std::string CombatSummary(const Table& table, const nlohmann::json& line)
        {
            std::string body;
            const auto& fights = Field(line, "fights");
            if (fights.is_array() && fights.size() == 1) {
                body = Fight(table, fights[0]);
            } else if (fights.is_array() && fights.size() > 1) {
                std::string all;
                for (const auto& f : fights) {
                    all = Join(table, std::move(all), Fight(table, f));
                }
                if (Flag(line, "others")) {
                    Arguments args;
                    args.Set("fights", std::move(all));
                    all = table.Render(kOthers, args);
                }
                Arguments args;
                args.Set("fights", std::move(all));
                body = table.Render(kSeveral, args);
            }
            if (const auto& damage = Field(line, "damage"); damage.is_array()) {
                for (const auto& d : damage) {
                    body = Join(table, std::move(body), Damage(table, d, Flag(d, "repeated")));
                }
            }
            return body;
        }

        std::string Weather(const Table& table, const nlohmann::json& line)
        {
            const std::string change = StringOr(line, "change");
            for (const auto& key : kWeather) {
                if (key.name == change) {
                    return table.Render(key, {});
                }
            }
            return table.Render(kWeather.back(), {});
        }

        std::string Travel(const Table& table, const nlohmann::json& line)
        {
            // The player is first, and saved empty when unnamed.
            auto party = Strings(Field(line, "party"));
            if (party.empty()) {
                party.emplace_back();
            }
            if (party[0].empty()) {
                party[0] = table.Render(kUnknownPlayer, {});
            }
            Arguments args = Fields(line);
            args.Set("count", static_cast<std::int64_t>(party.size()))
                .Set("party", std::move(party))
                .Set("places", Strings(Field(line, "places")));

            const std::string move = StringOr(line, "move");
            const bool interior = Flag(line, "interior");
            const auto render = [&](const Key& key) { return table.Render(key, args); };
            if (move == "entered_location") {
                return render(interior ? kEnteredInterior : kEnteredLocation);
            }
            if (move == "left_location") {
                return render(interior ? kLeftInterior : kLeftLocation);
            }
            if (move == "crossed_holds") {
                return render(kCrossedHolds);
            }
            if (move == "entered_wilderness") {
                return render(Flag(line, "exited") ? kWildernessExited : kWilderness);
            }
            if (move == "fast_travel_arrived") {
                if (StringOr(line, "origin").empty()) {
                    return render(interior ? kFastTravelInteriorNowhere : kFastTravelNowhere);
                }
                return render(interior ? kFastTravelInterior : kFastTravel);
            }
            if (move == "journey") {
                const bool origin = !StringOr(line, "origin").empty();
                const bool arrival = !StringOr(line, "arrival").empty();
                if (!origin && !arrival) {
                    return render(kJourneyNowhere);
                }
                if (!origin) {
                    return render(kJourneyTo);
                }
                if (!arrival) {
                    return render(kJourneyFrom);
                }
                return render(kJourney);
            }
            if (move == "visit") {
                const auto& places = Field(line, "places");
                return render(places.is_array() && !places.empty() ? kVisit : kVisitNowhere);
            }
            return render(kTravelUnknown);
        }

        std::string BookRead(const Table& table, const nlohmann::json& line)
        {
            Arguments args;
            const std::string reader = StringOr(line, "reader");
            args.Set("reader", reader.empty() ? table.Render(kUnknownReader, {}) : reader)
                .Set("title", StringOr(line, "title"));
            std::string text = table.Render(StringOr(line, "title").empty() ? kBookUntitled : kBookRead, args);
            if (std::string body = StringOr(line, "body"); !body.empty()) {
                Arguments withText;
                withText.Set("line", std::move(text)).Set("body", std::move(body));
                text = table.Render(kBookWithText, withText);
            }
            return text;
        }

        std::string Dialogue(const Table& table, const nlohmann::json& line, const Key& alone, const Key& to)
        {
            return table.Render(StringOr(line, "listener").empty() ? alone : to, Fields(line));
        }

        std::string Actor(const Table& table, const Key& key, const nlohmann::json& line)
        {
            return table.Render(key, Fields(line));
        }
    } // namespace

    const std::vector<TextKeys::Key>& Keys()
    {
        static const std::vector<Key> keys = [] {
            std::vector<Key> out{kLine, kIdle, kIdleLong, kAgeUnknown};
            out.insert(out.end(), kAges.begin(), kAges.end());
            out.insert(out.end(), kDurations.begin(), kDurations.end());
            for (const auto* key : {&kDialogue,
                                    &kDialogueTo,
                                    &kPlayerDialogue,
                                    &kPlayerDialogueTo,
                                    &kGamemaster,
                                    &kThoughts,
                                    &kDeath,
                                    &kBookRead,
                                    &kBookUntitled,
                                    &kBookWithText,
                                    &kUnknownReader,
                                    &kNoData,
                                    &kCombatStart,
                                    &kCombatEnd,
                                    &kCollapse,
                                    &kRegainFooting,
                                    &kStrike,
                                    &kAttack,
                                    &kTrade,
                                    &kDamage,
                                    &kDamageFrom,
                                    &kDamageRepeated,
                                    &kDamageRepeatedFrom,
                                    &kSourcePoison,
                                    &kSourceSpellTrap,
                                    &kSourceTrap,
                                    &kSeveral,
                                    &kOthers,
                                    &kJoin,
                                    &kEnteredLocation,
                                    &kEnteredInterior,
                                    &kLeftLocation,
                                    &kLeftInterior,
                                    &kCrossedHolds,
                                    &kWilderness,
                                    &kWildernessExited,
                                    &kFastTravel,
                                    &kFastTravelInterior,
                                    &kFastTravelNowhere,
                                    &kFastTravelInteriorNowhere,
                                    &kJourney,
                                    &kJourneyTo,
                                    &kJourneyFrom,
                                    &kJourneyNowhere,
                                    &kVisit,
                                    &kVisitNowhere,
                                    &kTravelUnknown,
                                    &kUnknownPlayer}) {
                out.push_back(*key);
            }
            out.insert(out.end(), kWeather.begin(), kWeather.end());
            return out;
        }();
        return keys;
    }

    std::string Ago(const Table& table, const nlohmann::json& age)
    {
        if (StringOr(age, "unit") == "unknown") {
            return table.Render(kAgeUnknown, {});
        }
        return Counted(table, UnitKey(kAges, age), age);
    }

    std::string Duration(const Table& table, const nlohmann::json& age)
    {
        return Counted(table, UnitKey(kDurations, age), age);
    }

    std::string Body(const Table& table, const nlohmann::json& line)
    {
        const std::string kind = StringOr(line, "kind");
        if (kind == "dialogue") {
            return Dialogue(table, line, kDialogue, kDialogueTo);
        }
        if (kind == "player_dialogue") {
            return Dialogue(table, line, kPlayerDialogue, kPlayerDialogueTo);
        }
        if (kind == "gamemaster_dialogue") {
            return table.Render(kGamemaster, Fields(line));
        }
        if (kind == "npc_thoughts") {
            return table.Render(kThoughts, Fields(line));
        }
        if (kind == "death") {
            return table.Render(kDeath, Fields(line));
        }
        if (kind == "book_read") {
            return BookRead(table, line);
        }
        if (kind == "text") {
            // Another plugin's own words: shown as they came.
            return StringOr(line, "text");
        }
        if (kind == "combat_start") {
            return Actor(table, kCombatStart, line);
        }
        if (kind == "combat_end") {
            return Actor(table, kCombatEnd, line);
        }
        if (kind == "collapse") {
            return Actor(table, kCollapse, line);
        }
        if (kind == "regain_footing") {
            return Actor(table, kRegainFooting, line);
        }
        if (kind == "hit") {
            if (StringOr(line, "source") == "actor") {
                return table.Render(kStrike, Fields(line));
            }
            return Damage(table, line, false);
        }
        if (kind == "combat_summary") {
            return CombatSummary(table, line);
        }
        if (kind == "weather") {
            return Weather(table, line);
        }
        if (kind == "travel") {
            return Travel(table, line);
        }
        if (kind == "idle") {
            Arguments args;
            args.Set("duration", Duration(table, Field(line, "duration")));
            return table.Render(kIdle, args);
        }
        if (kind == "idle_long") {
            return table.Render(kIdleLong, {});
        }
        return table.Render(kNoData, {});
    }

    std::string Line(const Table& table, const nlohmann::json& event)
    {
        std::string body = Body(table, Field(event, "line"));
        const auto& age = Field(event, "age");
        if (!age.is_object()) {
            return body;
        }
        Arguments args;
        args.Set("age", Ago(table, age)).Set("body", std::move(body));
        return table.Render(kLine, args);
    }
} // namespace NarrativeEngine::EventText
