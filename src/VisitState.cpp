#include <VisitState.h>

#include <logger.h>

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <array>
#include <atomic>
#include <deque>
#include <mutex>
#include <string>

namespace NarrativeEngine::VisitState
{
    namespace
    {
        // Editor ID of the quest that carries the state-machine phase.
        // Resolved lazily on first DerivePhase() call — safe before the
        // CK content has been authored (Step 7); DerivePhase just returns
        // Idle in that case.
        constexpr const char* kVisitQuestEditorID = "_ne_VisitQuest";

        // Co-save record version. v1 payload matches the Snapshot fields
        // per the design's Persistence section.
        constexpr std::uint32_t kRecordVersion = 1;

        std::mutex                              g_mutex;
        Snapshot                                g_snapshot;
        std::deque<HistoryEntry>                g_history;

        // Composing pseudo-state flag — see the header for the rationale.
        std::atomic<bool>                       g_composingSender{false};

        // Cached pointer to `_ne_VisitQuest`. Resolved lazily on first
        // access; nullptr if the EditorID doesn't resolve (Step 7 CK
        // content not yet authored, or the ESP failed to load).
        RE::TESQuest*                           g_visitQuest = nullptr;
        std::once_flag                          g_visitQuestLookupOnce;

        void EnsureVisitQuestResolved()
        {
            std::call_once(g_visitQuestLookupOnce, []() {
                auto* form = RE::TESForm::LookupByEditorID(kVisitQuestEditorID);
                if (!form) {
                    logger::info(
                        "VisitState: quest '{}' not resolved yet — DerivePhase() will report Idle",
                        kVisitQuestEditorID);
                    return;
                }
                auto* quest = form->As<RE::TESQuest>();
                if (!quest) {
                    logger::warn(
                        "VisitState: EditorID '{}' resolved to a non-Quest form",
                        kVisitQuestEditorID);
                    return;
                }
                g_visitQuest = quest;
                logger::info("VisitState: resolved '{}' -> {}", kVisitQuestEditorID,
                             fmt::ptr(g_visitQuest));
            });
        }

        // ------ Small serialization helpers ------

        void WriteString(SKSE::SerializationInterface* intfc, const std::string& s)
        {
            const auto len = static_cast<std::uint16_t>(s.size());
            intfc->WriteRecordData(len);
            if (len > 0) intfc->WriteRecordData(s.data(), len);
        }

        bool ReadString(SKSE::SerializationInterface* intfc, std::string& out)
        {
            std::uint16_t len = 0;
            if (intfc->ReadRecordData(len) != sizeof(len)) return false;
            out.resize(len);
            if (len > 0 && intfc->ReadRecordData(out.data(), len) != len) return false;
            return true;
        }
    }

    void SetComposingSender(bool value)
    {
        g_composingSender.store(value, std::memory_order_release);
    }

    bool GetComposingSender()
    {
        return g_composingSender.load(std::memory_order_acquire);
    }

    Snapshot GetSnapshot()
    {
        std::scoped_lock lock(g_mutex);
        return g_snapshot;
    }

    void SetSnapshot(const Snapshot& snapshot)
    {
        std::scoped_lock lock(g_mutex);
        g_snapshot = snapshot;
    }

    void Reset()
    {
        std::scoped_lock lock(g_mutex);
        g_snapshot = Snapshot{};
    }

    void PushHistory(HistoryEntry entry)
    {
        std::scoped_lock lock(g_mutex);
        g_history.push_back(std::move(entry));
        while (g_history.size() > kHistoryRingSize) {
            g_history.pop_front();
        }
    }

    std::vector<HistoryEntry> GetHistory()
    {
        std::scoped_lock lock(g_mutex);
        return { g_history.begin(), g_history.end() };
    }

    Mode DerivePhase()
    {
        if (g_composingSender.load(std::memory_order_acquire)) {
            return Mode::Composing;
        }

        EnsureVisitQuestResolved();
        if (!g_visitQuest) {
            return Mode::Idle;
        }

        const auto stage = g_visitQuest->GetCurrentStageID();
        switch (stage) {
            case 10:  return Mode::Salutation;
            case 20:  return Mode::Discuss;
            case 25:  return Mode::OnHold;
            case 27:  return Mode::ReEngage;
            case 30:  return Mode::Valediction;
            case 50:  return Mode::ReturnHome;
            case 0:
            case 60:  // rollback stage — teardown in progress; treat as Idle
            case 200: // shutdown fragment running; treat as Idle
            default:
                return Mode::Idle;
        }
    }

