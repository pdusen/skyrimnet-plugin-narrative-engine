#pragma once

// LetterSmokeTest — throwaway. Verifies whether a single TESObjectBOOK form
// can be (a) populated with LLM-generated content at runtime and (b) reused
// with different content across multiple deliveries. Drives the question
// "can our courier-letter action work?" before we commit to the design.
//
// Loop, started 10 seconds after kPostLoadGame / kNewGame:
//   1. Ask SkyrimNet's default LLM for a Ysolda-voice letter via the
//      `narrative_engine_letter_smoke` prompt.
//   2. Mutate `_ne_CourierLetter01`'s display name to "Test Letter 1" and
//      its body to the LLM response.
//   3. Add the book to the player's inventory.
//   4. Wait for TESBookReadEvent on that form.
//   5. Sleep 10 seconds, remove the book, loop.
//
// Body mutation hooks `TESDescription::GetDescription` and substitutes the
// cached LLM text whenever the engine reads our test book's description.
// `BGSLocalizedStringDL` isn't writable through CommonLibSSE-NG — the hook
// is the path of least resistance.
//
// Delete the entire file and its Plugin.cpp wiring when the test concludes.

namespace NarrativeEngine::LetterSmokeTest
{
    // Resolve the test book by EditorID, install the GetDescription hook,
    // register the book-read sink. Idempotent. Call from kDataLoaded AFTER
    // AsyncDispatch::Start and SkyrimNetAPI::Initialize.
    void Initialize();

    // Schedule the first letter request (10 seconds out). Safe to call
    // multiple times — second call while a test is in flight is a no-op.
    // Call from kPostLoadGame and kNewGame.
    void OnPostLoadGame();
}
