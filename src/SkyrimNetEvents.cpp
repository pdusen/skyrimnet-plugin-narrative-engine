#include <SkyrimNetEvents.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

namespace NarrativeEngine::SkyrimNetEvents
{
    std::string FormatRelativeGameTime(double secondsAgo)
    {
        if (secondsAgo < 60.0)        return "just now";
        if (secondsAgo < 3600.0) {
            const int m = static_cast<int>(secondsAgo / 60.0);
            return std::to_string(m) + (m == 1 ? " minute ago" : " minutes ago");
        }
        if (secondsAgo < 86400.0) {
            const int h = static_cast<int>(secondsAgo / 3600.0);
            return std::to_string(h) + (h == 1 ? " hour ago" : " hours ago");
        }
        if (secondsAgo < 7.0 * 86400.0) {
            const int d = static_cast<int>(secondsAgo / 86400.0);
            return std::to_string(d) + (d == 1 ? " day ago" : " days ago");
        }
        const int w = static_cast<int>(secondsAgo / (7.0 * 86400.0));
        return std::to_string(w) + (w == 1 ? " week ago" : " weeks ago");
    }

    std::string FormatRelativeGameDuration(double seconds)
    {
        if (seconds < 60.0)        return "less than a minute";
        if (seconds < 3600.0) {
            const int m = static_cast<int>(seconds / 60.0);
            return std::to_string(m) + (m == 1 ? " minute" : " minutes");
        }
        if (seconds < 86400.0) {
            const int h = static_cast<int>(seconds / 3600.0);
            return std::to_string(h) + (h == 1 ? " hour" : " hours");
        }
        if (seconds < 7.0 * 86400.0) {
            const int d = static_cast<int>(seconds / 86400.0);
            return std::to_string(d) + (d == 1 ? " day" : " days");
        }
        const int w = static_cast<int>(seconds / (7.0 * 86400.0));
        return std::to_string(w) + (w == 1 ? " week" : " weeks");
    }

