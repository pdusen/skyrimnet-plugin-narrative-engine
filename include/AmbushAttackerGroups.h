#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <MainThread.h>

#include <RE/Skyrim.h>

// AmbushAttackerGroups — the "who ambushes you" table, loaded from disk.
//
// The table is DATA, not code. It ships as
// Data/SKSE/Plugins/NarrativeEngine/AttackerGroups.ini and is parsed once
// at kDataLoaded. Adding a group, retuning one, or replacing the whole
// set is a text edit with no rebuild. This module owns the parse, the
// validation, and the runtime lookup — it authors nothing itself.
//
// It lives in its own file rather than in NarrativeEngine.ini because
// that file is tunables and this is content: a broken group table must
// not be able to take the settings surface down with it, and users
// should be able to swap group sets independently.
//
// == Per-group failure isolation ==
//
// The load-bearing property of this loader. Every [Group:<id>] section
// validates independently; a section that fails is skipped with a
// specific logged reason naming the section, and every other group still
// loads. A typo in someone's fifth group must not cost them the four
// that parsed fine. That requirement is why the format is INI rather
// than JSON or YAML — both of those fail the *entire file* on one bad
// character.
//
// Unknown keys are warned about and ignored rather than being fatal, so
// a group file authored against a later build still loads on this one.
//
// == Eligibility semantics ==
//
// Every eligibility key is optional; absent means "no constraint". A
// group with no eligibility keys at all is always eligible, and at least
// one such group must exist or the beat cannot fire in large parts of
// the world (the loader warns when none does).
//
// Different keys AND together. Repeating a key ORs its values — with one
// deliberate exception, RequireGlobal, where each occurrence must hold.
// The distinction: most keys name a bare entity on a single shared
// dimension ("which hold", "which keyword"), so repeats are alternatives.
// RequireGlobal's value is a whole predicate over a *different* variable
// each time, so repeats are additional requirements. Every Forbid* key
// is "none of these may match" regardless.
//
// == Threading ==
//
// Load() runs at kDataLoaded, after HoldGrid::Initialize() — hold
// validation needs form lookups available.
//
// Everything else is safe from any thread. Eligibility reads stable
// singleton pointers, Region::ForPlayer() (which TravelEventLog::Poll
// also calls from the plugin thread), Calendar, and plain flag / value
// loads on factions, keywords, quests and globals — the same shapes
// docs/MAIN_THREAD_STUTTER_AUDIT.md sanctions off-main and that the
// per-beat IsAvailable path already performs there. Find(),
// ComposeRoster(), and the count accessors are pure reads of immutable
// post-load state.
//
// The MainThread::Token overloads exist for callers already inside a
// Run / FireAndForget lambda, following the EngineUtils convention;
// they do not indicate a requirement.
namespace SKSE
{
    class SerializationInterface;
}

namespace NarrativeEngine::AmbushAttackerGroups
{
    // Default for a group's `CooldownGameHours` when the key is absent.
    // Long enough that the same faction doesn't feel like it's stalking
    // the player, short enough that a group isn't effectively retired
    // after one use.
    inline constexpr int kDefaultGroupCooldownHours = 24;

    // Comparison operator for the predicate-shaped eligibility keys
    // (RequireGlobal, RequireQuestStage).
    enum class CompareOp : std::uint8_t
    {
        Equal,
        NotEqual,
        Greater,
        Less,
        GreaterEqual,
        LessEqual,
    };

    // `<GlobalEditorID> <op> <value>`, resolved at load.
    struct GlobalConstraint
    {
        RE::TESGlobal* global = nullptr;
        CompareOp op = CompareOp::Equal;
        float value = 0.0f;
    };

    // `<QuestEditorID> <op> <stage>`, resolved at load. Compares against
    // TESQuest::GetCurrentStageID(), which is the *latest* stage reached
    // rather than "was this stage ever set" — CommonLibSSE-NG exposes no
    // GetStageDone equivalent. Prefer RequireQuestComplete where it will
    // do; reach for this only when a specific branch matters.
    struct QuestStageConstraint
    {
        RE::TESQuest* quest = nullptr;
        CompareOp op = CompareOp::Equal;
        std::uint16_t stage = 0;
    };

