#include <SkyrimNetEvents.h>

#include <nlohmann/json.hpp>

#include <string>

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
                text = data ? data->dump() : std::string{"(no data)"};
            }

            if (evt.contains("gameTime") && evt["gameTime"].is_number()) {
                const double eventTime = evt["gameTime"].get<double>();
                const double delta = currentGameTimeSeconds - eventTime;
                text = "[" + FormatRelativeGameTime(delta < 0.0 ? 0.0 : delta) + "] " + text;
            }

            evt["text"] = std::move(text);
        }
    }
}
