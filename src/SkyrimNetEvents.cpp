#include <SkyrimNetEvents.h>

#include <BookTextRenderer.h>
#include <JsonUtils.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

namespace NarrativeEngine::SkyrimNetEvents
{
    nlohmann::json RelativeGameTime(double seconds)
    {
        const auto bucket = [](double count, const char* unit) {
            return nlohmann::json{{"value", static_cast<int>(count)}, {"unit", unit}};
        };
        if (seconds < 60.0)
            return bucket(0.0, "moment");
        if (seconds < 3600.0)
            return bucket(seconds / 60.0, "minute");
        if (seconds < 86400.0)
            return bucket(seconds / 3600.0, "hour");
        if (seconds < 7.0 * 86400.0)
            return bucket(seconds / 86400.0, "day");
        return bucket(seconds / (7.0 * 86400.0), "week");
    }

    double MemoryAgeGameSeconds(const nlohmann::json& memoryRow, double nowGameSeconds)
    {
        if (!memoryRow.is_object() || nowGameSeconds <= 0.0) {
            return -1.0;
        }
        const auto it = memoryRow.find("game_time");
        if (it == memoryRow.end() || !it->is_number()) {
            return -1.0;
        }
        const double gameTime = it->get<double>();
        if (gameTime <= 0.0) {
            return -1.0;
        }
        // Clamp rather than return a negative age: a row written a few
        // simulation ticks ahead of the sampled clock is "just now", not
        // an error worth propagating to every caller.
        return std::max(0.0, nowGameSeconds - gameTime);
    }

    void DescribeEvents(nlohmann::json& events,
                        double currentGameTimeSeconds,
                        BookTextPolicy bookText,
                        std::string_view playerName)
    {
        for (auto& evt : events) {
            if (!evt.is_object())
                continue;

            const std::string type = JsonUtils::StringOr(evt, "type");

            // Normalize the book body before anything reads `data`, so both
            // the `line` below and the event object we hand back to the
            // caller are free of the raw markup.
            if (type == "book_read") {
                if (auto dataIt = evt.find("data"); dataIt != evt.end() && dataIt->is_object()) {
                    if (auto textIt = dataIt->find("book_text"); textIt != dataIt->end()) {
                        if (bookText == BookTextPolicy::Omit) {
                            dataIt->erase("book_text");
                        } else if (textIt->is_string()) {
                            std::string rendered =
                                BookTextRenderer::RenderToPlainText(textIt->get_ref<const std::string&>());
                            *textIt = std::move(rendered);
                        }
                    }
                }
            }

            const nlohmann::json* data = nullptr;
            if (auto it = evt.find("data"); it != evt.end() && it->is_object()) {
                data = &(*it);
            }
            // JsonUtils::StringOr rather than nlohmann's own value(). value()
            // falls back only on an ABSENT key: a key that is PRESENT holding
            // null -- or any non-string -- throws type_error.302 instead, and
            // the throw escapes this whole loop, so one malformed field costs
            // every event in the batch. SkyrimNet emits explicit nulls as a
            // matter of course.
            const auto str = [&](const char* key) -> std::string {
                return data ? JsonUtils::StringOr(*data, key) : std::string{};
            };

            nlohmann::json line;
            if (type == "dialogue" || type == "dialogue_background" || type == "dialogue_player_text") {
                line = {{"kind", type == "dialogue_player_text" ? "player_dialogue" : "dialogue"},
                        {"speaker", str("speaker")},
                        {"listener", str("listener")},
                        {"dialogue", str("dialogue")}};
            } else if (type == "gamemaster_dialogue") {
                line = {{"kind", "gamemaster_dialogue"},
                        {"speaker", str("speaker")},
                        {"target", str("target")},
                        {"topic", str("topic")},
                        {"dialogue", str("dialogue")}};
            } else if (type == "npc_thoughts") {
                line = {{"kind", "npc_thoughts"}, {"name", str("npc_name")}, {"thoughts", str("thoughts")}};
            } else if (type == "death") {
                line = {{"kind", "death"}, {"killer", str("killer")}, {"victim", str("victim")}};
            } else if (type == "persistent_generic") {
                line = {{"kind", "text"}, {"text", str("line")}};
            } else if (type == "book_read") {
                // Given its own kind rather than the fallback below: dumping
                // its data is what put ~20KB of tag soup into a Director
                // prompt and blew the request. SkyrimNet carries three
                // name-ish fields; `book_title` is the in-fiction one,
                // `book_name` the record's display name (usually identical),
                // and the EditorID is a last resort so a nameless book still
                // reads as something rather than "a book".
                std::string title = str("book_title");
                if (title.empty())
                    title = str("book_name");
                if (title.empty())
                    title = str("book_editor_id");

                // Empty under BookTextPolicy::Omit -- erased above.
                line = {{"kind", "book_read"},
                        {"reader", std::string{playerName}},
                        {"title", std::move(title)},
                        {"body", str("book_text")}};
            } else {
                // Unknown discriminator. Many third-party SkyrimNet plugins
                // (e.g. SeverActions' "follower_left") emit events whose
                // `data` is just a pre-rendered string sentence -- perfect
                // for our purposes. Use it directly when that's the shape.
                // Fall back to a JSON dump of an object payload, and to
                // no_data when the field is missing.
                const auto it = evt.find("data");
                if (it != evt.end() && it->is_string()) {
                    line = {{"kind", "text"}, {"text", it->get<std::string>()}};
                } else if (it != evt.end() && (it->is_object() || it->is_array())) {
                    line = {{"kind", "text"}, {"text", it->dump()}};
                } else {
                    line = {{"kind", "no_data"}};
                }
            }

            if (evt.contains("gameTime") && evt["gameTime"].is_number()) {
                const double eventTime = evt["gameTime"].get<double>();
                const double delta = currentGameTimeSeconds - eventTime;
                evt["age"] = RelativeGameTime(delta < 0.0 ? 0.0 : delta);
            }

            evt["line"] = std::move(line);
        }
    }

