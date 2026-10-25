#include <PlotItemPool.h>

#include <Localization.h>
#include <logger.h>
#include <Settings.h>

#include <fstream>
#include <sstream>

// The engine-bound half of the item pool: reading the file off disk and
// turning EditorIDs into forms. The parse itself is in PlotItemParse.cpp
// and is engine-free, because that is where every rule worth verifying
// lives.
namespace NarrativeEngine::PlotItemPool
{
    namespace
    {
        // Where a language file rewords an item's phrase, by its EditorID:
        // [Data.Item] TG04GoldenglowDeed = ...
        constexpr std::string_view kDataSection = "Data.Item";

        constexpr const char* kIniPath = "Data/SKSE/Plugins/NarrativeEngine/PlotItems.ini";

        std::vector<Item> g_items;
        bool g_loaded = false;
    } // namespace

    void Load()
    {
        if (g_loaded) {
            return;
        }
        g_loaded = true;
        g_items.clear();

        if (!Settings::Get().plotsEnabled) {
            return;
        }

        std::ifstream file(kIniPath, std::ios::binary);
        if (!file) {
            logger::warn("PlotItems: could not read {}. Acquire steps will name no object — they still run, "
                         "they are just vaguer.",
                         kIniPath);
            return;
        }
        std::ostringstream buffer;
        buffer << file.rdbuf();

        const auto report = ParseItems(buffer.str());
        for (const auto& line : report.warnings) {
            logger::warn("PlotItems: {}", line);
        }

        std::size_t unresolved = 0;
        std::size_t wrongType = 0;
        for (const auto& raw : report.items) {
            auto* form = RE::TESForm::LookupByEditorID(raw.editorId.c_str());
            if (form == nullptr) {
                // Costs this LINE, not the pool. A mod that removes one
                // object must not cost every other item in the file.
                logger::warn("PlotItems: '{}' did not resolve to any form; ignoring this line, the rest of the "
                             "pool is unaffected.",
                             raw.editorId);
                ++unresolved;
                continue;
            }

            // Type-checked, not merely resolved. An Acquire step has to
            // put its object into somebody's inventory, so a name that
            // resolves to a STAT or a CELL fails at runtime just as
            // surely as one that resolves to nothing -- and does so much
            // later, in a plot that already looked fine.
            auto* object = form->As<RE::TESBoundObject>();
            if (object == nullptr) {
                logger::warn("PlotItems: '{}' resolved to form type {}, which nobody can carry; ignoring this line.",
                             raw.editorId,
                             static_cast<int>(form->GetFormType()));
                ++wrongType;
                continue;
            }

            // A language file may reword it, by the line's EditorID.
            const auto* text = Localization::Get().DataText(kDataSection, raw.editorId);
            g_items.push_back({object->GetFormID(), text != nullptr ? *text : raw.displayName, raw.category});
        }

        {
            std::vector<std::string> ids;
            for (const auto& raw : report.items) {
                ids.push_back(raw.editorId);
            }

            // Overrides that reword nothing: a typo, or a row from a mod that is
            // not installed.
            for (const auto& line : Localization::Get().UnmatchedData(kDataSection, ids)) {
                logger::warn("PlotItems: the language file's {} matches no item line here", line);
            }
        }

        logger::info("PlotItems: loaded {} item(s); {} rejected by the parse, {} unresolved, {} not carryable.",
                     g_items.size(),
                     report.warnings.size(),
                     unresolved,
                     wrongType);
    }

    bool IsLoaded()
    {
        return g_loaded;
    }

    const std::vector<Item>& Items()
    {
        return g_items;
    }
} // namespace NarrativeEngine::PlotItemPool
