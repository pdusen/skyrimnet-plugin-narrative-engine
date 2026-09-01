#include <PlotItemPool.h>

#include <SimpleIni.h>

#include <algorithm>
#include <cctype>

// The pure half of reading PlotItems.ini: text in, structures and
// diagnostics out. No engine, no logging, no file system -- so every
// malformed-input rule can actually be exercised by a probe.
//
// EditorIDs come out of here as STRINGS. Turning them into forms is the
// engine-bound half's job, in PlotItemPool.cpp.
namespace NarrativeEngine::PlotItemPool
{
    namespace
    {
        constexpr const char* kSection = "Items";

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
    } // namespace

    std::string_view CategoryId(Category c) noexcept
    {
        switch (c) {
        case Category::Valuable:
            return "Valuable";
        case Category::Document:
            return "Document";
        case Category::Contraband:
            return "Contraband";
        case Category::Drink:
            return "Drink";
        case Category::Count:
            break;
        }
        return "unknown";
    }

    bool ParseCategory(std::string_view id, Category& out) noexcept
    {
        for (std::uint8_t i = 0; i < static_cast<std::uint8_t>(Category::Count); ++i) {
            const auto candidate = static_cast<Category>(i);
            if (EqualsNoCase(CategoryId(candidate), id)) {
                out = candidate;
                return true;
            }
        }
        return false;
    }

    ParseReport ParseItems(const std::string& iniText)
    {
        ParseReport report;

        CSimpleIniA ini;
        ini.SetUnicode();
        ini.SetMultiKey(true);
        if (ini.LoadData(iniText.c_str(), iniText.size()) < 0) {
            report.warnings.emplace_back("the file could not be parsed as INI at all");
            return report;
        }

        CSimpleIniA::TNamesDepend keys;
        if (!ini.GetAllKeys(kSection, keys)) {
            report.warnings.emplace_back("no [Items] section, so the pool is empty");
            return report;
        }
        keys.sort(CSimpleIniA::Entry::LoadOrder());

        // Duplicates are rejected rather than merged: two lines naming the
        // same object would put it on the menu twice, which biases the
        // model's pick toward it for no reason the author intended.
        std::vector<std::string> seen;

        for (const auto& key : keys) {
            if (key.pItem == nullptr) {
                continue;
            }
            Category category = Category::Valuable;
            const bool known = ParseCategory(key.pItem, category);

            CSimpleIniA::TNamesDepend values;
            if (!ini.GetAllValues(kSection, key.pItem, values)) {
                continue;
            }
            values.sort(CSimpleIniA::Entry::LoadOrder());

            for (const auto& value : values) {
                if (value.pItem == nullptr) {
                    continue;
                }
                const std::string raw{value.pItem};
                if (!known) {
                    // Warned about and ignored rather than treated as an
                    // error, so a file written for a newer build with a
                    // category this one has never heard of still loads
                    // everything else.
                    report.warnings.push_back(std::string("unknown category '") + key.pItem + "' on '" + raw
                                              + "'; line ignored");
                    continue;
                }

                const auto comma = raw.find(',');
                if (comma == std::string::npos) {
                    report.warnings.push_back("'" + raw + "' is not '<EditorID>, <display name>'; line ignored");
                    continue;
                }
                RawItem item;
                item.category = category;
                item.editorId = TrimAscii(std::string_view(raw).substr(0, comma));
                item.displayName = TrimAscii(std::string_view(raw).substr(comma + 1));
                if (item.editorId.empty() || item.displayName.empty()) {
                    report.warnings.push_back("'" + raw
                                              + "' is missing its EditorID or its display name; "
                                                "line ignored");
                    continue;
                }
                if (std::any_of(seen.begin(), seen.end(), [&item](const std::string& s) {
                        return EqualsNoCase(s, item.editorId);
                    })) {
                    report.warnings.push_back("'" + item.editorId
                                              + "' is listed more than once; "
                                                "the later line is ignored");
                    continue;
                }
                seen.push_back(item.editorId);
                report.items.push_back(std::move(item));
            }
        }

        return report;
    }
} // namespace NarrativeEngine::PlotItemPool