    void FormatEventsText(nlohmann::json &events, double currentGameTimeSeconds)
    {
        for (auto &evt : events) {
            if (!evt.is_object()) continue;

            const std::string type = evt.value("type", std::string{});

            const nlohmann::json *data = nullptr;
            if (auto it = evt.find("data"); it != evt.end() && it->is_object()) {
                data = &(*it);
            }
            const auto str = [&](const char *key) -> std::string {
                return data ? data->value(key, std::string{}) : std::string{};
            };

            std::string text;
            if (type == "dialogue" || type == "dialogue_background") {
                const std::string speaker  = str("speaker");
                const std::string listener = str("listener");
                const std::string dialogue = str("dialogue");
                if (!listener.empty()) {
                    text = speaker + " -> " + listener + ": \"" + dialogue + "\"";
                } else {
                    text = speaker + ": \"" + dialogue + "\"";
                }
            }
            else if (type == "dialogue_player_text") {
                const std::string speaker  = str("speaker");
                const std::string listener = str("listener");
                const std::string dialogue = str("dialogue");
                if (!listener.empty()) {
                    text = "(player) " + speaker + " -> " + listener + ": \"" + dialogue + "\"";
                } else {
                    text = "(player) " + speaker + ": \"" + dialogue + "\"";
                }
            }
            else if (type == "gamemaster_dialogue") {
                const std::string speaker  = str("speaker");
                const std::string target   = str("target");
                const std::string topic    = str("topic");
                const std::string dialogue = str("dialogue");
                text = speaker + " -> " + target + " (topic: " + topic + "): " + dialogue;
            }
            else if (type == "npc_thoughts") {
                text = str("npc_name") + " (thinking): \"" + str("thoughts") + "\"";
            }
            else if (type == "death") {
                text = str("killer") + " killed " + str("victim");
            }
            else if (type == "persistent_generic") {
                text = str("line");
            }
            else {
                // Unknown discriminator. Many third-party SkyrimNet plugins
                // (e.g. SeverActions' "follower_left") emit events whose
                // `data` is just a pre-rendered string sentence — perfect
                // for our purposes. Use it directly when that's the shape.
                // Fall back to a JSON dump of an object payload, and to a
                // last-resort "(no data)" when the field is missing.
                if (auto it = evt.find("data"); it != evt.end()) {
                    if (it->is_string()) {
                        text = it->get<std::string>();
                    } else if (it->is_object() || it->is_array()) {
                        text = it->dump();
                    } else {
                        text = "(no data)";
                    }
                } else {
                    text = "(no data)";
                }
            }

            if (evt.contains("gameTime") && evt["gameTime"].is_number()) {
                const double eventTime = evt["gameTime"].get<double>();
                const double delta = currentGameTimeSeconds - eventTime;
                text = "[" + FormatRelativeGameTime(delta < 0.0 ? 0.0 : delta) + "] " + text;
            }

            evt["text"] = std::move(text);
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

        // Unordered actor-pair key for "Hans vs Luke" grouping. Canonicalize
        // by ordering the two names so {Hans,Luke} == {Luke,Hans}.
        std::string PairKey(const std::string& a, const std::string& b)
        {
            return (a < b) ? (a + "\x1f" + b) : (b + "\x1f" + a);
        }

        struct ActorPairStats
        {
            std::string a, b;             // canonical (a <= b)
            int         aToB = 0;
            int         bToA = 0;
            int Total() const { return aToB + bToA; }
        };

        struct EnvBucket
        {
            std::string target;
            std::string sourceLabel;  // "" for bare fallback
            int         count = 0;
        };

        // Render a single condensed entry's body text from the pending hits.
        // Plan rules:
        //   - 1 actor-vs-actor hit         → "A strikes B"
        //   - 1 pair, one-sided (count>1)  → "A attacks B"
        //   - 1 pair, both sides           → "A and B trade blows"
        //   - multiple pairs               → "Combat continues: A attacks B; C attacks D"
        //                                     (top 3, ", and others" suffix if >3)
        //   - 1 env hit, with label        → "T took damage from <L>"
        //   - >1 env hit, same (T,L)       → "T took repeated damage from <L>"
        //   - 1 env hit, no label          → "T took damage"
        //   - >1 env hit, same T, no label → "T took repeated damage"
        //   - mixed                        → actor summary, then env summaries,
        //                                     joined with "; "
        std::string RenderCondensedBody(const std::vector<nlohmann::json>& hits)
        {
            std::vector<ActorPairStats>             pairs;
            std::vector<EnvBucket>                  envs;

            auto findPair = [&](const std::string& canA, const std::string& canB) -> ActorPairStats& {
                for (auto& p : pairs) {
                    if (p.a == canA && p.b == canB) return p;
                }
                pairs.push_back({canA, canB, 0, 0});
                return pairs.back();
            };
            auto findEnv = [&](const std::string& target, const std::string& label) -> EnvBucket& {
                for (auto& e : envs) {
                    if (e.target == target && e.sourceLabel == label) return e;
                }
                envs.push_back({target, label, 0});
                return envs.back();
            };

            for (const auto& h : hits) {
                const bool   namedActor = h.value("ne_actor_is_named", false);
                const std::string actor = h.value("originatingActorName", std::string{});
                const std::string targ  = h.value("targetActorName",      std::string{});
                if (namedActor && !actor.empty() && !targ.empty()) {
                    const std::string canA = (actor < targ) ? actor : targ;
                    const std::string canB = (actor < targ) ? targ  : actor;
                    auto& p = findPair(canA, canB);
                    if (actor == canA) p.aToB++; else p.bToA++;
                } else {
                    findEnv(targ, actor).count++;
                }
            }

            std::string actorSummary;
            if (!pairs.empty()) {
                if (pairs.size() == 1) {
                    auto& p = pairs.front();
                    const int total = p.Total();
                    if (total <= 1) {
                        // Single hit; render with attacker first.
                        const std::string& aggressor = (p.aToB > 0) ? p.a : p.b;
                        const std::string& victim    = (p.aToB > 0) ? p.b : p.a;
                        actorSummary = aggressor + " strikes " + victim;
                    } else if (p.aToB == 0 || p.bToA == 0) {
                        const std::string& aggressor = (p.aToB > 0) ? p.a : p.b;
                        const std::string& victim    = (p.aToB > 0) ? p.b : p.a;
                        actorSummary = aggressor + " attacks " + victim;
                    } else {
                        actorSummary = p.a + " and " + p.b + " trade blows";
                    }
                } else {
                    // Multiple distinct pairs — top 3 by total hits.
                    std::sort(pairs.begin(), pairs.end(),
                              [](const ActorPairStats& x, const ActorPairStats& y) {
                                  return x.Total() > y.Total();
                              });
                    actorSummary = "Combat continues: ";
                    const std::size_t cap = std::min<std::size_t>(3, pairs.size());
                    for (std::size_t i = 0; i < cap; ++i) {
                        if (i > 0) actorSummary += "; ";
                        const auto& p = pairs[i];
                        const std::string& aggressor = (p.aToB >= p.bToA) ? p.a : p.b;
                        const std::string& victim    = (p.aToB >= p.bToA) ? p.b : p.a;
                        actorSummary += aggressor + " attacks " + victim;
                    }
                    if (pairs.size() > cap) {
                        actorSummary += ", and others";
                    }
                }
            }

            std::vector<std::string> envSummaries;
            envSummaries.reserve(envs.size());
            for (const auto& e : envs) {
                std::string s;
                if (e.count <= 1) {
                    s = e.target + " took damage";
                    if (!e.sourceLabel.empty()) s += " from " + e.sourceLabel;
                } else {
                    s = e.target + " took repeated damage";
                    if (!e.sourceLabel.empty()) s += " from " + e.sourceLabel;
                }
                envSummaries.push_back(std::move(s));
            }

            std::string body;
            if (!actorSummary.empty()) body = actorSummary;
            for (auto& s : envSummaries) {
                if (!body.empty()) body += "; ";
                body += s;
            }
            return body;
        }

        // Build the final condensed JSON entry from the pending-hits buffer.
        // Inherits the timestamp + relative-prefix from the *most recent*
        // hit in the run.
        nlohmann::json BuildCondensedEntry(const std::vector<nlohmann::json>& hits,
                                           double                              currentGameTimeSeconds)
        {
            const auto& tail = hits.back();
            const double localTime = tail.value("localTime", 0.0);
            const double gameTime  = tail.value("gameTime",  0.0);
            const double delta     = currentGameTimeSeconds - gameTime;

            const std::string body = RenderCondensedBody(hits);
            const std::string text =
                "[" + FormatRelativeGameTime(delta < 0.0 ? 0.0 : delta) + "] " + body;

            nlohmann::json j;
            // Same shared `type` the individual combat events use, so the
            // dashboard / prompt see a single uniform label across the
            // whole combat family. `ne_kind` keeps the finer label.
            j["type"]      = "combat_event";
            j["ne_kind"]   = "combat_summary";
            j["localTime"] = localTime;
            j["gameTime"]  = gameTime;
            j["text"]      = text;
            return j;
        }
    }

    nlohmann::json BuildMergedTimeline(nlohmann::json skyrimNetEvents,
                                       nlohmann::json combatEvents,
                                       double         currentGameTimeSeconds)
    {
        // Game-time staleness: drop anything past this age, append an idle
        // marker once we go this long without anything fresh. Game time so
        // the player's `wait`/`sleep` actions advance the clock the same
        // way real-time elapsed activity would.
        constexpr double kMaxEventAgeGameSeconds         = 3600.0;  // 1 hour
        constexpr double kIdleMarkerThresholdGameSeconds = 600.0;   // 10 min

        // Concatenate, guarding against the inputs not being arrays.
        std::vector<nlohmann::json> merged;
        if (skyrimNetEvents.is_array()) {
            merged.reserve(skyrimNetEvents.size() + (combatEvents.is_array() ? combatEvents.size() : 0));
            for (auto& e : skyrimNetEvents) merged.push_back(std::move(e));
        }
        if (combatEvents.is_array()) {
            for (auto& e : combatEvents) merged.push_back(std::move(e));
        }

        // Age cap (game-time). Applied pre-condensation so straddling runs
        // of hits get summarized from only the still-fresh hits, not from
        // a mix that includes hour-old ones.
        merged.erase(std::remove_if(merged.begin(), merged.end(),
                                    [currentGameTimeSeconds](const nlohmann::json& e) {
                                        if (!e.is_object()) return true;
                                        const double gt = e.value("gameTime", 0.0);
                                        return (currentGameTimeSeconds - gt) > kMaxEventAgeGameSeconds;
                                    }),
                     merged.end());

        // Stable sort by localTime ascending so equal timestamps preserve
        // input order (SkyrimNet events tend to precede our internal events
        // when they share a tick boundary).
        std::stable_sort(merged.begin(), merged.end(),
                         [](const nlohmann::json& a, const nlohmann::json& b) {
                             const double at = a.is_object() ? a.value("localTime", 0.0) : 0.0;
                             const double bt = b.is_object() ? b.value("localTime", 0.0) : 0.0;
                             return at < bt;
                         });

        nlohmann::json              out = nlohmann::json::array();
        std::vector<nlohmann::json> pendingHits;

        const auto flushPending = [&]() {
            if (pendingHits.empty()) return;
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
        bool   needMarker   = false;
        double sinceSeconds = 0.0;
        if (out.empty()) {
            // Everything got filtered (or nothing was ever there). Use the
            // sentinel "over an hour" phrasing — we don't know the exact
            // gap because we just dropped the last event we could have
            // measured against.
            needMarker   = true;
            sinceSeconds = -1.0;
        } else {
            const auto&  last  = out.back();
            const double gt    = last.is_object() ? last.value("gameTime", 0.0) : 0.0;
            const double delta = currentGameTimeSeconds - gt;
            if (delta > kIdleMarkerThresholdGameSeconds) {
                needMarker   = true;
                sinceSeconds = delta;
            }
        }
        if (needMarker) {
            const std::string duration = (sinceSeconds < 0.0)
                ? std::string("over an hour")
                : FormatRelativeGameDuration(sinceSeconds);

            nlohmann::json marker;
            marker["type"]    = "idle_marker";
            marker["ne_kind"] = "idle_marker";
            // Game time = "now" so the marker sits naturally at the tail
            // by gameTime ordering. localTime uses the same Unix-epoch
            // basis as real events so any downstream sort puts it last.
            marker["gameTime"]  = currentGameTimeSeconds;
            marker["localTime"] = std::chrono::duration<double>(
                                      std::chrono::system_clock::now().time_since_epoch())
                                      .count();
            marker["text"] = "Nothing notable has happened for " + duration;
            out.push_back(std::move(marker));
        }

        return out;
    }
}