    void OnSave(SKSE::SerializationInterface* intfc)
    {
        if (!intfc) return;
        if (!intfc->OpenRecord(kRecordTypeId, kRecordVersion)) {
            logger::error("VisitState::OnSave: OpenRecord failed");
            return;
        }

        Snapshot snap;
        {
            std::scoped_lock lock(g_mutex);
            snap = g_snapshot;
        }

        // Fixed-size scalars first, then variable-length strings.
        intfc->WriteRecordData(snap.senderFormID);
        intfc->WriteRecordData(snap.returnCellFormID);
        intfc->WriteRecordData(snap.returnPosition.x);
        intfc->WriteRecordData(snap.returnPosition.y);
        intfc->WriteRecordData(snap.returnPosition.z);
        intfc->WriteRecordData(snap.returnAngleZ);
        intfc->WriteRecordData(snap.returnAnchorFormID);
        intfc->WriteRecordData(snap.dispatchedAtRealSeconds);
        intfc->WriteRecordData(snap.ignoreNudgeCount);
        intfc->WriteRecordData(snap.consecutivePollFailures);

        WriteString(intfc, snap.briefingText);
        WriteString(intfc, snap.topicTag);
        WriteString(intfc, snap.mood);

        const auto tagCount = static_cast<std::uint16_t>(snap.tags.size());
        intfc->WriteRecordData(tagCount);
        for (const auto& t : snap.tags) WriteString(intfc, t);
    }

    void OnLoad(SKSE::SerializationInterface* intfc,
                std::uint32_t                 version,
                std::uint32_t                 length)
    {
        if (!intfc) return;
        if (version != kRecordVersion) {
            logger::warn(
                "VisitState::OnLoad: unknown version {} (length={}); clearing snapshot",
                version, length);
            OnRevert();
            return;
        }

        Snapshot loaded;

        auto shortRead = [&](const char* what) {
            logger::error("VisitState::OnLoad: short read on {}", what);
            OnRevert();
        };

        if (intfc->ReadRecordData(loaded.senderFormID)         != sizeof(loaded.senderFormID)         ||
            intfc->ReadRecordData(loaded.returnCellFormID)     != sizeof(loaded.returnCellFormID)     ||
            intfc->ReadRecordData(loaded.returnPosition.x)     != sizeof(loaded.returnPosition.x)     ||
            intfc->ReadRecordData(loaded.returnPosition.y)     != sizeof(loaded.returnPosition.y)     ||
            intfc->ReadRecordData(loaded.returnPosition.z)     != sizeof(loaded.returnPosition.z)     ||
            intfc->ReadRecordData(loaded.returnAngleZ)         != sizeof(loaded.returnAngleZ)         ||
            intfc->ReadRecordData(loaded.returnAnchorFormID)   != sizeof(loaded.returnAnchorFormID)   ||
            intfc->ReadRecordData(loaded.dispatchedAtRealSeconds) != sizeof(loaded.dispatchedAtRealSeconds) ||
            intfc->ReadRecordData(loaded.ignoreNudgeCount)     != sizeof(loaded.ignoreNudgeCount)     ||
            intfc->ReadRecordData(loaded.consecutivePollFailures) != sizeof(loaded.consecutivePollFailures)) {
            shortRead("Snapshot header");
            return;
        }

        if (!ReadString(intfc, loaded.briefingText) ||
            !ReadString(intfc, loaded.topicTag) ||
            !ReadString(intfc, loaded.mood)) {
            shortRead("Snapshot strings");
            return;
        }

        std::uint16_t tagCount = 0;
        if (intfc->ReadRecordData(tagCount) != sizeof(tagCount)) {
            shortRead("Snapshot tag count");
            return;
        }
        loaded.tags.clear();
        loaded.tags.reserve(tagCount);
        for (std::uint16_t i = 0; i < tagCount; ++i) {
            std::string tag;
            if (!ReadString(intfc, tag)) {
                shortRead("Snapshot tag");
                return;
            }
            loaded.tags.push_back(std::move(tag));
        }

        // Resolve persisted FormIDs through SKSE's mapping table so a
        // load-order change between save and load doesn't corrupt the
        // references. Failures degrade to 0 (nulls out the FormID); the
        // consumers of the snapshot handle 0 via their own fallback paths
        // (e.g. ReturnHome teleport falls back to MoveTo(sender, 0)).
        auto resolve = [&](RE::FormID& id, const char* what) {
            if (id == 0) return;
            RE::FormID resolved = 0;
            if (!intfc->ResolveFormID(id, resolved)) {
                logger::warn("VisitState::OnLoad: could not resolve {} (0x{:08X}); "
                             "clearing to 0",
                             what, id);
                id = 0;
                return;
            }
            id = resolved;
        };
        resolve(loaded.senderFormID,       "senderFormID");
        resolve(loaded.returnCellFormID,   "returnCellFormID");
        resolve(loaded.returnAnchorFormID, "returnAnchorFormID");

        {
            std::scoped_lock lock(g_mutex);
            g_snapshot = std::move(loaded);
        }

        logger::info(
            "VisitState::OnLoad: restored snapshot (sender=0x{:08X}, returnCell=0x{:08X}, "
            "briefing={} chars, tags={})",
            g_snapshot.senderFormID, g_snapshot.returnCellFormID,
            g_snapshot.briefingText.size(), g_snapshot.tags.size());
    }

    void OnRevert()
    {
        {
            std::scoped_lock lock(g_mutex);
            g_snapshot = Snapshot{};
            // Recent-history ring is per-process anyway; clearing on
            // revert keeps its behaviour consistent with a fresh session.
            g_history.clear();
        }
        g_composingSender.store(false, std::memory_order_release);
    }
}