    // A game-hour window, `<start>-<end>` in 24-hour game time. WRAPS
    // across midnight: 20-6 means 8pm through 6am, which is the case
    // that actually matters and the one that would silently invert a
    // nocturnal group's schedule if implemented naively. Equal bounds
    // are rejected at load as ambiguous rather than guessed at.
    struct HourWindow
    {
        float start = 0.0f;
        float end = 0.0f;

        // True when `hour` falls in [start, end), handling the wrap.
        bool Contains(float hour) const;
    };

    // Resolved eligibility constraints for one group. Every vector is
    // empty when its key was absent, which reads as "no constraint".
    struct Eligibility
    {
        // Compared against Region::ForPlayer().holdFormID. Repeats OR.
        std::vector<RE::FormID> requireHolds;
        std::vector<RE::FormID> forbidHolds;

        // Actor::IsInFaction on the player. Repeats OR.
        std::vector<RE::TESFaction*> requireFactions;
        std::vector<RE::TESFaction*> forbidFactions;

        // Actor::HasKeyword on the player. This is how vampirism is
        // detected — turning swaps the player's race to <Race>Vampire,
        // and every vanilla vampire race carries the `Vampire` keyword
        // (as does Dawnguard's beast race, so it holds in Vampire Lord
        // form too). Repeats OR.
        std::vector<RE::BGSKeyword*> requireKeywords;
        std::vector<RE::BGSKeyword*> forbidKeywords;

        // TESQuest::IsCompleted(). Handles quests with several
        // completion stages without needing to know the numbers.
        // Repeats OR.
        std::vector<RE::TESQuest*> requireQuestsComplete;
        std::vector<RE::TESQuest*> forbidQuestsComplete;

        // Repeats OR.
        std::vector<QuestStageConstraint> requireQuestStages;
        std::vector<QuestStageConstraint> forbidQuestStages;

        // Repeats AND — see the note on eligibility semantics above.
        std::vector<GlobalConstraint> requireGlobals;

        // RE::Calendar::GetHour(). Repeats OR.
        std::vector<HourWindow> requireHours;

        // Actor::GetLevel().
        std::optional<int> minPlayerLevel;
        std::optional<int> maxPlayerLevel;

        // True when no eligibility key was set at all, i.e. the group is
        // eligible everywhere and always. Exactly one shipped group
        // (`bandits`) is unconstrained, and that is load-bearing: it is
        // the sole guarantee the eligible set is non-empty on an
        // otherwise-valid tick.
        bool IsUnconstrained() const;
    };

    // Resolved roster forms for one group.
    struct Roster
    {
        // Rank and file. At least one is required. Repeated `LineForm`
        // keys are drawn round-robin so a group can mix lists.
        std::vector<RE::TESLevCharacter*> lineForms;
        // Optional. Roughly one per three attackers when set.
        RE::TESLevCharacter* rangedForm = nullptr;
        // Optional. One, and only at counts of 3+.
        RE::TESLevCharacter* leaderForm = nullptr;
    };

    struct Group
    {
        // Section id — what the Director returns as `attacker_group`.
        std::string id;
        std::string displayName;
        std::string flavor;

        // Master switch, distinct from eligibility. A disabled group is
        // still parsed and validated (so re-enabling it later can't
        // surprise you with an error that was hiding), but is never
        // returned by EligibleGroups or Find. Defaults true when the key
        // is absent; a malformed value is an error rather than a silent
        // false, because a group vanishing because someone typed
        // `Enabled = yes` would be near-impossible to diagnose.
        bool enabled = true;

        // In-game hours this group sits out after being used. Stamped
        // when an ambush actually spawns, not when one is merely
        // considered, so a failed compose doesn't retire the group.
        // 0 disables the cooldown for this group.
        int cooldownGameHours = kDefaultGroupCooldownHours;

