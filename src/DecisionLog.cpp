#include <DecisionLog.h>

#include <array>

#include <LLMTextSanitizer.h>
#include <logger.h>

#include <deque>
#include <mutex>
#include <shared_mutex>
#include <utility>

namespace NarrativeEngine::DecisionLog
{
    namespace
    {
        constexpr std::uint32_t kRecordVersion = 2;

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

            intfc->WriteRecordData(static_cast<std::uint8_t>(r.failure));
            WriteString(intfc, r.failureDetail);
            intfc->WriteRecordData(static_cast<std::uint8_t>(r.note));
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
            std::uint8_t failureByte = 0;
            std::uint8_t noteByte = 0;
            if (intfc->ReadRecordData(failureByte) != sizeof(failureByte) || !ReadString(intfc, r.failureDetail)
                || intfc->ReadRecordData(noteByte) != sizeof(noteByte)) {
                logger::error("DecisionLog::OnLoad: failed to read the failure and note on record {}/{}", i, count);
                Clear();
                return;
            }
            r.failure = failureByte <= static_cast<std::uint8_t>(DecisionRecord::Failure::NotOffered)
                            ? static_cast<DecisionRecord::Failure>(failureByte)
                            : DecisionRecord::Failure::None;
            r.note = noteByte <= static_cast<std::uint8_t>(DecisionRecord::Note::ForcedDispatch)
                         ? static_cast<DecisionRecord::Note>(noteByte)
                         : DecisionRecord::Note::Model;

            // Repair the note's UTF-8. Builds before the byte-boundary fix
            // in LLMTextSanitizer::TruncateUTF8 clamped this field with a
            // raw resize(), so any co-save written by one of them can hold a
            // note ending in a split multi-byte character. Left alone it
            // throws type_error.316 out of every dump() that touches the
            // decision log -- which is every Director tick and every
            // dashboard push -- so an affected save would stay broken even
            // on a fixed build. Sanitize drops the malformed tail bytes.
            r.narrativeNote = LLMTextSanitizer::Sanitize(r.narrativeNote);

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

    namespace
    {
        using TextKeys::Key;
        using TextKeys::Param;

        constexpr std::array kReasonParams{Param{"reason", TextTemplate::Kind::Text}};
        constexpr std::array kDetailParams{Param{"detail", TextTemplate::Kind::Text}};
        constexpr Key kFailed{"Decision", "failed", kReasonParams};
        constexpr std::array kReasonKeys{Key{"Decision", "reason.beat_missing", kDetailParams},
                                         Key{"Decision", "reason.llm_error", kDetailParams},
                                         Key{"Decision", "reason.not_json", kDetailParams},
                                         Key{"Decision", "reason.not_offered", kDetailParams}};
        constexpr std::array kNoteKeys{Key{"Decision", "note.invalid_json", {}},
                                       Key{"Decision", "note.not_an_object", {}},
                                       Key{"Decision", "note.forced_dispatch", {}}};
    } // namespace

    const std::vector<TextKeys::Key>& Keys()
    {
        static const std::vector<Key> keys = [] {
            std::vector<Key> out{kFailed};
            out.insert(out.end(), kReasonKeys.begin(), kReasonKeys.end());
            out.insert(out.end(), kNoteKeys.begin(), kNoteKeys.end());
            return out;
        }();
        return keys;
    }

    std::string ActionText(const DecisionRecord& record, const LanguageTable::Table& table)
    {
        if (record.failure == DecisionRecord::Failure::None) {
            return record.beatSelected;
        }
        return table.Render(kFailed, TextTemplate::Arguments{}.Set("reason", FailureText(record, table)));
    }

    std::string FailureText(const DecisionRecord& record, const LanguageTable::Table& table)
    {
        if (record.failure == DecisionRecord::Failure::None) {
            return {};
        }
        const auto& reasonKey = kReasonKeys[static_cast<std::size_t>(record.failure) - 1];
        return table.Render(reasonKey, TextTemplate::Arguments{}.Set("detail", record.failureDetail));
    }

    std::string_view FailureId(DecisionRecord::Failure failure) noexcept
    {
        switch (failure) {
        case DecisionRecord::Failure::BeatMissing:
            return "beat_missing";
        case DecisionRecord::Failure::LlmError:
            return "llm_error";
        case DecisionRecord::Failure::NotJson:
            return "not_json";
        case DecisionRecord::Failure::NotOffered:
            return "not_offered";
        case DecisionRecord::Failure::None:
            break;
        }
        return {};
    }

    std::string_view NoteId(DecisionRecord::Note note) noexcept
    {
        switch (note) {
        case DecisionRecord::Note::InvalidJson:
            return "invalid_json";
        case DecisionRecord::Note::NotAnObject:
            return "not_an_object";
        case DecisionRecord::Note::ForcedDispatch:
            return "forced_dispatch";
        case DecisionRecord::Note::Model:
            break;
        }
        return "model";
    }

    std::string NoteText(const DecisionRecord& record, const LanguageTable::Table& table)
    {
        if (record.note == DecisionRecord::Note::Model) {
            return record.narrativeNote;
        }
        return table.Render(kNoteKeys[static_cast<std::size_t>(record.note) - 1], {});
    }
} // namespace NarrativeEngine::DecisionLog
