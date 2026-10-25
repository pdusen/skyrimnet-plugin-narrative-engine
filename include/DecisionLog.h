#pragma once

#include <LanguageTable.h>
#include <PhaseTracker.h>
#include <TextKeys.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace SKSE
{
    class SerializationInterface;
}

// Persistent, bounded ring buffer of every Director evaluation (including
// no-beat evaluations). Doubles as the Director's record of its own past
// dispatches — `beatSelected` + `narrativeNote` + `advancedToPhase` capture
// everything Beta-canon needs.
//
// Threading: appends and structural mutations are exclusive; snapshot reads
// (Tail, LatestTensionScore) take a shared lock so the SkyrimNet decorators
// from Step 12 can read concurrently from the prompt-rendering thread.
namespace NarrativeEngine::DecisionLog
{
    struct DecisionRecord
    {
        double realTimeSec = 0.0;
        float gameDaysPassed = 0.0f;
        std::uint32_t tensionScore = 0; // 0..100
        PhaseTracker::Phase currentPhase = PhaseTracker::Phase::Exposition;
        std::optional<PhaseTracker::Phase> advancedToPhase; // nullopt = no advancement this tick
        std::string beatSelected;                           // the chosen beat's id; empty = none, or a failure
        std::string beatParametersJSON;
        std::string narrativeNote;                 // the model's own rationale, and nothing else
        std::uint32_t alphaCanonActiveSignals = 0; // bitmask snapshot at evaluation time

        // Why beat selection failed, when it did. The words are the
        // language file's ([Decision]); see ActionText.
        enum class Failure : std::uint8_t
        {
            None,
            BeatMissing, // the model chose a beat the registry does not have
            LlmError,    // the call itself failed
            NotJson,     // the reply was not a JSON object
            NotOffered,  // the model kept choosing something it was not offered
        };
        Failure failure = Failure::None;
        // What the failure was about, verbatim: the beat id, the model's
        // reply, or the validator's message. Diagnostic data, like a log
        // line, so it is kept as it came.
        std::string failureDetail;

        // Where the note came from: the model, or one of the plugin's own
        // stand-ins for it, whose words are the language file's; see
        // NoteText.
        enum class Note : std::uint8_t
        {
            Model,
            InvalidJson,    // the evaluation's reply did not parse
            NotAnObject,    // it parsed, but not as an object
            ForcedDispatch, // a beat fired from the debug UI
        };
        Note note = Note::Model;
    };

    // The keys ActionText and NoteText render from.
    [[nodiscard]] const std::vector<TextKeys::Key>& Keys();

    // What was chosen: the beat's id, or "(failed: ...)" with the reason.
    [[nodiscard]] std::string ActionText(const DecisionRecord& record, const LanguageTable::Table& table);

    // The record's note: the model's own words, or the stand-in for why
    // there were none.
    [[nodiscard]] std::string NoteText(const DecisionRecord& record, const LanguageTable::Table& table);

    // The failure and the note as tokens, for the Director's own prompt to
    // word: beat_missing, llm_error, not_json, not_offered, or empty for none;
    // model, invalid_json, not_an_object or forced_dispatch.
    [[nodiscard]] std::string_view FailureId(DecisionRecord::Failure failure) noexcept;
    [[nodiscard]] std::string_view NoteId(DecisionRecord::Note note) noexcept;

    // SKSE co-save record type ID for the decision log. Frozen.
    inline constexpr std::uint32_t kRecordTypeId = 'DCLG';

    // Push a record onto the back. Trims the front if over the configured
    // max-entries cap.
    void Append(DecisionRecord record);

    // Copy of the last `n` records, oldest first. If `n` exceeds the current
    // size, returns everything available. Safe to call from any thread.
    std::vector<DecisionRecord> Tail(std::size_t n);

    // Drop every record.
    void Clear();

    // Set the ring-buffer cap and trim immediately if exceeded. Wired at
    // kDataLoaded from Settings::Get().decisionLogMaxEntries.
    void SetMaxEntries(std::size_t n);

    // Most-recent decision's tension score, or nullopt if the log is empty.
    // Backs the Step 12 `ne_narrative_tension` decorator (which maps nullopt
    // to "0" so the prompt template always has a numeric value to interpret).
    std::optional<std::uint32_t> LatestTensionScore();

    // SKSE serialization callbacks. OnLoad receives the per-record version
    // and length advanced past the header by Plugin.cpp's dispatcher.
    void OnSave(SKSE::SerializationInterface* intfc);
    void OnLoad(SKSE::SerializationInterface* intfc, std::uint32_t version, std::uint32_t length);
    void OnRevert();
} // namespace NarrativeEngine::DecisionLog