        Roster roster;
        Eligibility eligibility;
    };

    // Player-and-world state captured ONCE by the caller, then tested
    // against every group. Capturing it up front rather than re-reading
    // per group keeps Region::ForPlayer() (which does a grid lookup and
    // possibly a parent-location walk) off the per-group path.
    struct EligibilityContext
    {
        RE::Actor* player = nullptr;
        RE::FormID holdFormID = 0;
        // Hour of day, 0-24, for RequireGameHour windows.
        float gameHour = 0.0f;
        // Absolute game-hours since the calendar epoch, for per-group
        // cooldowns. Distinct from gameHour above — that one wraps.
        double nowGameHours = 0.0;
        std::uint16_t playerLevel = 0;
    };

    // Main-thread. Parse and validate the group file. Safe to call more
    // than once; a reload replaces the table wholesale. A missing file
    // is logged at error level and yields zero groups — loud, because a
    // silently disabled beat is worse than a noisy one.
    void Load();

    // Snapshot the player/world state EligibleGroups needs. Returns a
    // context with a null player when there is no player yet.
    EligibilityContext CaptureContext();
    EligibilityContext CaptureContext(const MainThread::Token&);

    // Every enabled group whose eligibility holds against `ctx`, in
    // file order. Pointers are stable until the next Load().
    std::vector<const Group*> EligibleGroups(const EligibilityContext& ctx);
    std::vector<const Group*> EligibleGroups(const MainThread::Token&, const EligibilityContext& ctx);

    // Look up a group by id. Returns nullptr for an unknown id OR for a
    // disabled one — a disabled group must be invisible to callers, not
    // merely deprioritised.
    const Group* Find(std::string_view id);

    // Plain-data view of one eligible group, for the beat-select prompt.
    struct GroupSummary
    {
        std::string id;
        std::string displayName;
        std::string flavor;
    };

    // Capture + evaluate + flatten in one call, for BeatSystem's
    // prompt-context build.
    //
    // Returns plain strings rather than `const Group*` per the
    // return-by-value discipline in docs/THREADING_MODEL.md — no
    // engine-owned pointers escape to a worker thread.
    std::vector<GroupSummary> EligibleGroupSummaries();
    std::vector<GroupSummary> EligibleGroupSummaries(const MainThread::Token&);

    // Deterministic roster for `count` attackers: one leader if the
    // group has one and count >= 3, roughly one ranged per three when
    // the group has one, and the balance drawn round-robin from
    // lineForms. Deterministic given (group, count) so the mix is
    // reproducible from the logs. Returns `count` entries, or fewer only
    // if the group somehow has no line forms (validation prevents that).
    std::vector<RE::TESLevCharacter*> ComposeRoster(const Group& group, int count);

    // Counts from the last Load(), for logging and for the beat's
    // availability gate.
    std::size_t EnabledGroupCount();
    std::size_t TotalGroupCount();

    // ---- Per-group cooldowns -------------------------------------
    //
    // Keyed by group id rather than by index, so the table survives the
    // user reordering, adding, or removing groups between sessions. An
    // id that no longer exists in the file is simply never consulted.

    // Record `id` as used, as of now. Called when an ambush actually
    // spawns — a failed compose must not retire the group.
    void StampGroupUsed(std::string_view id);

    // In-game hours left before `id` is usable again; 0 when it is
    // ready now, has no cooldown configured, or is unknown.
    double RemainingCooldownGameHours(std::string_view id);

    // Cosave hooks, driven by AmbushBeat_Persistence. Layout:
    //     u32 count
    //     [u32 idLen + idLen chars + double stamp] * count
    void SerializeCooldowns(SKSE::SerializationInterface* intfc);
    bool DeserializeCooldowns(SKSE::SerializationInterface* intfc);
    void ClearCooldowns();
} // namespace NarrativeEngine::AmbushAttackerGroups
