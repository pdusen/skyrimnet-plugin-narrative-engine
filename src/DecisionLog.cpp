#include <DecisionLog.h>

#include <logger.h>

#include <deque>
#include <mutex>
#include <shared_mutex>
#include <utility>

namespace NarrativeEngine::DecisionLog
{
    namespace
    {
        constexpr std::uint32_t kRecordVersion = 1;

        std::shared_mutex g_mutex;
        std::deque<DecisionRecord> g_records;
        std::size_t g_maxEntries = 200; // matches Settings default; SetMaxEntries overrides

        void TrimLocked()
        {
            while (g_records.size() > g_maxEntries) {
                g_records.pop_front();
            }
        }

        // ---- Length-prefixed string helpers --------------------------------
        // Format: uint32_t length, then raw bytes (no terminator).

        void WriteString(SKSE::SerializationInterface* intfc, const std::string& s)
        {
            const auto len = static_cast<std::uint32_t>(s.size());
            intfc->WriteRecordData(len);
            if (len > 0) {
                intfc->WriteRecordData(s.data(), len);
            }
        }

        bool ReadString(SKSE::SerializationInterface* intfc, std::string& out)
        {
            std::uint32_t len = 0;
            if (intfc->ReadRecordData(len) != sizeof(len)) {
                return false;
            }
            out.resize(len);
            if (len > 0 && intfc->ReadRecordData(out.data(), len) != len) {
                return false;
            }
            return true;
        }
    } // namespace

    void Append(DecisionRecord record)
    {
        std::unique_lock lock(g_mutex);
        g_records.push_back(std::move(record));
        TrimLocked();
    }

    std::vector<DecisionRecord> Tail(std::size_t n)
    {
        std::shared_lock lock(g_mutex);
        if (n >= g_records.size()) {
            return {g_records.begin(), g_records.end()};
        }
        return {g_records.end() - static_cast<std::ptrdiff_t>(n), g_records.end()};
    }

    void Clear()
    {
        std::unique_lock lock(g_mutex);
        g_records.clear();
    }

    void SetMaxEntries(std::size_t n)
    {
        std::unique_lock lock(g_mutex);
        g_maxEntries = n;
        TrimLocked();
    }

    std::optional<std::uint32_t> LatestTensionScore()
    {
        std::shared_lock lock(g_mutex);
        if (g_records.empty()) {
            return std::nullopt;
        }
        return g_records.back().tensionScore;
    }

    void OnSave(SKSE::SerializationInterface* intfc)
    {
        if (!intfc) {
            return;
        }
        if (!intfc->OpenRecord(kRecordTypeId, kRecordVersion)) {
            logger::error("DecisionLog::OnSave: OpenRecord failed");
            return;
        }

        // Take a snapshot under the lock, then write outside the lock so
        // SKSE I/O isn't serialized against concurrent readers.
        std::vector<DecisionRecord> snapshot;
        {
            std::shared_lock lock(g_mutex);
            snapshot.assign(g_records.begin(), g_records.end());
        }

        const auto count = static_cast<std::uint32_t>(snapshot.size());
        intfc->WriteRecordData(count);

        for (const auto& r : snapshot) {
            intfc->WriteRecordData(r.realTimeSec);
            intfc->WriteRecordData(r.gameDaysPassed);
            intfc->WriteRecordData(r.tensionScore);

            const auto currentPhaseByte = static_cast<std::uint8_t>(r.currentPhase);
            intfc->WriteRecordData(currentPhaseByte);

            // optional<Phase> → uint8_t flag + uint8_t value. We always
            // emit the value byte (defaulting to Exposition when unset) so
            // the wire format is fixed-width per record header.
            const auto hasAdvanced = static_cast<std::uint8_t>(r.advancedToPhase.has_value() ? 1 : 0);
            intfc->WriteRecordData(hasAdvanced);
            const auto advancedByte =
                static_cast<std::uint8_t>(r.advancedToPhase.value_or(PhaseTracker::Phase::Exposition));
            intfc->WriteRecordData(advancedByte);

            WriteString(intfc, r.beatSelected);
            WriteString(intfc, r.beatParametersJSON);
            WriteString(intfc, r.narrativeNote);

            intfc->WriteRecordData(r.alphaCanonActiveSignals);
        }
    }

    void OnLoad(SKSE::SerializationInterface* intfc, std::uint32_t version, std::uint32_t length)
    {
        if (!intfc) {
            return;
        }
        if (version != kRecordVersion) {
            logger::warn(
                "DecisionLog::OnLoad: unrecognized version {} (length={}); discarding payload", version, length);
            Clear();
            return;
        }

        std::uint32_t count = 0;
        if (intfc->ReadRecordData(count) != sizeof(count)) {
            logger::error("DecisionLog::OnLoad: failed to read entry count");
            Clear();
            return;
        }

        std::deque<DecisionRecord> loaded;
        for (std::uint32_t i = 0; i < count; ++i) {
            DecisionRecord r;
            std::uint8_t currentPhaseByte = 0;
            std::uint8_t hasAdvanced = 0;
            std::uint8_t advancedByte = 0;

            if (intfc->ReadRecordData(r.realTimeSec) != sizeof(r.realTimeSec)
                || intfc->ReadRecordData(r.gameDaysPassed) != sizeof(r.gameDaysPassed)
                || intfc->ReadRecordData(r.tensionScore) != sizeof(r.tensionScore)
                || intfc->ReadRecordData(currentPhaseByte) != sizeof(currentPhaseByte)
                || intfc->ReadRecordData(hasAdvanced) != sizeof(hasAdvanced)
                || intfc->ReadRecordData(advancedByte) != sizeof(advancedByte)) {
                logger::error("DecisionLog::OnLoad: short read on record {}/{}; discarding payload", i, count);
                Clear();
                return;
            }
            if (!ReadString(intfc, r.beatSelected) || !ReadString(intfc, r.beatParametersJSON)
                || !ReadString(intfc, r.narrativeNote)) {
                logger::error("DecisionLog::OnLoad: string read failure on record {}/{}; discarding payload", i, count);
                Clear();
                return;
            }
            if (intfc->ReadRecordData(r.alphaCanonActiveSignals) != sizeof(r.alphaCanonActiveSignals)) {
                logger::error("DecisionLog::OnLoad: failed to read alphaCanonActiveSignals on record {}/{}", i, count);
                Clear();
                return;
            }

            // Validate phase bytes — clamp to Exposition (and drop bad
            // advancement values) so a corrupt save doesn't poison live state.
            if (currentPhaseByte >= static_cast<std::uint8_t>(PhaseTracker::Phase::Count)) {
                logger::warn(
                    "DecisionLog::OnLoad: record {} has invalid currentPhase byte {}; clamping", i, currentPhaseByte);
                currentPhaseByte = 0;
            }
            r.currentPhase = static_cast<PhaseTracker::Phase>(currentPhaseByte);
            if (hasAdvanced) {
                if (advancedByte >= static_cast<std::uint8_t>(PhaseTracker::Phase::Count)) {
                    logger::warn("DecisionLog::OnLoad: record {} has invalid advancedToPhase byte {}; clearing",
                                 i,
                                 advancedByte);
                } else {
                    r.advancedToPhase = static_cast<PhaseTracker::Phase>(advancedByte);
                }
            }

            loaded.push_back(std::move(r));
        }

        {
            std::unique_lock lock(g_mutex);
            g_records = std::move(loaded);
            TrimLocked();
        }
        logger::info("DecisionLog::OnLoad: restored {} record(s)", count);
    }

    void OnRevert()
    {
        Clear();
    }
} // namespace NarrativeEngine::DecisionLog
