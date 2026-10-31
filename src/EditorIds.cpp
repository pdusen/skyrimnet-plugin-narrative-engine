#include <EditorIds.h>

#include <logger.h>

#include <algorithm>
#include <atomic>
#include <utility>
#include <vector>

namespace NarrativeEngine::EditorIds
{
    namespace
    {
        using Entry = std::pair<RE::FormID, RE::BSFixedString>;

        // Sorted by FormID. A sorted vector rather than a hash map because a
        // large load order puts hundreds of thousands of names in the table,
        // and this holds each in sixteen bytes. The BSFixedString keeps its
        // string alive in the engine's pool for as long as the index exists.
        std::vector<Entry> g_names;

        // Published after the index is built, so a worker that reads it later
        // sees the whole thing.
        std::atomic<bool> g_ready{false};

        bool ById(const Entry& entry, RE::FormID id)
        {
            return entry.first < id;
        }
    } // namespace

    void Initialize()
    {
        g_ready.store(false, std::memory_order_release);
        g_names.clear();
        {
            const auto& [map, lock] = RE::TESForm::GetAllFormsByEditorID();
            [[maybe_unused]] const RE::BSReadLockGuard guard{lock};
            if (map) {
                g_names.reserve(map->size());
                for (const auto& [name, form] : *map) {
                    if (form) {
                        g_names.emplace_back(form->GetFormID(), name);
                    }
                }
            }
        }
        std::sort(g_names.begin(), g_names.end(), [](const Entry& a, const Entry& b) { return a.first < b.first; });
        g_ready.store(true, std::memory_order_release);
        logger::info("EditorIds: indexed {} EditorID(s) from the engine's table", g_names.size());
    }

    const char* Of(const RE::TESForm* form)
    {
        if (!form) {
            return "";
        }
        if (const char* own = form->GetFormEditorID(); own && *own) {
            return own;
        }
        if (!g_ready.load(std::memory_order_acquire)) {
            return "";
        }

        const auto id = form->GetFormID();
        for (auto it = std::lower_bound(g_names.begin(), g_names.end(), id, ById);
             it != g_names.end() && it->first == id;
             ++it) {
            // The index is a snapshot of the table. Confirm the name still
            // leads back to this form, so a stale entry can never put another
            // record's name on it.
            const char* name = it->second.c_str();
            if (RE::TESForm::LookupByEditorID(name) == form) {
                return name;
            }
        }
        return "";
    }
} // namespace NarrativeEngine::EditorIds