    namespace
    {
        // ---- Condensation pieces for BuildMergedTimeline -----------------

        bool IsInternalEvent(const nlohmann::json& e)
        {
            return e.is_object() && e.contains("ne_kind") && e["ne_kind"].is_string();
        }

        bool IsHit(const nlohmann::json& e)
        {
            return IsInternalEvent(e) && e["ne_kind"].get<std::string>() == "hit";
        }

        struct ActorPairStats
        {
            std::string a, b; // canonical (a <= b)
            int aToB = 0;
            int bToA = 0;
            int Total() const
            {
                return aToB + bToA;
            }
        };

        struct EnvBucket
        {
            std::string target;
            std::string source;
            std::string actor;
            int count = 0;
        };

        // A condensed entry's `line` from the pending hits. Rules:
        //   - 1 actor-vs-actor hit         -> one fight, shape strike
        //   - 1 pair, one-sided (count>1)  -> one fight, shape attack
        //   - 1 pair, both sides           -> one fight, shape trade
        //   - multiple pairs               -> the top 3 by hits, each shape
        //                                     attack, and `others` if more
        //   - any other hit                -> damage to its target, grouped by
        //                                     target and source, `repeated`
        //                                     past one
        nlohmann::json CondensedLine(const std::vector<nlohmann::json>& hits)
        {
            std::vector<ActorPairStats> pairs;
            std::vector<EnvBucket> envs;

            auto findPair = [&](const std::string& canA, const std::string& canB) -> ActorPairStats& {
                for (auto& p : pairs) {
                    if (p.a == canA && p.b == canB)
                        return p;
                }
                pairs.push_back({canA, canB, 0, 0});
                return pairs.back();
            };
            auto findEnv =
                [&](const std::string& target, const std::string& source, const std::string& actor) -> EnvBucket& {
                for (auto& e : envs) {
                    if (e.target == target && e.source == source && e.actor == actor)
                        return e;
                }
                envs.push_back({target, source, actor, 0});
                return envs.back();
            };

            for (const auto& h : hits) {
                static const nlohmann::json kNoLine = nlohmann::json::object();
                const auto lineIt = h.find("line");
                const nlohmann::json& hit = lineIt != h.end() ? *lineIt : kNoLine;
                const std::string source = JsonUtils::StringOr(hit, "source");
                const std::string actor = JsonUtils::StringOr(hit, "actor");
                const std::string targ = JsonUtils::StringOr(hit, "target");
                if (source == "actor" && !actor.empty() && !targ.empty()) {
                    const std::string canA = (actor < targ) ? actor : targ;
                    const std::string canB = (actor < targ) ? targ : actor;
                    auto& p = findPair(canA, canB);
                    if (actor == canA)
                        p.aToB++;
                    else
                        p.bToA++;
                } else {
                    findEnv(targ, source, actor).count++;
                }
            }

            const auto fight = [](const std::string& aggressor, const std::string& victim, const char* shape) {
                return nlohmann::json{{"aggressor", aggressor}, {"victim", victim}, {"shape", shape}};
            };

            nlohmann::json fights = nlohmann::json::array();
            bool others = false;
            if (pairs.size() == 1) {
                auto& p = pairs.front();
                const std::string& aggressor = (p.aToB > 0) ? p.a : p.b;
                const std::string& victim = (p.aToB > 0) ? p.b : p.a;
                if (p.Total() <= 1) {
                    fights.push_back(fight(aggressor, victim, "strike"));
                } else if (p.aToB == 0 || p.bToA == 0) {
                    fights.push_back(fight(aggressor, victim, "attack"));
                } else {
                    fights.push_back(fight(p.a, p.b, "trade"));
                }
            } else if (pairs.size() > 1) {
                // Multiple distinct pairs -- top 3 by total hits.
                std::sort(pairs.begin(), pairs.end(), [](const ActorPairStats& x, const ActorPairStats& y) {
                    return x.Total() > y.Total();
                });
                const std::size_t cap = std::min<std::size_t>(3, pairs.size());
                for (std::size_t i = 0; i < cap; ++i) {
                    const auto& p = pairs[i];
                    const std::string& aggressor = (p.aToB >= p.bToA) ? p.a : p.b;
                    const std::string& victim = (p.aToB >= p.bToA) ? p.b : p.a;
                    fights.push_back(fight(aggressor, victim, "attack"));
                }
                others = pairs.size() > cap;
            }

            nlohmann::json damage = nlohmann::json::array();
            for (const auto& e : envs) {
                damage.push_back(
                    {{"target", e.target}, {"source", e.source}, {"actor", e.actor}, {"repeated", e.count > 1}});
            }

            return {{"kind", "combat_summary"},
                    {"fights", std::move(fights)},
                    {"others", others},
                    {"damage", std::move(damage)}};
        }

