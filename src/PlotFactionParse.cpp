#include <PlotFactionRoster.h>

#include <SimpleIni.h>

#include <algorithm>
#include <cctype>
#include <stdexcept>

// The pure half of reading PlotFactions.ini: text in, structures and
// diagnostics out. No engine, no logging, no file system -- so every
// malformed-input rule can actually be exercised by a probe.
//
// EditorIDs come out of here as STRINGS. Turning them into forms is the
// engine-bound half's job, in PlotFactionRoster.cpp.
namespace NarrativeEngine::PlotFactionRoster
{
    namespace
    {
        constexpr const char* kSectionPrefix = "Faction:";

        std::string TrimAscii(std::string_view in)
        {
            const auto notSpace = [](unsigned char c) { return std::isspace(c) == 0; };
            auto begin = std::find_if(in.begin(), in.end(), notSpace);
            auto end = std::find_if(in.rbegin(), in.rend(), notSpace).base();
            return begin < end ? std::string(begin, end) : std::string{};
        }

        bool EqualsNoCase(std::string_view a, std::string_view b)
        {
            return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                       return std::tolower(static_cast<unsigned char>(x))
                              == std::tolower(static_cast<unsigned char>(y));
                   });
        }

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
                    auto trimmed = TrimAscii(v.pItem);
                    if (!trimmed.empty()) {
                        out.push_back(std::move(trimmed));
                    }
                }
            }
            return out;
        }

        std::string ValueFor(const CSimpleIniA& ini, const char* section, const char* key)
        {
            const auto all = ValuesFor(ini, section, key);
            return all.empty() ? std::string{} : all.front();
        }

        bool ParseInt(const std::string& text, int& out)
        {
            try {
                std::size_t consumed = 0;
                const int value = std::stoi(text, &consumed);
                if (consumed != text.size()) {
                    return false;
                }
                out = value;
                return true;
            } catch (const std::exception&) {
                return false;
            }
        }

        // Keys this build understands. Anything else is warned about and
        // ignored rather than treated as an error, so a file written for
        // a newer build still works on an older one.
        bool IsKnownKey(std::string_view key)
        {
            static constexpr std::string_view kKnown[] = {
                "Enabled", "Faction", "DisplayName", "RankMethod", "MaxRank", "MarkerFaction", "Member"};
            return std::any_of(
                std::begin(kKnown), std::end(kKnown), [key](std::string_view k) { return EqualsNoCase(k, key); });
        }
    } // namespace

    ParseReport ParseRoster(const std::string& iniText)
    {
        ParseReport report;

        CSimpleIniA ini;
        ini.SetUnicode();
        ini.SetMultiKey(true);
        if (ini.LoadData(iniText.c_str(), iniText.size()) < 0) {
            report.skipped.push_back("the file could not be parsed as INI at all");
            return report;
        }

        CSimpleIniA::TNamesDepend sections;
        ini.GetAllSections(sections);
        sections.sort(CSimpleIniA::Entry::LoadOrder());

        for (const auto& section : sections) {
            if (!section.pItem) {
                continue;
            }
            const std::string name{section.pItem};
            if (name.rfind(kSectionPrefix, 0) != 0) {
                report.skipped.push_back("[" + name + "]: sections must be named [Faction:<id>]");
                continue;
            }

            RawEntry entry;
            entry.id = name.substr(std::char_traits<char>::length(kSectionPrefix));

            const auto enabled = ValueFor(ini, section.pItem, "Enabled");
            if (!enabled.empty() && (EqualsNoCase(enabled, "false") || enabled == "0")) {
                report.warnings.push_back(entry.id + ": disabled");
                continue;
            }

            CSimpleIniA::TNamesDepend keys;
            ini.GetAllKeys(section.pItem, keys);
            for (const auto& k : keys) {
                if (k.pItem && !IsKnownKey(k.pItem)) {
                    report.warnings.push_back(entry.id + ": unknown key '" + k.pItem + "'; ignored");
                }
            }

            entry.factionEditorId = ValueFor(ini, section.pItem, "Faction");
            if (entry.factionEditorId.empty()) {
                report.skipped.push_back(entry.id + ": no Faction");
                continue;
            }

            entry.displayName = ValueFor(ini, section.pItem, "DisplayName");
            if (entry.displayName.empty()) {
                report.skipped.push_back(entry.id + ": no DisplayName");
                continue;
            }

            const auto method = ValueFor(ini, section.pItem, "RankMethod");
            if (method.empty()) {
                report.skipped.push_back(entry.id + ": no RankMethod");
                continue;
            }
            if (EqualsNoCase(method, "Rank")) {
                entry.method = RankMethod::Rank;
            } else if (EqualsNoCase(method, "Marker")) {
                entry.method = RankMethod::Marker;
            } else if (EqualsNoCase(method, "Explicit")) {
                entry.method = RankMethod::Explicit;
            } else {
                report.skipped.push_back(entry.id + ": RankMethod '" + method
                                         + "' is not one of Rank, Marker, Explicit");
                continue;
            }

            if (entry.method == RankMethod::Rank) {
                const auto maxRank = ValueFor(ini, section.pItem, "MaxRank");
                if (maxRank.empty()) {
                    report.skipped.push_back(entry.id + ": RankMethod = Rank needs MaxRank");
                    continue;
                }
                if (!ParseInt(maxRank, entry.maxRank) || entry.maxRank < 1) {
                    report.skipped.push_back(entry.id + ": MaxRank '" + maxRank + "' is not a positive whole number");
                    continue;
                }
            }

            if (entry.method == RankMethod::Marker) {
                entry.markerEditorIds = ValuesFor(ini, section.pItem, "MarkerFaction");
                if (entry.markerEditorIds.empty()) {
                    report.skipped.push_back(entry.id + ": RankMethod = Marker needs at least one MarkerFaction");
                    continue;
                }
            }

            // Overrides are valid under every method. A malformed line
            // costs that LINE only.
            for (const auto& raw : ValuesFor(ini, section.pItem, "Member")) {
                const auto comma = raw.find(',');
                if (comma == std::string::npos) {
                    report.warnings.push_back(entry.id + ": Member '" + raw
                                              + "' is not '<NpcEditorID>, <rank>'; line ignored");
                    continue;
                }
                RawOverride o;
                o.npcEditorId = TrimAscii(std::string_view(raw).substr(0, comma));
                const auto rankText = TrimAscii(std::string_view(raw).substr(comma + 1));
                if (o.npcEditorId.empty() || rankText.empty() || !ParseInt(rankText, o.rank)) {
                    report.warnings.push_back(entry.id + ": Member '" + raw
                                              + "' has a missing or non-numeric rank; line ignored");
                    continue;
                }
                entry.overrides.push_back(std::move(o));
            }

            if (entry.method == RankMethod::Explicit && entry.overrides.empty()) {
                report.skipped.push_back(entry.id
                                         + ": RankMethod = Explicit with no usable Member lines describes no "
                                           "hierarchy at all");
                continue;
            }

            report.entries.push_back(std::move(entry));
        }

        return report;
    }
} // namespace NarrativeEngine::PlotFactionRoster
