#include <AmbushAttackerGroups.h>

#include <EngineUtils.h>
#include <logger.h>
#include <Region.h>
#include <Settings.h>

#include <SKSE/SKSE.h>

#include <SimpleIni.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace NarrativeEngine::AmbushAttackerGroups
{
    namespace
    {
        constexpr const char* kGroupFilePath = "Data/SKSE/Plugins/NarrativeEngine/AttackerGroups.ini";
        constexpr const char* kSectionPrefix = "Group:";

        // Populated by Load(). Pointers handed out by EligibleGroups /
        // Find point into this vector, so it is never mutated outside
        // Load() and Load() replaces it wholesale.
        std::vector<Group> g_groups;
        std::size_t g_enabledCount = 0;

        // Group id -> absolute game-hours when it was last used. Keyed
        // by id so it survives the user reordering or editing the file
        // between sessions. Guarded because the cosave hooks and the
        // eligibility path can run on different threads.
        std::mutex g_cooldownMutex;
        std::unordered_map<std::string, double> g_cooldownStamps;

        // --- small string helpers -------------------------------------------

        std::string_view TrimAscii(std::string_view s)
        {
            std::size_t start = 0;
            while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
                ++start;
            }
            std::size_t end = s.size();
            while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
                --end;
            }
            return s.substr(start, end - start);
        }

        std::string ToLowerCopy(std::string_view s)
        {
            std::string r;
            r.reserve(s.size());
            for (char c : s) {
                r += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return r;
        }

        bool ParseInt(std::string_view s, int& out)
        {
            s = TrimAscii(s);
            if (s.empty()) {
                return false;
            }
            const auto* first = s.data();
            const auto* last = s.data() + s.size();
            const auto res = std::from_chars(first, last, out);
            return res.ec == std::errc{} && res.ptr == last;
        }

        bool ParseFloat(std::string_view s, float& out)
        {
            s = TrimAscii(s);
            if (s.empty()) {
                return false;
            }
            // std::from_chars for floats is available on MSVC, but keep
            // this tolerant of the "1" / "1.0" / "-3" shapes a user is
            // likely to type by hand.
            const std::string tmp{s};
            char* end = nullptr;
            const double v = std::strtod(tmp.c_str(), &end);
            if (end == tmp.c_str() || *end != '\0') {
                return false;
            }
            out = static_cast<float>(v);
            return true;
        }

        // Accepts the spellings a user is likely to reach for. Anything
        // else is an error rather than a silent false — see the note on
        // Group::enabled.
        bool ParseBool(std::string_view s, bool& out)
        {
            const std::string v = ToLowerCopy(TrimAscii(s));
            if (v == "true" || v == "1" || v == "yes" || v == "on") {
                out = true;
                return true;
            }
            if (v == "false" || v == "0" || v == "no" || v == "off") {
                out = false;
                return true;
            }
            return false;
        }

        // snake_case: lowercase ASCII letters, digits, and underscores,
        // starting with a letter. Keeps ids safe to round-trip through
        // JSON prompt context and log lines without quoting surprises.
        bool IsSnakeCase(std::string_view s)
        {
            if (s.empty() || !(s.front() >= 'a' && s.front() <= 'z')) {
                return false;
            }
            for (char c : s) {
                const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
                if (!ok) {
                    return false;
                }
            }
            return true;
        }

        const char* OpName(CompareOp op)
        {
            switch (op) {
            case CompareOp::Equal:
                return "=";
            case CompareOp::NotEqual:
                return "!=";
            case CompareOp::Greater:
                return ">";
            case CompareOp::Less:
                return "<";
            case CompareOp::GreaterEqual:
                return ">=";
            case CompareOp::LessEqual:
                return "<=";
            }
            return "?";
        }

        template <typename T> bool ApplyOp(CompareOp op, T lhs, T rhs)
        {
            switch (op) {
            case CompareOp::Equal:
                return lhs == rhs;
            case CompareOp::NotEqual:
                return lhs != rhs;
            case CompareOp::Greater:
                return lhs > rhs;
            case CompareOp::Less:
                return lhs < rhs;
            case CompareOp::GreaterEqual:
                return lhs >= rhs;
            case CompareOp::LessEqual:
                return lhs <= rhs;
            }
            return false;
        }

        // Split `<name> <op> <value>` where whitespace around the
        // operator is optional. Longest-operator-first so ">=" isn't
        // mis-read as ">".
        bool SplitPredicate(std::string_view raw, std::string_view& name, CompareOp& op, std::string_view& value)
        {
            struct OpSpec
            {
                std::string_view token;
                CompareOp op;
            };
            // Order matters: two-character operators must be tested
            // before their one-character prefixes.
            static constexpr OpSpec kOps[] = {
                {">=", CompareOp::GreaterEqual},
                {"<=", CompareOp::LessEqual},
                {"!=", CompareOp::NotEqual},
                {"==", CompareOp::Equal},
                {"=", CompareOp::Equal},
                {">", CompareOp::Greater},
                {"<", CompareOp::Less},
            };

            for (const auto& spec : kOps) {
                const auto pos = raw.find(spec.token);
                if (pos == std::string_view::npos) {
                    continue;
                }
                name = TrimAscii(raw.substr(0, pos));
                value = TrimAscii(raw.substr(pos + spec.token.size()));
                op = spec.op;
                return !name.empty() && !value.empty();
            }
            return false;
        }

        // --- form resolution -------------------------------------------------

        // Resolve an EditorID and confirm it is the expected concrete
        // type. On a type mismatch the caller logs the form type it
        // actually got, which is what catches an NPC_ EditorID pasted
        // where an LVLN was meant.
        template <typename T> T* LookupTyped(std::string_view editorId, RE::FormType& actualTypeOut, bool& foundOut)
        {
            const std::string id{editorId};
            auto* form = RE::TESForm::LookupByEditorID(id.c_str());
            foundOut = form != nullptr;
            if (!form) {
                actualTypeOut = RE::FormType::None;
                return nullptr;
            }
            actualTypeOut = form->GetFormType();
            return form->As<T>();
        }

        RE::BGSKeyword* g_locTypeHoldKeyword = nullptr;
        bool g_locTypeHoldResolved = false;

        // The LocTypeHold check is what catches `WhiterunLocation`
        // written where `WhiterunHoldLocation` was meant. Without it
        // that group would simply never match, silently, forever.
        RE::BGSKeyword* LocTypeHoldKeyword()
        {
            if (!g_locTypeHoldResolved) {
                g_locTypeHoldResolved = true;
                auto* form = RE::TESForm::LookupByEditorID("LocTypeHold");
                g_locTypeHoldKeyword = form ? form->As<RE::BGSKeyword>() : nullptr;
                if (!g_locTypeHoldKeyword) {
                    logger::warn("AttackerGroups: 'LocTypeHold' did not resolve to a keyword; hold "
                                 "validation will accept any location.");
                }
            }
            return g_locTypeHoldKeyword;
        }

        // --- per-group parse -------------------------------------------------

        // Everything the parser knows how to read. Anything outside this
        // set is warned about and ignored, so a file authored against a
        // later build still loads here.
        bool IsKnownKey(const std::string& lowerKey)
        {
            static const std::unordered_set<std::string> kKnown = {
                "enabled",
                "cooldowngamehours",
                "displayname",
                "flavor",
                "lineform",
                "rangedform",
                "leaderform",
                "requirehold",
                "forbidhold",
                "requireplayerinfaction",
                "forbidplayerinfaction",
                "requireplayerkeyword",
                "forbidplayerkeyword",
                "requirequestcomplete",
                "forbidquestcomplete",
                "requirequeststage",
                "forbidqueststage",
                "requireglobal",
                "requiregamehour",
                "minplayerlevel",
                "maxplayerlevel",
            };
            return kKnown.count(lowerKey) != 0;
        }

        // Collect every value for `key` in `section`. SimpleIni is
        // configured multi-key, so a repeated key yields several values.
        std::vector<std::string> ValuesFor(const CSimpleIniA& ini, const char* section, const char* key)
        {
            std::vector<std::string> out;
            CSimpleIniA::TNamesDepend values;
            if (!ini.GetAllValues(section, key, values)) {
                return out;
            }
            values.sort(CSimpleIniA::Entry::LoadOrder());
            for (const auto& v : values) {
                if (v.pItem) {
                    const auto trimmed = TrimAscii(v.pItem);
                    if (!trimmed.empty()) {
                        out.emplace_back(trimmed);
                    }
                }
            }
            return out;
        }

        // One resolved leveled-character form, or a logged reason why
        // not. `what` names the key for the log line.
        bool ResolveLevCharacter(std::string_view editorId,
                                 const std::string& groupId,
                                 const char* what,
                                 RE::TESLevCharacter*& out)
        {
            RE::FormType actual = RE::FormType::None;
            bool found = false;
            auto* lvln = LookupTyped<RE::TESLevCharacter>(editorId, actual, found);
            if (!found) {
                logger::warn(
                    "AttackerGroups: skipping '{}': {} '{}' did not resolve to any form.", groupId, what, editorId);
                return false;
            }
            if (!lvln) {
                logger::warn("AttackerGroups: skipping '{}': {} '{}' resolved to form type {}, not a "
                             "leveled character (LVLN).",
                             groupId,
                             what,
                             editorId,
                             static_cast<int>(actual));
                return false;
            }
            out = lvln;
            return true;
        }

        // Parse one [Group:<id>] section. Returns false to skip the
        // group; every false path has already logged a specific reason
        // naming the section.
        bool ParseGroup(const CSimpleIniA& ini, const char* sectionName, const std::string& id, Group& out)
        {
            out.id = id;
            out.displayName = id;

            // --- Enabled -------------------------------------------------
            if (const auto vals = ValuesFor(ini, sectionName, "Enabled"); !vals.empty()) {
                bool enabled = true;
                if (!ParseBool(vals.back(), enabled)) {
                    logger::warn("AttackerGroups: skipping '{}': Enabled = '{}' is not a boolean "
                                 "(use true or false).",
                                 id,
                                 vals.back());
                    return false;
                }
                out.enabled = enabled;
            }

            // --- Cooldown ------------------------------------------------
            if (const auto vals = ValuesFor(ini, sectionName, "CooldownGameHours"); !vals.empty()) {
                int hours = 0;
                if (!ParseInt(vals.back(), hours) || hours < 0) {
                    logger::warn("AttackerGroups: skipping '{}': CooldownGameHours = '{}' is not a "
                                 "non-negative integer.",
                                 id,
                                 vals.back());
                    return false;
                }
                out.cooldownGameHours = hours;
            }

            // --- Display / flavor ---------------------------------------
            if (const auto vals = ValuesFor(ini, sectionName, "DisplayName"); !vals.empty()) {
                out.displayName = vals.back();
            }
            if (const auto vals = ValuesFor(ini, sectionName, "Flavor"); !vals.empty()) {
                out.flavor = vals.back();
            }

            // --- Roster --------------------------------------------------
            const auto lineForms = ValuesFor(ini, sectionName, "LineForm");
            if (lineForms.empty()) {
                logger::warn("AttackerGroups: skipping '{}': no LineForm. Every group needs at least "
                             "one rank-and-file leveled-character list.",
                             id);
                return false;
            }
            for (const auto& lf : lineForms) {
                RE::TESLevCharacter* form = nullptr;
                if (!ResolveLevCharacter(lf, id, "LineForm", form)) {
                    return false;
                }
                out.roster.lineForms.push_back(form);
            }
            if (const auto vals = ValuesFor(ini, sectionName, "RangedForm"); !vals.empty()) {
                if (!ResolveLevCharacter(vals.back(), id, "RangedForm", out.roster.rangedForm)) {
                    return false;
                }
            }
            if (const auto vals = ValuesFor(ini, sectionName, "LeaderForm"); !vals.empty()) {
                if (!ResolveLevCharacter(vals.back(), id, "LeaderForm", out.roster.leaderForm)) {
                    return false;
                }
            }

            // --- Holds ---------------------------------------------------
            auto parseHolds = [&](const char* key, std::vector<RE::FormID>& dst) -> bool {
                for (const auto& val : ValuesFor(ini, sectionName, key)) {
                    RE::FormType actual = RE::FormType::None;
                    bool found = false;
                    auto* loc = LookupTyped<RE::BGSLocation>(val, actual, found);
                    if (!found || !loc) {
                        logger::warn("AttackerGroups: skipping '{}': {} '{}' did not resolve to a "
                                     "location.",
                                     id,
                                     key,
                                     val);
                        return false;
                    }
                    if (auto* kw = LocTypeHoldKeyword(); kw && !loc->HasKeyword(kw)) {
                        logger::warn("AttackerGroups: skipping '{}': {} '{}' is a location but is not "
                                     "a hold (no LocTypeHold keyword). Did you mean the hold-level "
                                     "location, e.g. WhiterunHoldLocation rather than WhiterunLocation?",
                                     id,
                                     key,
                                     val);
                        return false;
                    }
                    dst.push_back(loc->GetFormID());
                }
                return true;
            };
            if (!parseHolds("RequireHold", out.eligibility.requireHolds)) {
                return false;
            }
            if (!parseHolds("ForbidHold", out.eligibility.forbidHolds)) {
                return false;
            }

            // --- Factions ------------------------------------------------
            auto parseFactions = [&](const char* key, std::vector<RE::TESFaction*>& dst) -> bool {
                for (const auto& val : ValuesFor(ini, sectionName, key)) {
                    RE::FormType actual = RE::FormType::None;
                    bool found = false;
                    auto* fac = LookupTyped<RE::TESFaction>(val, actual, found);
                    if (!found || !fac) {
                        logger::warn("AttackerGroups: skipping '{}': {} '{}' did not resolve to a "
                                     "faction.",
                                     id,
                                     key,
                                     val);
                        return false;
                    }
                    dst.push_back(fac);
                }
                return true;
            };
            if (!parseFactions("RequirePlayerInFaction", out.eligibility.requireFactions)) {
                return false;
            }
            if (!parseFactions("ForbidPlayerInFaction", out.eligibility.forbidFactions)) {
                return false;
            }

            // --- Keywords ------------------------------------------------
            auto parseKeywords = [&](const char* key, std::vector<RE::BGSKeyword*>& dst) -> bool {
                for (const auto& val : ValuesFor(ini, sectionName, key)) {
                    RE::FormType actual = RE::FormType::None;
                    bool found = false;
                    auto* kw = LookupTyped<RE::BGSKeyword>(val, actual, found);
                    if (!found || !kw) {
                        logger::warn("AttackerGroups: skipping '{}': {} '{}' did not resolve to a "
                                     "keyword.",
                                     id,
                                     key,
                                     val);
                        return false;
                    }
                    dst.push_back(kw);
                }
                return true;
            };
            if (!parseKeywords("RequirePlayerKeyword", out.eligibility.requireKeywords)) {
                return false;
            }
            if (!parseKeywords("ForbidPlayerKeyword", out.eligibility.forbidKeywords)) {
                return false;
            }

            // --- Quest completion ----------------------------------------
            auto parseQuests = [&](const char* key, std::vector<RE::TESQuest*>& dst) -> bool {
                for (const auto& val : ValuesFor(ini, sectionName, key)) {
                    RE::FormType actual = RE::FormType::None;
                    bool found = false;
                    auto* quest = LookupTyped<RE::TESQuest>(val, actual, found);
                    if (!found || !quest) {
                        logger::warn("AttackerGroups: skipping '{}': {} '{}' did not resolve to a "
                                     "quest.",
                                     id,
                                     key,
                                     val);
                        return false;
                    }
                    dst.push_back(quest);
                }
                return true;
            };
            if (!parseQuests("RequireQuestComplete", out.eligibility.requireQuestsComplete)) {
                return false;
            }
            if (!parseQuests("ForbidQuestComplete", out.eligibility.forbidQuestsComplete)) {
                return false;
            }

            // --- Quest stage predicates ----------------------------------
            auto parseQuestStages = [&](const char* key, std::vector<QuestStageConstraint>& dst) -> bool {
                for (const auto& val : ValuesFor(ini, sectionName, key)) {
                    std::string_view name;
                    std::string_view value;
                    CompareOp op = CompareOp::Equal;
                    if (!SplitPredicate(val, name, op, value)) {
                        logger::warn("AttackerGroups: skipping '{}': {} = '{}' is not of the form "
                                     "<QuestEditorID> <op> <stage>, where <op> is one of = != > < >= <=.",
                                     id,
                                     key,
                                     val);
                        return false;
                    }
                    int stage = 0;
                    if (!ParseInt(value, stage) || stage < 0 || stage > 0xFFFF) {
                        logger::warn("AttackerGroups: skipping '{}': {} = '{}' has a stage of '{}', "
                                     "which is not an integer in [0, 65535].",
                                     id,
                                     key,
                                     val,
                                     value);
                        return false;
                    }
                    RE::FormType actual = RE::FormType::None;
                    bool found = false;
                    auto* quest = LookupTyped<RE::TESQuest>(name, actual, found);
                    if (!found || !quest) {
                        logger::warn(
                            "AttackerGroups: skipping '{}': {} '{}' did not resolve to a quest.", id, key, name);
                        return false;
                    }
                    dst.push_back(QuestStageConstraint{quest, op, static_cast<std::uint16_t>(stage)});
                }
                return true;
            };
            if (!parseQuestStages("RequireQuestStage", out.eligibility.requireQuestStages)) {
                return false;
            }
            if (!parseQuestStages("ForbidQuestStage", out.eligibility.forbidQuestStages)) {
                return false;
            }

            // --- Globals -------------------------------------------------
            for (const auto& val : ValuesFor(ini, sectionName, "RequireGlobal")) {
                std::string_view name;
                std::string_view value;
                CompareOp op = CompareOp::Equal;
                if (!SplitPredicate(val, name, op, value)) {
                    logger::warn("AttackerGroups: skipping '{}': RequireGlobal = '{}' is not of the "
                                 "form <GlobalEditorID> <op> <value>, where <op> is one of = != > < >= <=.",
                                 id,
                                 val);
                    return false;
                }
                float num = 0.0f;
                if (!ParseFloat(value, num)) {
                    logger::warn("AttackerGroups: skipping '{}': RequireGlobal = '{}' has a value of "
                                 "'{}', which is not a number.",
                                 id,
                                 val,
                                 value);
                    return false;
                }
                RE::FormType actual = RE::FormType::None;
                bool found = false;
                auto* global = LookupTyped<RE::TESGlobal>(name, actual, found);
                if (!found || !global) {
                    logger::warn("AttackerGroups: skipping '{}': RequireGlobal '{}' did not resolve to "
                                 "a global variable.",
                                 id,
                                 name);
                    return false;
                }
                out.eligibility.requireGlobals.push_back(GlobalConstraint{global, op, num});
            }

            // --- Hour windows --------------------------------------------
            for (const auto& val : ValuesFor(ini, sectionName, "RequireGameHour")) {
                const auto dash = val.find('-', 1);
                if (dash == std::string::npos) {
                    logger::warn("AttackerGroups: skipping '{}': RequireGameHour = '{}' is not of the "
                                 "form <start>-<end> (24-hour game time, wraps across midnight).",
                                 id,
                                 val);
                    return false;
                }
                float start = 0.0f;
                float end = 0.0f;
                if (!ParseFloat(std::string_view{val}.substr(0, dash), start)
                    || !ParseFloat(std::string_view{val}.substr(dash + 1), end)) {
                    logger::warn("AttackerGroups: skipping '{}': RequireGameHour = '{}' has a bound "
                                 "that is not a number.",
                                 id,
                                 val);
                    return false;
                }
                if (start < 0.0f || start >= 24.0f || end < 0.0f || end >= 24.0f) {
                    logger::warn("AttackerGroups: skipping '{}': RequireGameHour = '{}' has a bound "
                                 "outside [0, 24).",
                                 id,
                                 val);
                    return false;
                }
                if (start == end) {
                    // Refuse to guess: this could mean "never" or
                    // "always" and the intent is genuinely ambiguous.
                    logger::warn("AttackerGroups: skipping '{}': RequireGameHour = '{}' has equal "
                                 "bounds, which is ambiguous (a zero-width window or an all-day one?). "
                                 "Omit the key for all day.",
                                 id,
                                 val);
                    return false;
                }
                out.eligibility.requireHours.push_back(HourWindow{start, end});
            }

            // --- Level bounds --------------------------------------------
            auto parseLevel = [&](const char* key, std::optional<int>& dst) -> bool {
                const auto vals = ValuesFor(ini, sectionName, key);
                if (vals.empty()) {
                    return true;
                }
                int level = 0;
                if (!ParseInt(vals.back(), level) || level < 0) {
                    logger::warn("AttackerGroups: skipping '{}': {} = '{}' is not a non-negative integer.",
                                 id,
                                 key,
                                 vals.back());
                    return false;
                }
                dst = level;
                return true;
            };
            if (!parseLevel("MinPlayerLevel", out.eligibility.minPlayerLevel)) {
                return false;
            }
            if (!parseLevel("MaxPlayerLevel", out.eligibility.maxPlayerLevel)) {
                return false;
            }
            if (out.eligibility.minPlayerLevel && out.eligibility.maxPlayerLevel
                && *out.eligibility.minPlayerLevel > *out.eligibility.maxPlayerLevel) {
                logger::warn("AttackerGroups: skipping '{}': MinPlayerLevel ({}) is greater than "
                             "MaxPlayerLevel ({}).",
                             id,
                             *out.eligibility.minPlayerLevel,
                             *out.eligibility.maxPlayerLevel);
                return false;
            }

            // --- Unknown keys --------------------------------------------
            // Warn, don't fail: a group file written for a later build
            // must still load here.
            CSimpleIniA::TNamesDepend keys;
            if (ini.GetAllKeys(sectionName, keys)) {
                for (const auto& k : keys) {
                    if (k.pItem && !IsKnownKey(ToLowerCopy(k.pItem))) {
                        logger::warn("AttackerGroups: '{}' has unknown key '{}' — ignoring it. (Not an "
                                     "error: the group still loads.)",
                                     id,
                                     k.pItem);
                    }
                }
            }

            return true;
        }

        // --- eligibility evaluation -----------------------------------------

        // True when `group` was used recently enough to still be sitting
        // out. Takes the cooldown mutex itself; the name carries the
        // "Locked" suffix only to flag that callers must not already
        // hold it.
        bool IsGroupOnCooldown(const Group& group, double nowGameHours)
        {
            if (group.cooldownGameHours <= 0) {
                return false;
            }
            std::scoped_lock lock(g_cooldownMutex);
            const auto it = g_cooldownStamps.find(group.id);
            if (it == g_cooldownStamps.end()) {
                return false;
            }
            return (nowGameHours - it->second) < static_cast<double>(group.cooldownGameHours);
        }

        bool EvaluateEligibility(const Group& group, const EligibilityContext& ctx)
        {
            const auto& e = group.eligibility;

            // Holds. A zero holdFormID means no hold resolved at all
            // (unclassified or mod-added worldspace); every hold-gated
            // group correctly becomes ineligible and the unconstrained
            // fallback carries the tick.
            if (!e.requireHolds.empty()) {
                if (std::find(e.requireHolds.begin(), e.requireHolds.end(), ctx.holdFormID) == e.requireHolds.end()) {
                    return false;
                }
            }
            if (std::find(e.forbidHolds.begin(), e.forbidHolds.end(), ctx.holdFormID) != e.forbidHolds.end()) {
                return false;
            }

            // Everything below reads the player.
            if (!ctx.player) {
                return e.requireFactions.empty() && e.forbidFactions.empty() && e.requireKeywords.empty()
                       && e.forbidKeywords.empty() && !e.minPlayerLevel && !e.maxPlayerLevel;
            }

            if (!e.requireFactions.empty()) {
                const bool any = std::any_of(e.requireFactions.begin(), e.requireFactions.end(), [&](auto* f) {
                    return f && ctx.player->IsInFaction(f);
                });
                if (!any) {
                    return false;
                }
            }
            for (auto* f : e.forbidFactions) {
                if (f && ctx.player->IsInFaction(f)) {
                    return false;
                }
            }

            if (!e.requireKeywords.empty()) {
                const bool any = std::any_of(e.requireKeywords.begin(), e.requireKeywords.end(), [&](auto* k) {
                    return k && ctx.player->HasKeyword(k);
                });
                if (!any) {
                    return false;
                }
            }
            for (auto* k : e.forbidKeywords) {
                if (k && ctx.player->HasKeyword(k)) {
                    return false;
                }
            }

            if (!e.requireQuestsComplete.empty()) {
                const bool any = std::any_of(e.requireQuestsComplete.begin(),
                                             e.requireQuestsComplete.end(),
                                             [](auto* q) { return q && q->IsCompleted(); });
                if (!any) {
                    return false;
                }
            }
            for (auto* q : e.forbidQuestsComplete) {
                if (q && q->IsCompleted()) {
                    return false;
                }
            }

            if (!e.requireQuestStages.empty()) {
                const bool any =
                    std::any_of(e.requireQuestStages.begin(), e.requireQuestStages.end(), [](const auto& c) {
                        return c.quest && ApplyOp<int>(c.op, c.quest->GetCurrentStageID(), c.stage);
                    });
                if (!any) {
                    return false;
                }
            }
            for (const auto& c : e.forbidQuestStages) {
                if (c.quest && ApplyOp<int>(c.op, c.quest->GetCurrentStageID(), c.stage)) {
                    return false;
                }
            }

            // Globals AND rather than OR — each occurrence names a
            // different variable, so repeats are additional
            // requirements, not alternatives.
            for (const auto& c : e.requireGlobals) {
                if (!c.global || !ApplyOp<float>(c.op, c.global->value, c.value)) {
                    return false;
                }
            }

            if (!e.requireHours.empty()) {
                const bool any = std::any_of(e.requireHours.begin(), e.requireHours.end(), [&](const auto& w) {
                    return w.Contains(ctx.gameHour);
                });
                if (!any) {
                    return false;
                }
            }

            if (e.minPlayerLevel && static_cast<int>(ctx.playerLevel) < *e.minPlayerLevel) {
                return false;
            }
            if (e.maxPlayerLevel && static_cast<int>(ctx.playerLevel) > *e.maxPlayerLevel) {
                return false;
            }

            return true;
        }
    } // namespace

    bool HourWindow::Contains(float hour) const
    {
        // Non-wrapping window: [start, end).
        if (start < end) {
            return hour >= start && hour < end;
        }
        // Wrapping window (e.g. 20-6): everything at or after `start`,
        // plus everything before `end`. This is the case that matters —
        // getting it wrong silently inverts a nocturnal group.
        return hour >= start || hour < end;
    }

    bool Eligibility::IsUnconstrained() const
    {
        return requireHolds.empty() && forbidHolds.empty() && requireFactions.empty() && forbidFactions.empty()
               && requireKeywords.empty() && forbidKeywords.empty() && requireQuestsComplete.empty()
               && forbidQuestsComplete.empty() && requireQuestStages.empty() && forbidQuestStages.empty()
               && requireGlobals.empty() && requireHours.empty() && !minPlayerLevel && !maxPlayerLevel;
    }

    void Load()
    {
        g_groups.clear();
        g_enabledCount = 0;

        std::error_code ec;
        if (!std::filesystem::exists(kGroupFilePath, ec)) {
            // Loud on purpose. A silently disabled beat is worse than a
            // noisy one — without this file the ambush beat can never
            // fire, and the user has no other way to see why.
            logger::error("AttackerGroups: '{}' is MISSING. The ambush beat has no attackers to draw "
                          "from and will stay unavailable. Reinstall the mod or restore the file.",
                          kGroupFilePath);
            return;
        }

        CSimpleIniA ini;
        ini.SetUnicode();
        // Repeated keys are load-bearing here: RequireHold, LineForm and
        // friends all rely on a key appearing more than once.
        ini.SetMultiKey(true);
        if (const auto rc = ini.LoadFile(kGroupFilePath); rc < 0) {
            logger::error("AttackerGroups: failed to parse '{}' (SimpleIni error {}). No groups loaded.",
                          kGroupFilePath,
                          static_cast<int>(rc));
            return;
        }

        CSimpleIniA::TNamesDepend sections;
        ini.GetAllSections(sections);
        sections.sort(CSimpleIniA::Entry::LoadOrder());

        std::size_t skipped = 0;
        std::size_t disabled = 0;
        std::unordered_set<std::string> seenIds;

        for (const auto& section : sections) {
            if (!section.pItem) {
                continue;
            }
            const std::string_view name{section.pItem};
            if (name.rfind(kSectionPrefix, 0) != 0) {
                logger::warn("AttackerGroups: ignoring section '[{}]' — group sections must be named "
                             "[Group:<id>].",
                             name);
                continue;
            }

            const std::string id{TrimAscii(name.substr(std::string_view{kSectionPrefix}.size()))};
            if (!IsSnakeCase(id)) {
                logger::warn("AttackerGroups: skipping '[{}]': id '{}' must be snake_case (lowercase "
                             "letters, digits and underscores, starting with a letter).",
                             name,
                             id);
                ++skipped;
                continue;
            }
            if (!seenIds.insert(id).second) {
                logger::warn("AttackerGroups: skipping '[{}]': duplicate group id '{}'.", name, id);
                ++skipped;
                continue;
            }

            Group group;
            if (!ParseGroup(ini, section.pItem, id, group)) {
                ++skipped;
                continue;
            }
            if (!group.enabled) {
                ++disabled;
            }
            g_groups.push_back(std::move(group));
        }

        g_enabledCount = static_cast<std::size_t>(
            std::count_if(g_groups.begin(), g_groups.end(), [](const Group& g) { return g.enabled; }));

        const std::size_t total = g_groups.size() + skipped;
        logger::info("AttackerGroups: loaded {} of {} groups, {} disabled, {} skipped.",
                     g_groups.size(),
                     total,
                     disabled,
                     skipped);

        // An eligible set that can go empty is invisible from inside the
        // game — the beat just never fires in some places and the user
        // has no way to tell that from "the Director didn't pick it".
        // Say so at load time instead.
        const bool anyUnconstrained = std::any_of(g_groups.begin(), g_groups.end(), [](const Group& g) {
            return g.enabled && g.eligibility.IsUnconstrained();
        });
        if (!anyUnconstrained && g_enabledCount > 0) {
            logger::warn("AttackerGroups: no ENABLED group is unconstrained. Every group has at least "
                         "one eligibility requirement, so the ambush beat will be unable to fire "
                         "wherever none of them are satisfied. Consider enabling 'bandits' or adding a "
                         "group with no Require*/Forbid* keys.");
        }
        if (g_enabledCount == 0) {
            logger::warn("AttackerGroups: no enabled groups. The ambush beat will stay unavailable.");
        }
    }

    EligibilityContext CaptureContext()
    {
        EligibilityContext ctx;
        ctx.player = RE::PlayerCharacter::GetSingleton();
        ctx.holdFormID = Region::ForPlayer().holdFormID;
        if (auto* calendar = RE::Calendar::GetSingleton()) {
            ctx.gameHour = calendar->GetHour();
        }
        ctx.nowGameHours = EngineUtils::GetCurrentGameHours();
        if (ctx.player) {
            ctx.playerLevel = ctx.player->GetLevel();
        }
        return ctx;
    }

    std::vector<const Group*> EligibleGroups(const EligibilityContext& ctx)
    {
        std::vector<const Group*> out;
        out.reserve(g_groups.size());
        int onCooldown = 0;
        for (const auto& g : g_groups) {
            if (!g.enabled || !EvaluateEligibility(g, ctx)) {
                continue;
            }
            // Cooldown last: a group excluded for being recently used is
            // worth counting separately from one that was never eligible
            // here in the first place.
            if (IsGroupOnCooldown(g, ctx.nowGameHours)) {
                ++onCooldown;
                continue;
            }
            out.push_back(&g);
        }

        if (Settings::Get().debugMode) {
            std::string ids;
            for (const auto* g : out) {
                if (!ids.empty()) {
                    ids += ", ";
                }
                ids += g->id;
            }
            logger::debug("AttackerGroups: {} eligible [{}] ({} on cooldown) (hold=0x{:08X}, hour={:.2f}, "
                          "level={})",
                          out.size(),
                          ids,
                          onCooldown,
                          ctx.holdFormID,
                          ctx.gameHour,
                          ctx.playerLevel);
        }

        return out;
    }

    EligibilityContext CaptureContext(const MainThread::Token&)
    {
        return CaptureContext();
    }

    std::vector<const Group*> EligibleGroups(const MainThread::Token&, const EligibilityContext& ctx)
    {
        return EligibleGroups(ctx);
    }

    std::vector<GroupSummary> EligibleGroupSummaries()
    {
        std::vector<GroupSummary> out;
        const auto ctx = CaptureContext();
        for (const auto* g : EligibleGroups(ctx)) {
            out.push_back(GroupSummary{g->id, g->displayName, g->flavor});
        }
        return out;
    }

    std::vector<GroupSummary> EligibleGroupSummaries(const MainThread::Token&)
    {
        return EligibleGroupSummaries();
    }

    const Group* Find(std::string_view id)
    {
        for (const auto& g : g_groups) {
            // Disabled groups are invisible, not merely deprioritised —
            // a stale id from a prompt built before the user flipped the
            // switch must not resolve.
            if (g.enabled && g.id == id) {
                return &g;
            }
        }
        return nullptr;
    }

    std::vector<RE::TESLevCharacter*> ComposeRoster(const Group& group, int count)
    {
        std::vector<RE::TESLevCharacter*> out;
        if (count <= 0 || group.roster.lineForms.empty()) {
            return out;
        }
        out.reserve(static_cast<std::size_t>(count));

        int remaining = count;

        // Leader: one, and only when the group is big enough for a
        // leader to read as one rather than as "the only attacker".
        if (group.roster.leaderForm && count >= 3) {
            out.push_back(group.roster.leaderForm);
            --remaining;
        }

        // Ranged: roughly one per three attackers, sized off the
        // requested count rather than the remainder so the ratio holds
        // whether or not a leader was added.
        if (group.roster.rangedForm) {
            const int rangedCount = std::min(remaining, count / 3);
            for (int i = 0; i < rangedCount; ++i) {
                out.push_back(group.roster.rangedForm);
            }
            remaining -= rangedCount;
        }

        // Balance: round-robin across the line forms so a group that
        // lists several rank-and-file lists mixes them evenly.
        for (int i = 0; i < remaining; ++i) {
            out.push_back(group.roster.lineForms[static_cast<std::size_t>(i) % group.roster.lineForms.size()]);
        }

        return out;
    }

    std::size_t EnabledGroupCount()
    {
        return g_enabledCount;
    }

    std::size_t TotalGroupCount()
    {
        return g_groups.size();
    }

    void StampGroupUsed(std::string_view id)
    {
        if (id.empty()) {
            return;
        }
        const double now = EngineUtils::GetCurrentGameHours();
        {
            std::scoped_lock lock(g_cooldownMutex);
            g_cooldownStamps[std::string{id}] = now;
        }
        logger::info("AttackerGroups: '{}' stamped used at {:.2f} game hours", std::string{id}, now);
    }

    double RemainingCooldownGameHours(std::string_view id)
    {
        const Group* group = nullptr;
        for (const auto& g : g_groups) {
            if (g.id == id) {
                group = &g;
                break;
            }
        }
        if (!group || group->cooldownGameHours <= 0) {
            return 0.0;
        }

        double stamp = 0.0;
        {
            std::scoped_lock lock(g_cooldownMutex);
            const auto it = g_cooldownStamps.find(std::string{id});
            if (it == g_cooldownStamps.end()) {
                return 0.0;
            }
            stamp = it->second;
        }
        const double remaining =
            static_cast<double>(group->cooldownGameHours) - (EngineUtils::GetCurrentGameHours() - stamp);
        return remaining > 0.0 ? remaining : 0.0;
    }

    void SerializeCooldowns(SKSE::SerializationInterface* intfc)
    {
        if (!intfc) {
            return;
        }
        std::scoped_lock lock(g_cooldownMutex);
        const auto count = static_cast<std::uint32_t>(g_cooldownStamps.size());
        intfc->WriteRecordData(count);
        for (const auto& [id, stamp] : g_cooldownStamps) {
            const auto len = static_cast<std::uint32_t>(id.size());
            intfc->WriteRecordData(len);
            if (len > 0) {
                intfc->WriteRecordData(id.data(), len);
            }
            intfc->WriteRecordData(stamp);
        }
    }

    bool DeserializeCooldowns(SKSE::SerializationInterface* intfc)
    {
        if (!intfc) {
            return false;
        }
        std::uint32_t count = 0;
        if (intfc->ReadRecordData(count) != sizeof(count)) {
            return false;
        }
        // Sanity bound: the group file would have to be absurd to
        // exceed this, and a corrupt length shouldn't allocate wildly.
        if (count > 4096) {
            return false;
        }

        std::unordered_map<std::string, double> loaded;
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint32_t len = 0;
            if (intfc->ReadRecordData(len) != sizeof(len) || len > 1024) {
                return false;
            }
            std::string id;
            if (len > 0) {
                id.resize(len);
                if (intfc->ReadRecordData(id.data(), len) != len) {
                    return false;
                }
            }
            double stamp = 0.0;
            if (intfc->ReadRecordData(stamp) != sizeof(stamp)) {
                return false;
            }
            if (!id.empty()) {
                loaded.emplace(std::move(id), stamp);
            }
        }

        {
            std::scoped_lock lock(g_cooldownMutex);
            g_cooldownStamps = std::move(loaded);
        }
        return true;
    }

    void ClearCooldowns()
    {
        std::scoped_lock lock(g_cooldownMutex);
        g_cooldownStamps.clear();
    }
} // namespace NarrativeEngine::AmbushAttackerGroups