        // Build the final condensed JSON entry from the pending-hits buffer.
        // Inherits the timestamps and age of the *most recent* hit in the
        // run.
        nlohmann::json BuildCondensedEntry(const std::vector<nlohmann::json>& hits, double currentGameTimeSeconds)
        {
            const auto& tail = hits.back();
            const double localTime = JsonUtils::NumberOr(tail, "localTime");
            const double gameTime = JsonUtils::NumberOr(tail, "gameTime");
            const double delta = currentGameTimeSeconds - gameTime;

            nlohmann::json j;
            // Same shared `type` the individual combat events use, so the
            // dashboard / prompt see a single uniform label across the
            // whole combat family. `ne_kind` keeps the finer label.
            j["type"] = "combat_event";
            j["ne_kind"] = "combat_summary";
            j["localTime"] = localTime;
            j["gameTime"] = gameTime;
            j["age"] = RelativeGameTime(delta < 0.0 ? 0.0 : delta);
            j["line"] = CondensedLine(hits);
            return j;
        }
    } // namespace

    nlohmann::json BuildMergedTimeline(nlohmann::json skyrimNetEvents,
                                       nlohmann::json combatEvents,
                                       nlohmann::json weatherEvents,
                                       nlohmann::json travelEvents,
                                       double currentGameTimeSeconds)
    {
        // Game-time staleness: drop anything past this age, append an idle
        // marker once we go this long without anything fresh. Game time so
        // the player's `wait`/`sleep` actions advance the clock the same
        // way real-time elapsed activity would.
        constexpr double kMaxEventAgeGameSeconds = 3600.0;        // 1 hour
        constexpr double kIdleMarkerThresholdGameSeconds = 600.0; // 10 min

        // Concatenate, guarding against the inputs not being arrays.
        std::vector<nlohmann::json> merged;
        const std::size_t total = (skyrimNetEvents.is_array() ? skyrimNetEvents.size() : 0)
                                  + (combatEvents.is_array() ? combatEvents.size() : 0)
                                  + (weatherEvents.is_array() ? weatherEvents.size() : 0)
                                  + (travelEvents.is_array() ? travelEvents.size() : 0);
        merged.reserve(total);
        if (skyrimNetEvents.is_array()) {
            for (auto& e : skyrimNetEvents)
                merged.push_back(std::move(e));
        }
        if (combatEvents.is_array()) {
            for (auto& e : combatEvents)
                merged.push_back(std::move(e));
        }
        if (weatherEvents.is_array()) {
            for (auto& e : weatherEvents)
                merged.push_back(std::move(e));
        }
        if (travelEvents.is_array()) {
            for (auto& e : travelEvents)
                merged.push_back(std::move(e));
        }

        // Age cap (game-time). Applied pre-condensation so straddling runs
        // of hits get summarized from only the still-fresh hits, not from
        // a mix that includes hour-old ones.
        merged.erase(std::remove_if(merged.begin(),
                                    merged.end(),
                                    [currentGameTimeSeconds](const nlohmann::json& e) {
                                        if (!e.is_object())
                                            return true;
                                        const double gt = JsonUtils::NumberOr(e, "gameTime");
                                        return (currentGameTimeSeconds - gt) > kMaxEventAgeGameSeconds;
                                    }),
                     merged.end());

        // Stable sort by localTime ascending so equal timestamps preserve
        // input order (SkyrimNet events tend to precede our internal events
        // when they share a tick boundary).
        std::stable_sort(merged.begin(), merged.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
            const double at = JsonUtils::NumberOr(a, "localTime");
            const double bt = JsonUtils::NumberOr(b, "localTime");
            return at < bt;
        });

