#include <AlphaCanon.h>

#include <logger.h>
#include <Settings.h>

#include <algorithm>
#include <cctype>
#include <string_view>

namespace NarrativeEngine::AlphaCanon
{
    namespace
    {
        // Trim leading/trailing ASCII whitespace from a string_view.
        std::string_view Trim(std::string_view sv)
        {
            const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
            const auto first = std::find_if(sv.begin(), sv.end(), notSpace);
            if (first == sv.end()) {
                return {};
            }
            const auto last = std::find_if(sv.rbegin(), sv.rend(), notSpace).base();
            return sv.substr(static_cast<std::size_t>(first - sv.begin()), static_cast<std::size_t>(last - first));
        }

        // True if `needle` appears as one of the comma-separated tokens in
        // `csv`. Case-sensitive (EditorIDs are), whitespace-trimmed.
        bool CsvContains(std::string_view csv, std::string_view needle)
        {
            if (csv.empty() || needle.empty()) {
                return false;
            }
            std::size_t pos = 0;
            while (pos <= csv.size()) {
                const auto comma = csv.find(',', pos);
                const auto end = (comma == std::string_view::npos) ? csv.size() : comma;
                if (Trim(csv.substr(pos, end - pos)) == needle) {
                    return true;
                }
                if (comma == std::string_view::npos) {
                    break;
                }
                pos = comma + 1;
            }
            return false;
        }

        // Upper bound on parentLoc traversal depth. Vanilla never nests
        // more than two or three deep; the cap is defensive against a
        // cycle introduced by malformed mod data.
        constexpr int kMaxParentDepth = 16;
    } // namespace

    bool IsInActiveCombat()
    {
        auto* pc = RE::PlayerCharacter::GetSingleton();
        return pc && pc->IsInCombat();
    }

    bool IsInDialogue()
    {
        auto* ui = RE::UI::GetSingleton();
        return ui && ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME);
    }

    bool IsInScriptedScene()
    {
        auto* pc = RE::PlayerCharacter::GetSingleton();
        if (!pc) {
            return false;
        }
        auto* scene = pc->GetCurrentScene();
        return scene && scene->isPlaying;
    }

    bool IsInDoNotDisturbCell()
    {
        const auto& csv = Settings::Get().doNotDisturbCellEDIDsCSV;
        if (csv.empty()) {
            return false;
        }

        auto* pc = RE::PlayerCharacter::GetSingleton();
        if (!pc) {
            return false;
        }
        auto* cell = pc->GetParentCell();
        if (!cell) {
            return false;
        }

        // Cell EditorIDs are only retained at runtime when an EditorID-
        // recovery mod (e.g. powerofthree's Tweaks) is installed. Without
        // one, GetFormEditorID() returns "" for cells and this predicate is
        // effectively always false — the do-not-disturb feature is opt-in
        // and assumes that infrastructure is in place.
        const char* edid = cell->GetFormEditorID();
        if (!edid || *edid == '\0') {
            return false;
        }
        return CsvContains(csv, edid);
    }

    bool IsInBlacklistedLocation()
    {
        const auto& csv = Settings::Get().blacklistedLocationEDIDsCSV;
        if (csv.empty()) {
            return false;
        }

        auto* pc = RE::PlayerCharacter::GetSingleton();
        if (!pc) {
            return false;
        }

        // Walk the player's Location and its parentLoc ancestors so a
        // blacklisted parent covers every Location beneath it — e.g.
        // SovngardeHallofHeroesLocation's ParentLocation is
        // SovngardeLocation, so blacklisting the latter blocks the
        // former without naming it.
        auto* loc = pc->GetCurrentLocation();
        for (int depth = 0; loc && depth < kMaxParentDepth; ++depth) {
            // Location EditorIDs need runtime retention (powerofthree's
            // Tweaks or equivalent); without it this is "" for every
            // Location and the blacklist degrades open.
            if (const char* edid = loc->GetFormEditorID(); edid && *edid && CsvContains(csv, edid)) {
                logger::debug("AlphaCanon: player location blacklisted via '{}'", edid);
                return true;
            }
            loc = loc->parentLoc;
        }
        return false;
    }

    Signal EvaluateAll()
    {
        Signal mask = Signal::None;
        if (IsInActiveCombat())
            mask |= Signal::InActiveCombat;
        if (IsInScriptedScene())
            mask |= Signal::InScriptedScene;
        if (IsInDialogue())
            mask |= Signal::InDialogue;
        if (IsInDoNotDisturbCell())
            mask |= Signal::InDoNotDisturbCell;
        if (IsInBlacklistedLocation())
            mask |= Signal::InBlacklistedLocation;
        return mask;
    }

    std::vector<std::string> Names(Signal mask)
    {
        std::vector<std::string> names;
        if (HasFlag(mask, Signal::InActiveCombat))
            names.emplace_back("InActiveCombat");
        if (HasFlag(mask, Signal::InScriptedScene))
            names.emplace_back("InScriptedScene");
        if (HasFlag(mask, Signal::InDialogue))
            names.emplace_back("InDialogue");
        if (HasFlag(mask, Signal::InDoNotDisturbCell))
            names.emplace_back("InDoNotDisturbCell");
        if (HasFlag(mask, Signal::InBlacklistedLocation))
            names.emplace_back("InBlacklistedLocation");
        return names;
    }
} // namespace NarrativeEngine::AlphaCanon