        nlohmann::json out = nlohmann::json::array();
        std::vector<nlohmann::json> pendingHits;

        const auto flushPending = [&]() {
            if (pendingHits.empty())
                return;
            out.push_back(BuildCondensedEntry(pendingHits, currentGameTimeSeconds));
            pendingHits.clear();
        };

        for (auto& evt : merged) {
            if (IsHit(evt)) {
                pendingHits.push_back(std::move(evt));
            } else {
                flushPending();
                out.push_back(std::move(evt));
            }
        }
        flushPending();

        // Idle marker — synthetic "newest" entry the LLM will see at the
        // bottom of the list when nothing has happened recently. Without
        // this, the LLM keeps re-reading the same stale combat events tick
        // after tick and tension stays artificially pinned high long after
        // the actual fight has ended. The marker gives it a concrete signal
        // that time has passed and the world is currently quiet.
        bool needMarker = false;
        double sinceSeconds = 0.0;
        if (out.empty()) {
            // Everything got filtered (or nothing was ever there). Say
            // idle_long, "over an hour" — we don't know the exact
            // gap because we just dropped the last event we could have
            // measured against.
            needMarker = true;
            sinceSeconds = -1.0;
        } else {
            const auto& last = out.back();
            const double gt = JsonUtils::NumberOr(last, "gameTime");
            const double delta = currentGameTimeSeconds - gt;
            if (delta > kIdleMarkerThresholdGameSeconds) {
                needMarker = true;
                sinceSeconds = delta;
            }
        }
        if (needMarker) {
            nlohmann::json marker;
            marker["type"] = "idle_marker";
            marker["ne_kind"] = "idle_marker";
            // Game time = "now" so the marker sits naturally at the tail
            // by gameTime ordering. localTime uses the same Unix-epoch
            // basis as real events so any downstream sort puts it last.
            marker["gameTime"] = currentGameTimeSeconds;
            marker["localTime"] =
                std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
            marker["line"] = (sinceSeconds < 0.0)
                                 ? nlohmann::json{{"kind", "idle_long"}}
                                 : nlohmann::json{{"kind", "idle"}, {"duration", RelativeGameTime(sinceSeconds)}};
            out.push_back(std::move(marker));
        }

        return out;
    }
} // namespace NarrativeEngine::SkyrimNetEvents
