#include <AmbushBeat.h>

#include <AmbushAttackerGroups.h>
#include <AmbushSpawnPoints.h>
#include <BeatRegistry.h>
#include <EngineUtils.h>
#include <JsonUtils.h>
#include <LLMTextSanitizer.h>
#include <LocationKeywords.h>
#include <logger.h>
#include <MainThread.h>
#include <QuestUtils.h>
#include <Settings.h>
#include <StuckRecovery.h>

#include <nlohmann/json.hpp>

#include <RE/A/Actor.h>
#include <RE/A/ActorValues.h>
#include <RE/B/BGSRefAlias.h>
#include <RE/N/NiPoint3.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/T/TES.h>
#include <RE/T/TESDataHandler.h>
#include <RE/T/TESLevCharacter.h>
#include <RE/T/TESObjectCELL.h>
#include <RE/T/TESObjectREFR.h>
#include <RE/T/TESQuest.h>

#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace NarrativeEngine
{
    using namespace std::string_view_literals;

    namespace
    {
        constexpr const char* kAmbushQuestEditorID = "_ne_AmbushQuest";
        constexpr const char* kQuestScriptName = "_ne_AmbushQuest";
        constexpr const char* kPlayerRefAliasName = "PlayerRef";

        // Quest stages. 0 is the startup stage and self-advances to 10;
        // C++ drives everything after that.
        constexpr std::uint32_t kStageSpawning = 10;
        constexpr std::uint32_t kStageEngaged = 20;
        constexpr std::uint32_t kStageComplete = 200;

        // RUNNING poll cadence. The master poll runs at 250 ms; this
        // beat only needs to look every few seconds.
        constexpr double kRunningPollSeconds = 5.0;

        // The engage check runs far tighter than that: at a run an
        // attacker covers well over a thousand units in five seconds, so
        // sampling on the completion cadence would first notice them
        // inside the engage radius when they are already on top of the
        // player. Costs nothing once the group has engaged.
        constexpr double kEngageCheckSeconds = 0.5;

        // Ticks VerifyingFill waits for the VM to run the queued
        // FillAttackerSlot calls. VMDispatchOnQuest only reports that a
        // call was queued, so the readback has to happen later.
        constexpr int kFillVerifyMaxTicks = 20;

        // Time to let a freshly-created actor fall and come to rest
        // before judging where it landed. One still in the air reads as
        // unsettled and triggers a pointless relocation.
        constexpr double kSettleWaitSeconds = 1.5;

        // Vertical drift from the requested position still counted as
        // settled. Past this it fell through the world, slid off a
        // ledge, or was ejected out of collision.
        constexpr float kMaxSettleDriftUnits = 300.0f;

        constexpr int kSlotCount = Settings::kAmbushAttackerSlotCount;

        // ---- Resolved forms (kDataLoaded) ---------------------------

        std::atomic<bool> g_pointersResolved{false};
        RE::TESQuest* g_ambushQuest = nullptr;
        RE::BGSRefAlias* g_playerAlias = nullptr;
        std::array<RE::BGSRefAlias*, kSlotCount> g_attackerAliases{};

        // ---- Session state ------------------------------------------

        enum class ComposeSubPhase : std::uint8_t
        {
            Start,
            SelectingPoints,
            StartingQuest,
            Spawning,
            VerifyingFill,
            Settling,
            Arming,
            Failed,
        };

        std::mutex g_stateMutex;

        // Set in OnStart, guarded by g_stateMutex. The group and count
        // come from the Director; the spawn distance deliberately does
        // NOT — it is copied straight from settings. See OnStart.
        std::string g_requestedGroupId;
        int g_requestedCount = 0;
        int g_spawnDistanceUnits = 0;

        // In-world prose from the Director naming the attackers and
        // their grievance. Held until the player is actually in combat,
        // then submitted once as silent narration.
        std::string g_narrationProse;

        // Resolved during COMPOSE. Guarded by g_stateMutex.
        std::string g_activeGroupId;
        int g_activeCount = 0;

        std::atomic<ComposeSubPhase> g_subPhase{ComposeSubPhase::Start};
        std::atomic<int> g_fillVerifyTicks{0};
        std::atomic<double> g_settleAccumulator{0.0};
        // Latches after the one relocation round, so a position that
        // still reads unsettled can't loop COMPOSE forever.
        std::atomic<bool> g_settleRetried{false};
        std::atomic<double> g_runningPollAccumulator{0.0};
        std::atomic<double> g_runningElapsedSeconds{0.0};
        std::atomic<bool> g_composeSucceeded{false};
        std::atomic<bool> g_staleCheckDone{false};
        // Latches when the narration has been fired, so it lands once
        // per encounter no matter how combat starts and stops.
        std::atomic<bool> g_narrationFired{false};

        // Group-wide engage latch. The handoff is all-or-nothing: the
        // first attacker to close on the player turns the whole group
        // hostile, so the trailing half isn't still walking with
        // aggression 0 while the leader is already swinging.
        std::atomic<bool> g_groupEngaged{false};
        std::atomic<double> g_engageCheckAccumulator{0.0};

        // Moves attackers that aren't travelling. Its clock ticks on the
        // plugin thread; everything that touches an actor happens inside
        // a MainThread::Run. The two never overlap — the hop blocks the
        // plugin thread that would otherwise be advancing the clock.
        StuckRecovery::Escort g_escort{"ambush"};

        // References created this attempt, so a partial spawn can be
        // torn down before the aliases are filled — otherwise a failure
        // at attacker 4 of 6 strands the first three.
        std::vector<RE::FormID> g_createdRefs;

        // Carried between COMPOSE sub-phases. Guarded by g_stateMutex.
        std::vector<RE::NiPoint3> g_spawnPoints;
        // Validated-but-unused points from the same search, handed to
        // the escort so a stalled attacker has somewhere real to go.
        std::vector<RE::NiPoint3> g_spawnFallbacks;
        std::vector<RE::TESLevCharacter*> g_roster;

        // ---- Cooldown -----------------------------------------------

        std::mutex g_cooldownMutex;
        double g_lastCompletionGameHours = 0.0;

        // ---- Helpers ------------------------------------------------

        void ResetSessionState()
        {
            {
                std::scoped_lock lock(g_stateMutex);
                g_requestedGroupId.clear();
                g_activeGroupId.clear();
                g_narrationProse.clear();
                g_requestedCount = 0;
                g_spawnDistanceUnits = 0;
                g_activeCount = 0;
                g_createdRefs.clear();
                g_spawnPoints.clear();
                g_spawnFallbacks.clear();
                g_roster.clear();
            }
            g_subPhase.store(ComposeSubPhase::Start, std::memory_order_release);
            g_fillVerifyTicks.store(0, std::memory_order_release);
            g_settleAccumulator.store(0.0, std::memory_order_release);
            g_settleRetried.store(false, std::memory_order_release);
            g_runningPollAccumulator.store(0.0, std::memory_order_release);
            g_runningElapsedSeconds.store(0.0, std::memory_order_release);
            g_composeSucceeded.store(false, std::memory_order_release);
            g_staleCheckDone.store(false, std::memory_order_release);
            g_narrationFired.store(false, std::memory_order_release);
            g_groupEngaged.store(false, std::memory_order_release);
            g_engageCheckAccumulator.store(0.0, std::memory_order_release);
            // Fallback cursors are per-encounter; a fresh group must
            // not inherit a spent one through a recycled FormID.
            g_escort.Clear();
        }

        void SetSubPhase(ComposeSubPhase next, std::string_view reason = {})
        {
            g_subPhase.store(next, std::memory_order_release);
            if (!reason.empty()) {
                logger::info("AmbushBeat: compose sub-phase -> {} ({})", static_cast<int>(next), reason);
            }
        }

        // Ambushes happen on the road and in open wilderness, and
        // nowhere else. Returns nullptr when the spot is usable, or the
        // reason it isn't.
        //
        // Three exclusions, all hard:
        //   * Interiors — the beat spawns a travelling approach.
        //   * Safe locations — towns, farms, inns. Guards and scheduled
        //     NPCs already own those cells.
        //   * Dangerous locations — dungeons, camps, lairs. Vanilla
        //     already populates them with combat; adding more stacks
        //     into gauntlets.
        //
        // Shared by the pre-LLM availability gate and the compose-time
        // re-check so the two can't drift apart.
        const char* AmbushLocationBlocker(bool interior, RE::BGSLocation* loc)
        {
            if (interior) {
                return "interior";
            }
            if (LocationKeywords::IsSafe(loc)) {
                return "safe_location";
            }
            if (LocationKeywords::IsDangerous(loc)) {
                return "dangerous_location";
            }
            return nullptr;
        }

        // Every COMPOSE failure funnels through here so the log always
        // names a specific cause — "the ambush didn't happen" with no
        // reason attached is not a diagnosable report.
        TickResult FailCompose(std::string_view failureReason)
        {
            logger::warn("AmbushBeat: COMPOSE failed — failure_reason='{}'", failureReason);
            g_composeSucceeded.store(false, std::memory_order_release);
            SetSubPhase(ComposeSubPhase::Failed);
            return TickResult{BeatState::CLEANUP};
        }

        // Resolve a leveled-character list to a concrete NPC. Spawning
        // must be handed a TESNPC — an LVLN base yields a reference that
        // never becomes an Actor (see
        // docs/engine-findings/createreferenceatlocation-does-not-resolve-leveled-lists.md).
        //
        // CalculateCurrentFormList is the engine's own resolver, so list
        // flags and level filtering behave natively. Depth cap because a
        // mod-added list can be cyclic.
        RE::TESNPC* ResolveLeveledCharacter(RE::TESLevCharacter* list, std::uint16_t level, int depth = 0)
        {
            constexpr int kMaxDepth = 8;
            if (!list || depth >= kMaxDepth) {
                return nullptr;
            }
            RE::BSScrapArray<RE::CALCED_OBJECT> calced;
            list->CalculateCurrentFormList(level, 1, calced, 0, /*a_usePlayerLevel=*/true);
            for (const auto& entry : calced) {
                if (!entry.form) {
                    continue;
                }
                if (auto* npc = entry.form->As<RE::TESNPC>()) {
                    return npc;
                }
                if (auto* nested = entry.form->As<RE::TESLevCharacter>()) {
                    if (auto* npc = ResolveLeveledCharacter(nested, level, depth + 1)) {
                        return npc;
                    }
                }
            }
            return nullptr;
        }

        RE::Actor* AttackerInSlot(int slot)
        {
            if (slot < 0 || slot >= kSlotCount || !g_attackerAliases[static_cast<std::size_t>(slot)]) {
                return nullptr;
            }
            return g_attackerAliases[static_cast<std::size_t>(slot)]->GetActorReference();
        }

        RE::TESObjectREFR* RefInSlot(int slot)
        {
            if (slot < 0 || slot >= kSlotCount || !g_attackerAliases[static_cast<std::size_t>(slot)]) {
                return nullptr;
            }
            return g_attackerAliases[static_cast<std::size_t>(slot)]->GetReference();
        }

        // Delete every reference this attempt created that is still
        // alive, whether or not its alias ever filled. Corpses are left
        // alone — see Cleanup.
        void DeleteCreatedRefs(std::string_view why)
        {
            std::vector<RE::FormID> ids;
            {
                std::scoped_lock lock(g_stateMutex);
                ids.swap(g_createdRefs);
            }
            int deleted = 0;
            for (const auto id : ids) {
                auto* form = RE::TESForm::LookupByID(id);
                auto* ref = form ? form->AsReference() : nullptr;
                if (!ref) {
                    continue;
                }
                auto* actor = ref->As<RE::Actor>();
                if (actor && actor->IsDead()) {
                    // Leave bodies. They carry the encounter's loot, and
                    // deleting one out from under a player mid-loot is
                    // worse than leaving a dynamic ref for the engine's
                    // own cell reset to reap.
                    continue;
                }
                ref->Disable();
                ref->SetDelete(true);
                ++deleted;
            }
            if (deleted > 0) {
                logger::info("AmbushBeat: deleted {} spawned reference(s) ({})", deleted, why);
            }
        }

        // Retire the previous encounter's quest so a fresh one can
        // start: delete what the aliases still hold, then stop and reset.
        //
        // Deferred to the next dispatch rather than run at the end of an
        // encounter, because stopping the quest releases the aliases and
        // the engine then reaps the corpses before the player can loot
        // them. Reads the aliases, not g_createdRefs, so it still finds
        // the bodies after a save/load.
        void RetireQuest(std::string_view why)
        {
            if (!g_ambushQuest) {
                return;
            }
            int deleted = 0;
            for (int i = 0; i < kSlotCount; ++i) {
                if (auto* ref = RefInSlot(i)) {
                    ref->Disable();
                    ref->SetDelete(true);
                    ++deleted;
                }
            }
            g_ambushQuest->Stop();
            g_ambushQuest->Reset();
            if (deleted > 0) {
                logger::info(
                    "AmbushBeat: retired previous encounter — removed {} leftover reference(s) ({})", deleted, why);
            }
        }

        // End-of-encounter teardown. Deletes the attackers who are still
        // ALIVE and leaves the corpses in their aliases, then marks the
        // quest complete without stopping it — see RetireQuest for why
        // the stop is deferred.
        void Cleanup(const MainThread::Token&, bool stampCooldown)
        {
            // Collect from the aliases, delete survivors, leave corpses.
            // Merge the alias-held set into the created-ref list so a
            // reference that never made it into an alias is covered too.
            {
                std::scoped_lock lock(g_stateMutex);
                for (int i = 0; i < kSlotCount; ++i) {
                    if (auto* ref = RefInSlot(i)) {
                        const auto id = ref->GetFormID();
                        if (std::find(g_createdRefs.begin(), g_createdRefs.end(), id) == g_createdRefs.end()) {
                            g_createdRefs.push_back(id);
                        }
                    }
                }
            }
            DeleteCreatedRefs("cleanup");

            // Mark complete, but do NOT Stop() or Reset(). The quest
            // stays open holding the corpses until the next dispatch
            // retires it.
            if (g_ambushQuest) {
                QuestUtils::VMDispatchQuestSetStage(g_ambushQuest, kStageComplete);
            }

            // Cooldown, but only when COMPOSE actually produced an
            // encounter. A failed spawn must not burn a day.
            if (stampCooldown) {
                const double now = EngineUtils::GetCurrentGameHours();
                {
                    std::scoped_lock lock(g_cooldownMutex);
                    g_lastCompletionGameHours = now;
                }
                logger::info("AmbushBeat: cooldown stamped at {:.2f} game hours", now);
            }
        }

        // ---- COMPOSE steps -------------------------------------------

        // Re-validate the Director's chosen group against eligibility as
        // it stands NOW, not as it stood when the prompt was built. The
        // player can cross a hold border or have dawn break between the
        // two.
        const AmbushAttackerGroups::Group* ResolveGroup(const MainThread::Token& mt, std::string& resolvedIdOut)
        {
            std::string requested;
            {
                std::scoped_lock lock(g_stateMutex);
                requested = g_requestedGroupId;
            }

            const auto ctx = AmbushAttackerGroups::CaptureContext(mt);
            const auto eligible = AmbushAttackerGroups::EligibleGroups(mt, ctx);
            if (eligible.empty()) {
                return nullptr;
            }

            for (const auto* g : eligible) {
                if (g->id == requested) {
                    resolvedIdOut = g->id;
                    return g;
                }
            }

            // Unknown or no-longer-eligible id. Fall back rather than
            // fail — the Director asked for an ambush and the world can
            // still supply one, just not that flavor.
            const auto* fallback = eligible.front();
            logger::warn("AmbushBeat: requested group '{}' is not currently eligible; falling back to '{}'",
                         requested.empty() ? "(none)" : requested.c_str(),
                         fallback->id);
            resolvedIdOut = fallback->id;
            return fallback;
        }

        // ---- COMPOSE -------------------------------------------------

        TickResult TickCompose(const PluginThread::Token& pt)
        {
            switch (g_subPhase.load(std::memory_order_acquire)) {
            case ComposeSubPhase::Start: {
                // Re-check the location. IsAvailable already rejected
                // interiors, safe locations and dangerous ones before the
                // Director was asked, but a beat-select round trip takes
                // seconds and the player can walk into a cave or a town
                // inside that window. Fail before anything is spawned.
                //
                // Read directly rather than through MainThread::Run:
                // these are the same reads IsAvailable makes on the
                // plugin thread (singleton pointer, a constexpr member
                // load, a flag, and a cached-keyword parentLoc walk),
                // sanctioned off-main by docs/MAIN_THREAD_STUTTER_AUDIT.md.
                auto* pc = RE::PlayerCharacter::GetSingleton();
                if (!pc) {
                    return FailCompose("no_player");
                }
                auto* cell = pc->GetParentCell();
                if (const char* blocker =
                        AmbushLocationBlocker(!cell || cell->IsInteriorCell(), pc->GetCurrentLocation())) {
                    return FailCompose(blocker);
                }
                SetSubPhase(ComposeSubPhase::SelectingPoints);
                return {};
            }

            case ComposeSubPhase::SelectingPoints: {
                // One main-thread hop: pick the group, size the roster,
                // and search for ground. All three read engine state.
                const auto outcome = MainThread::Run(pt, [](const MainThread::Token& mt) {
                    struct Outcome
                    {
                        bool groupOk = false;
                        bool pointsOk = false;
                        int count = 0;
                        std::string groupId;
                    } o;

                    const auto* group = ResolveGroup(mt, o.groupId);
                    if (!group) {
                        return o;
                    }
                    o.groupOk = true;

                    int requestedCount = 0;
                    int spawnDistanceUnits = 0;
                    {
                        std::scoped_lock lock(g_stateMutex);
                        requestedCount = g_requestedCount;
                        spawnDistanceUnits = g_spawnDistanceUnits;
                    }

                    auto* player = RE::PlayerCharacter::GetSingleton();
                    auto found = AmbushSpawnPoints::Find(mt, player, spawnDistanceUnits, requestedCount);
                    if (!found.Ok()) {
                        return o;
                    }

                    // The search may return fewer usable points than
                    // asked for; the roster follows the points, not the
                    // request. Log both so the Director's effect stays
                    // traceable.
                    o.count = static_cast<int>(found.spawnPoints.size());
                    auto roster = AmbushAttackerGroups::ComposeRoster(*group, o.count);
                    if (roster.empty()) {
                        return o;
                    }

                    {
                        std::scoped_lock lock(g_stateMutex);
                        g_spawnPoints = std::move(found.spawnPoints);
                        g_spawnFallbacks = std::move(found.fallbacks);
                        g_roster = std::move(roster);
                        g_activeGroupId = o.groupId;
                        g_activeCount = o.count;
                    }
                    o.pointsOk = true;
                    return o;
                });

                if (!outcome.groupOk) {
                    return FailCompose("no_eligible_group");
                }
                if (!outcome.pointsOk) {
                    return FailCompose("no_spawn_point");
                }

                int requested = 0;
                {
                    std::scoped_lock lock(g_stateMutex);
                    requested = g_requestedCount;
                }
                logger::info("AmbushBeat: group='{}' attackers requested={} resolved={}",
                             outcome.groupId,
                             requested,
                             outcome.count);
                SetSubPhase(ComposeSubPhase::StartingQuest);
                return {};
            }

            case ComposeSubPhase::StartingQuest: {
                const bool started = MainThread::Run(pt, [](const MainThread::Token&) {
                    if (!g_ambushQuest) {
                        return false;
                    }
                    // The previous encounter's deferred teardown.
                    // Idempotent, so it no-ops on the first ambush.
                    RetireQuest("making room for a new encounter");

                    bool engineResult = false;
                    const bool callOk = g_ambushQuest->EnsureQuestStarted(engineResult, /*a_startNow=*/true);
                    if (!callOk || !engineResult) {
                        logger::warn(
                            "AmbushBeat: EnsureQuestStarted failed (callOk={}, engineResult={})", callOk, engineResult);
                        return false;
                    }
                    return true;
                });
                if (!started) {
                    return FailCompose("quest_start_failed");
                }
                SetSubPhase(ComposeSubPhase::Spawning);
                return {};
            }

            case ComposeSubPhase::Spawning: {
                const bool spawned = MainThread::Run(pt, [](const MainThread::Token&) {
                    std::vector<RE::NiPoint3> points;
                    std::vector<RE::TESLevCharacter*> roster;
                    {
                        std::scoped_lock lock(g_stateMutex);
                        points = g_spawnPoints;
                        roster = g_roster;
                    }
                    auto* player = RE::PlayerCharacter::GetSingleton();
                    if (!player || points.empty() || roster.empty()) {
                        return false;
                    }

                    const std::uint16_t playerLevel = player->GetLevel();
                    const std::size_t n = std::min(points.size(), roster.size());
                    for (std::size_t i = 0; i < n; ++i) {
                        auto* list = roster[i];
                        if (!list) {
                            continue;
                        }
                        // Resolve the leveled list to a concrete NPC
                        // FIRST. Passing the TESLevCharacter directly
                        // type-checks and produces a reference, but the
                        // reference stays an unresolved placeholder that
                        // never becomes an Actor — see
                        // ResolveLeveledCharacter's comment.
                        auto* base = ResolveLeveledCharacter(list, playerLevel);
                        if (!base) {
                            logger::warn("AmbushBeat: leveled list '{}' (0x{:08X}) resolved to no NPC at "
                                         "player level {}",
                                         list->GetFormEditorID() ? list->GetFormEditorID() : "?",
                                         list->GetFormID(),
                                         playerLevel);
                            return false;
                        }
                        // PlaceObjectAtMe (native PlaceAtMe) rather than
                        // CreateReferenceAtLocation: it places in the
                        // anchor's own cell and constructs the right
                        // object class, so an NPC base yields an Actor
                        // whose alias fill actually works.
                        auto ref = player->PlaceObjectAtMe(base, /*a_forcePersist=*/true);
                        if (!ref) {
                            logger::warn("AmbushBeat: PlaceObjectAtMe returned no reference for slot {}", i);
                            return false;
                        }

                        // Relocate to the spawn point in this same frame,
                        // before 3D finishes loading, so the actor is
                        // never rendered standing on the player.
                        if (auto* spawnedActor = ref->As<RE::Actor>()) {
                            spawnedActor->SetPosition(points[i], /*a_updateCharController=*/true);
                        } else {
                            ref->SetPosition(points[i]);
                        }
                        ref->Update3DPosition(/*a_warp=*/true);

                        // ForceRefTo rejects a ref with no valid engine
                        // handle, so check that here rather than
                        // inferring it from the Papyrus log later.
                        const auto refHandle = ref->CreateRefHandle();
                        const bool handleOk = static_cast<bool>(refHandle);
                        auto* landedCell = ref->GetParentCell();
                        if (!handleOk) {
                            logger::error("AmbushBeat: slot {} ref=0x{:08X} has NO valid engine handle — "
                                          "ForceRefTo will reject it",
                                          i,
                                          ref->GetFormID());
                        }
                        {
                            std::scoped_lock lock(g_stateMutex);
                            g_createdRefs.push_back(ref->GetFormID());
                        }

                        // Pacify at spawn, not at Arming: an attacker
                        // carries its base aggression until told
                        // otherwise, and would fight on its own
                        // initiative in the meantime.
                        if (auto* pacify = ref->As<RE::Actor>()) {
                            pacify->AsActorValueOwner()->SetActorValue(RE::ActorValue::kAggression, 0.0f);
                        }

                        // ForceRefTo has no native binding, so the fill
                        // goes through the quest script. Fire-and-forget,
                        // hence the readback on a later tick.
                        //
                        // Passed as a FormID, not an ObjectReference; see
                        // docs/engine-findings/passing-references-to-papyrus-from-cpp.md.
                        QuestUtils::VMDispatchOnQuest(g_ambushQuest,
                                                      kQuestScriptName,
                                                      "FillAttackerSlot",
                                                      static_cast<std::int32_t>(i),
                                                      static_cast<std::int32_t>(ref->GetFormID()));
                        logger::info("AmbushBeat: spawned slot {} ref=0x{:08X} base=0x{:08X} '{}' "
                                     "isActor={} handleOk={} cell=0x{:08X} 3D={}",
                                     i,
                                     ref->GetFormID(),
                                     base->GetFormID(),
                                     base->GetFormEditorID() ? base->GetFormEditorID() : "?",
                                     ref->As<RE::Actor>() != nullptr,
                                     handleOk,
                                     landedCell ? landedCell->GetFormID() : 0u,
                                     ref->Get3D() != nullptr);
                    }
                    return true;
                });

                if (!spawned) {
                    // Partial spawn is the important correctness case —
                    // tear down whatever did get created.
                    MainThread::Run(pt, [](const MainThread::Token&) {
                        DeleteCreatedRefs("partial spawn rollback");
                        return 0;
                    });
                    return FailCompose("spawn_failed");
                }
                g_fillVerifyTicks.store(0, std::memory_order_release);
                SetSubPhase(ComposeSubPhase::VerifyingFill);
                return {};
            }

            case ComposeSubPhase::VerifyingFill: {
                int expected = 0;
                {
                    std::scoped_lock lock(g_stateMutex);
                    expected = g_activeCount;
                }
                const int filled = MainThread::Run(pt, [expected](const MainThread::Token&) {
                    int n = 0;
                    for (int i = 0; i < expected && i < kSlotCount; ++i) {
                        if (RefInSlot(i)) {
                            ++n;
                        }
                    }
                    return n;
                });

                if (filled >= expected) {
                    // Report every slot unconditionally, including ones
                    // whose reference is not an actor — gating the log on
                    // the cast hides exactly the case worth seeing.
                    const int actorCount = MainThread::Run(pt, [expected](const MainThread::Token&) {
                        int actors = 0;
                        for (int i = 0; i < expected && i < kSlotCount; ++i) {
                            auto* ref = RefInSlot(i);
                            if (!ref) {
                                logger::warn("AmbushBeat: slot {} reads empty during fill report", i);
                                continue;
                            }
                            auto* actor = AttackerInSlot(i);
                            auto* baseForm = ref->GetBaseObject();
                            if (actor) {
                                ++actors;
                            }
                            logger::info("AmbushBeat: slot {} filled — ref=0x{:08X} base=0x{:08X} "
                                         "baseType={} isActor={} name='{}'",
                                         i,
                                         ref->GetFormID(),
                                         baseForm ? baseForm->GetFormID() : 0u,
                                         baseForm ? static_cast<int>(baseForm->GetFormType()) : -1,
                                         actor != nullptr,
                                         ref->GetDisplayFullName());
                        }
                        return actors;
                    });

                    if (actorCount < expected) {
                        // Filled, but not with actors: nothing would arm
                        // and the first RUNNING poll would read every
                        // slot as gone.
                        MainThread::Run(pt, [](const MainThread::Token&) {
                            DeleteCreatedRefs("alias filled with non-actor references");
                            return 0;
                        });
                        logger::error("AmbushBeat: {} of {} filled slots hold references that are not "
                                      "actors — the spawned base did not resolve to an NPC",
                                      expected - actorCount,
                                      expected);
                        return FailCompose("spawned_ref_not_actor");
                    }
                    g_settleAccumulator.store(0.0, std::memory_order_release);
                    SetSubPhase(ComposeSubPhase::Settling);
                    return {};
                }

                const int ticks = g_fillVerifyTicks.fetch_add(1, std::memory_order_acq_rel) + 1;
                if (ticks >= kFillVerifyMaxTicks) {
                    MainThread::Run(pt, [](const MainThread::Token&) {
                        DeleteCreatedRefs("fill verification timeout");
                        return 0;
                    });
                    logger::warn(
                        "AmbushBeat: only {}/{} attacker aliases filled after {} ticks", filled, expected, ticks);
                    return FailCompose("alias_fill_timeout");
                }
                return {};
            }

            case ComposeSubPhase::Settling: {
                // Post-spawn settle check. Containment says there is
                // navmesh under the requested point; only a spawned
                // actor can say whether it came to rest on it.
                const double pollInterval = static_cast<double>(Settings::Get().beatSystemPollIntervalMs) / 1000.0;
                const double waited =
                    g_settleAccumulator.fetch_add(pollInterval, std::memory_order_acq_rel) + pollInterval;
                if (waited < kSettleWaitSeconds) {
                    return {};
                }

                int expected = 0;
                std::vector<RE::NiPoint3> intended;
                {
                    std::scoped_lock lock(g_stateMutex);
                    expected = g_activeCount;
                    intended = g_spawnPoints;
                }

                struct SettleResult
                {
                    int checked = 0;
                    int unsettled = 0;
                };

                const bool retried = g_settleRetried.load(std::memory_order_acquire);
                const auto result = MainThread::Run(pt, [expected, &intended, retried](const MainThread::Token&) {
                    SettleResult r;
                    // Attacker 0's point is the search winner — the
                    // best-validated position we have, and therefore
                    // the fallback anyone else gets relocated to.
                    const RE::NiPoint3 fallback = intended.empty() ? RE::NiPoint3{} : intended.front();

                    for (int i = 0; i < expected && i < kSlotCount; ++i) {
                        auto* actor = AttackerInSlot(i);
                        if (!actor) {
                            continue;
                        }
                        ++r.checked;

                        const auto now = actor->GetPosition();
                        const auto& want = (static_cast<std::size_t>(i) < intended.size())
                                               ? intended[static_cast<std::size_t>(i)]
                                               : fallback;
                        const bool drifted = std::fabs(now.z - want.z) > kMaxSettleDriftUnits;
                        const bool offMesh = !StuckRecovery::IsOnNavmesh(now);
                        // Positive evidence, unlike a pre-spawn height
                        // comparison: catches shoreline and shallow-river
                        // cases the search's water gate misses.
                        const bool inWater = actor->IsInWater();
                        if (!drifted && !offMesh && !inWater) {
                            continue;
                        }

                        ++r.unsettled;
                        logger::warn("AmbushBeat: slot {} did not settle (drift_z={:.0f}u offMesh={} "
                                     "inWater={}) at ({:.0f},{:.0f},{:.0f})",
                                     i,
                                     now.z - want.z,
                                     offMesh,
                                     inWater,
                                     now.x,
                                     now.y,
                                     now.z);
                        if (!retried) {
                            // One relocation attempt, onto the winning
                            // point. The char-controller flag and the
                            // warp are both required, or the physics
                            // body stays put and the actor walks back.
                            actor->SetPosition(fallback, /*a_updateCharController=*/true);
                            actor->Update3DPosition(/*a_warp=*/true);
                        }
                    }
                    return r;
                });

                if (result.checked == 0 && expected > 0) {
                    // Nothing to check is not the same as nothing wrong.
                    MainThread::Run(pt, [](const MainThread::Token&) {
                        DeleteCreatedRefs("no attacker resolved at settle check");
                        return 0;
                    });
                    logger::error("AmbushBeat: settle check found 0 of {} expected attackers", expected);
                    return FailCompose("no_attacker_resolved");
                }

                if (result.unsettled == 0) {
                    SetSubPhase(ComposeSubPhase::Arming);
                    return {};
                }

                if (!retried) {
                    logger::info("AmbushBeat: relocated {}/{} unsettled attacker(s) onto the validated "
                                 "spawn point; re-checking",
                                 result.unsettled,
                                 result.checked);
                    g_settleRetried.store(true, std::memory_order_release);
                    g_settleAccumulator.store(0.0, std::memory_order_release);
                    return {};
                }

                // After the retry everyone still failing is standing on
                // the winning point, which passed every gate — the check
                // is likelier wrong than the ground is, so proceed.
                if (result.checked > 0 && result.unsettled >= result.checked) {
                    // Unless nobody settled at all, which is a real
                    // signal that something systematic is broken.
                    MainThread::Run(pt, [](const MainThread::Token&) {
                        DeleteCreatedRefs("no attacker settled");
                        return 0;
                    });
                    return FailCompose("settle_failed");
                }
                logger::warn("AmbushBeat: proceeding with {}/{} attacker(s) still reading unsettled after "
                             "relocation",
                             result.unsettled,
                             result.checked);
                SetSubPhase(ComposeSubPhase::Arming);
                return {};
            }

            case ComposeSubPhase::Arming: {
                int expected = 0;
                std::vector<RE::NiPoint3> fallbacks;
                {
                    std::scoped_lock lock(g_stateMutex);
                    expected = g_activeCount;
                    fallbacks = g_spawnFallbacks;
                }
                const int armed = MainThread::Run(pt, [expected, &fallbacks](const MainThread::Token&) {
                    // Escort starts here, at the same moment the
                    // travel packages do: everything before this is
                    // COMPOSE placing actors, and an actor that
                    // hasn't been told to walk yet isn't stuck.
                    g_escort.Begin(std::move(fallbacks));

                    int n = 0;
                    for (int i = 0; i < expected && i < kSlotCount; ++i) {
                        auto* actor = AttackerInSlot(i);
                        if (!actor) {
                            continue;
                        }
                        // Aggression 0 so the travel package keeps
                        // control the whole way in. The handoff to
                        // hostility happens at close range in RUNNING.
                        actor->AsActorValueOwner()->SetActorValue(RE::ActorValue::kAggression, 0.0f);
                        actor->EvaluatePackage();
                        // Baseline is where it actually IS, not where
                        // it was asked to spawn — physics has already
                        // had its say by now.
                        g_escort.Track(actor, actor->GetPosition());
                        ++n;
                    }
                    if (g_ambushQuest) {
                        QuestUtils::VMDispatchQuestSetStage(g_ambushQuest, kStageEngaged);
                    }
                    return n;
                });

                if (armed == 0) {
                    MainThread::Run(pt, [](const MainThread::Token&) {
                        DeleteCreatedRefs("nothing armed");
                        return 0;
                    });
                    return FailCompose("nothing_armed");
                }
                g_composeSucceeded.store(true, std::memory_order_release);
                logger::info("AmbushBeat: armed {} of {} attacker(s); COMPOSE complete", armed, expected);

                // Put this group on its cooldown. Stamped here rather
                // than at OnStart or CLEANUP because this is the first
                // point where attackers demonstrably exist in the world
                // — a compose that failed anywhere earlier must not
                // retire the group it was going to use.
                {
                    std::string usedGroup;
                    {
                        std::scoped_lock lock(g_stateMutex);
                        usedGroup = g_activeGroupId;
                    }
                    AmbushAttackerGroups::StampGroupUsed(usedGroup);
                }
                {
                    std::scoped_lock lock(g_stateMutex);
                    g_activeCount = armed;
                }
                return TickResult{BeatState::RUNNING};
            }

            case ComposeSubPhase::Failed:
            default:
                return TickResult{BeatState::CLEANUP};
            }
        }

        // ---- RUNNING -------------------------------------------------

        // Which attacker tripped the handoff, and at what range. slot is
        // -1 while nobody has closed yet.
        struct EngageTrigger
        {
            int slot = -1;
            float dist = 0.0f;
        };

        // The moment ANY attacker is inside the engage radius, every
        // surviving attacker goes to aggression 2 and is put into combat
        // with the player.
        //
        // Whole-group rather than per-attacker because the group arrives
        // strung out. On each member's own crossing, stragglers would
        // approach a fight that had already started and the player's
        // combat state would hang on whichever attacker was in front.
        //
        // The scan runs here on the plugin thread — alias reads,
        // GetPosition and IsDead are all off-main-safe per
        // docs/MAIN_THREAD_STUTTER_AUDIT.md. Only the two mutations need
        // the main thread, and neither returns anything worth waiting
        // for, so each attacker gets its own FireAndForget. The slot
        // index is captured rather than the actor pointer: the task runs
        // later, so it re-resolves through the alias.
        EngageTrigger TryEngageGroup(const PluginThread::Token& pt, int expected, float engageDist)
        {
            EngageTrigger trigger;
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                return trigger;
            }
            const auto playerPos = player->GetPosition();

            for (int i = 0; i < expected && i < kSlotCount; ++i) {
                auto* actor = AttackerInSlot(i);
                if (!actor || actor->IsDead()) {
                    continue;
                }
                const float dist = actor->GetPosition().GetDistance(playerPos);
                if (dist <= engageDist) {
                    trigger.slot = i;
                    trigger.dist = dist;
                    break;
                }
            }
            if (trigger.slot < 0) {
                return trigger;
            }

            for (int i = 0; i < expected && i < kSlotCount; ++i) {
                MainThread::FireAndForget(pt, [i](const MainThread::Token&) {
                    auto* actor = AttackerInSlot(i);
                    if (!actor || actor->IsDead()) {
                        return;
                    }
                    actor->AsActorValueOwner()->SetActorValue(RE::ActorValue::kAggression, 2.0f);
                    // Actor::StartCombat has no CommonLibSSE-NG binding —
                    // only StopCombat is exposed — so this one stays a VM
                    // call out to _ne_AmbushQuest.EngageAttacker.
                    QuestUtils::VMDispatchOnQuest(
                        g_ambushQuest, kQuestScriptName, "EngageAttacker", static_cast<std::int32_t>(i));
                });
            }
            return trigger;
        }

        TickResult TickRunning(const PluginThread::Token& pt)
        {
            const auto& cfg = Settings::Get();

            // Stale-state self-validation, once per load. BeatSystem's
            // "unknown beat -> reset to idle" recovery does not fire for
            // a beat that is still registered, so a cosave can restore
            // BEAT_RUNNING/RUNNING with no world state behind it. This
            // is also the recovery path after a crash or a forced abort.
            if (!g_staleCheckDone.exchange(true, std::memory_order_acq_rel)) {
                const bool sane = MainThread::Run(pt, [](const MainThread::Token&) {
                    if (!g_ambushQuest || g_ambushQuest->GetCurrentStageID() != kStageEngaged) {
                        return false;
                    }
                    for (int i = 0; i < kSlotCount; ++i) {
                        if (RefInSlot(i)) {
                            return true;
                        }
                    }
                    return false;
                });
                if (!sane) {
                    logger::warn("AmbushBeat: RUNNING state has no world behind it (quest not at stage "
                                 "{} or no attacker alias filled) — recovering via CLEANUP",
                                 kStageEngaged);
                    g_composeSucceeded.store(false, std::memory_order_release);
                    return TickResult{BeatState::CLEANUP};
                }
            }

            // Narration fires the moment the PLAYER is actually in
            // combat — not at spawn, and not when the attackers merely
            // engage. The prose says who is jumping the player and why,
            // so it should land when the fight is real to them rather
            // than while a group is still jogging over the hill.
            //
            // Checked every tick rather than inside the 5 s poll gate
            // below: it costs one bool read, and the log entry should be
            // roughly contemporaneous with the fight starting.
            if (!g_narrationFired.load(std::memory_order_acquire) && EngineUtils::IsPlayerInCombat()) {
                std::string prose;
                {
                    std::scoped_lock lock(g_stateMutex);
                    prose = g_narrationProse;
                }
                if (prose.empty()) {
                    // Nothing to say; latch anyway so we stop checking.
                    g_narrationFired.store(true, std::memory_order_release);
                } else if (!g_narrationFired.exchange(true, std::memory_order_acq_rel)) {
                    MainThread::Run(pt, [prose = std::move(prose)](const MainThread::Token&) {
                        QuestUtils::VMDispatchOnQuest(
                            g_ambushQuest, kQuestScriptName, "RunAmbushNarration", RE::BSFixedString(prose.c_str()));
                        return 0;
                    });
                    logger::info("AmbushBeat: player entered combat — submitted ambush narration");
                }
            }

            // Tick-driven accumulator: no wall-clock timer of this beat's
            // own. The master poll's interval is the unit of time here.
            const double pollInterval = static_cast<double>(Settings::Get().beatSystemPollIntervalMs) / 1000.0;
            const double elapsed =
                g_runningElapsedSeconds.fetch_add(pollInterval, std::memory_order_acq_rel) + pollInterval;
            const double sinceLastPoll =
                g_runningPollAccumulator.fetch_add(pollInterval, std::memory_order_acq_rel) + pollInterval;

            int expected = 0;
            {
                std::scoped_lock lock(g_stateMutex);
                expected = g_activeCount;
            }

            if (!g_groupEngaged.load(std::memory_order_acquire)) {
                const double sinceEngageCheck =
                    g_engageCheckAccumulator.fetch_add(pollInterval, std::memory_order_acq_rel) + pollInterval;
                if (sinceEngageCheck >= kEngageCheckSeconds) {
                    g_engageCheckAccumulator.store(0.0, std::memory_order_release);
                    const auto trigger =
                        TryEngageGroup(pt, expected, static_cast<float>(cfg.ambushEngageDistanceUnits));
                    if (trigger.slot >= 0) {
                        g_groupEngaged.store(true, std::memory_order_release);
                        logger::info("AmbushBeat: slot {} closed to {:.0f}u — engaged all {} attacker(s)",
                                     trigger.slot,
                                     trigger.dist,
                                     expected);
                    }
                }
            }

            // Escort clock advances every tick, ahead of the completion
            // poll's gate, so its cadence is StuckRecovery's own setting
            // and not a multiple of this beat's much slower poll. The
            // clock is plugin-thread arithmetic — only the check it
            // gates costs a main-thread hop.
            StuckRecovery::Options escortOpts;
            escortOpts.movementThresholdUnits = static_cast<float>(cfg.stuckRecoveryMovementThresholdUnits);
            escortOpts.checkIntervalSeconds = static_cast<double>(cfg.stuckRecoveryCheckIntervalSeconds);

            if (g_escort.DueForCheck(pollInterval, escortOpts)) {
                MainThread::Run(pt, [expected, &escortOpts](const MainThread::Token& mt) {
                    auto* player = RE::PlayerCharacter::GetSingleton();
                    if (!player) {
                        return 0;
                    }
                    const auto playerPos = player->GetPosition();
                    for (int i = 0; i < expected && i < kSlotCount; ++i) {
                        auto* actor = AttackerInSlot(i);
                        if (!actor || actor->IsDead()) {
                            continue;
                        }
                        // StuckRecovery logs each warp itself, so
                        // nothing is counted here.
                        g_escort.Update(mt, actor, playerPos, escortOpts);
                    }
                    return 0;
                });
            }

            if (sinceLastPoll < kRunningPollSeconds) {
                return {};
            }
            g_runningPollAccumulator.store(0.0, std::memory_order_release);

            if (elapsed >= static_cast<double>(cfg.ambushMaxDurationSeconds)) {
                logger::info("AmbushBeat: abandoned by timeout after {:.0f}s", elapsed);
                return TickResult{BeatState::CLEANUP};
            }

            struct PollResult
            {
                int alive = 0;
                int deadOrGone = 0;
                int beyondAbandon = 0;
            };

            const auto poll = MainThread::Run(pt, [expected, &cfg](const MainThread::Token&) {
                PollResult r;
                auto* player = RE::PlayerCharacter::GetSingleton();
                if (!player) {
                    return r;
                }
                const auto playerPos = player->GetPosition();

                for (int i = 0; i < expected && i < kSlotCount; ++i) {
                    auto* ref = RefInSlot(i);
                    auto* actor = AttackerInSlot(i);
                    // "Gone" — the alias is filled but the reference no
                    // longer resolves (another mod deleted it, say).
                    // Counts as dead for completion purposes; it must
                    // not stall the all-dead check forever.
                    if (!ref || !actor || actor->IsDead()) {
                        ++r.deadOrGone;
                        if (actor) {
                            g_escort.Forget(actor->GetFormID());
                        }
                        continue;
                    }
                    ++r.alive;

                    const auto pos = actor->GetPosition();
                    const float dx = pos.x - playerPos.x;
                    const float dy = pos.y - playerPos.y;
                    const float dz = pos.z - playerPos.z;
                    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);

                    if (dist > static_cast<float>(cfg.ambushAbandonDistanceUnits)) {
                        ++r.beyondAbandon;
                    }
                }
                return r;
            });

            if (poll.alive == 0) {
                logger::info("AmbushBeat: all {} attacker(s) down — encounter complete", expected);
                return TickResult{BeatState::CLEANUP};
            }
            if (poll.beyondAbandon == poll.alive) {
                logger::info("AmbushBeat: player outran the encounter ({} survivor(s) beyond {}u)",
                             poll.alive,
                             cfg.ambushAbandonDistanceUnits);
                return TickResult{BeatState::CLEANUP};
            }

            if (cfg.debugMode) {
                logger::debug("AmbushBeat: poll — alive={} down={} beyondAbandon={} elapsed={:.0f}s",
                              poll.alive,
                              poll.deadOrGone,
                              poll.beyondAbandon,
                              elapsed);
            }
            return {};
        }
    } // namespace

    // ---------------------------------------------------------------------
    // IBeat implementation
    // ---------------------------------------------------------------------

    std::string AmbushBeat::Name() const
    {
        return "ambush";
    }

    BeatPolarity AmbushBeat::Polarity() const
    {
        return BeatPolarity::Raise;
    }

    std::string AmbushBeat::Description() const
    {
        return "A group of hostile NPCs ambushes the player in the open world. They appear out of sight "
               "at a distance, close on the player on foot with weapons drawn, and turn hostile at short "
               "range, resolving into ordinary combat.\n"
               "\n"
               "Fits when the story wants sudden physical danger: tension has been building with no "
               "outlet, the player is travelling through open country, or someone they have wronged has "
               "had time to send people. Works best when the chosen group has a specific reason to want "
               "this player in particular.\n"
               "\n"
               "Does NOT fit when the player is somewhere an ambush reads as absurd (a city, an inn, a "
               "guarded camp), when they are already in a fight, or when the scene calls for reflection "
               "rather than escalation. This beat only ever raises tension; it cannot resolve anything.\n"
               "\n"
               "Parameters:\n"
               "  attacker_group (REQUIRED, string) — MUST be one of the `id` values listed in this "
               "candidate's ambush_attacker_candidates. Do not invent an id, do not use a display name, "
               "and do not reuse an id from a previous turn: the list is recomputed from current game "
               "state every time, and an id that is not on it right now will be rejected.\n"
               "  attacker_count (REQUIRED, integer) — how many attackers appear. Clamped to the "
               "configured range. Larger groups read as an organized operation; two or three read as "
               "opportunists.\n"
               "  narration_prose (REQUIRED, string) — one to three sentences of in-world prose stating WHO "
               "is attacking and WHAT their motivation is. Refer to the player by the name given under "
               "'Where the player is', exactly as you would in parameter_justification — never as 'the "
               "player', and never by an epithet such as 'the intruder' or 'the outsider'. Written as "
               "narration, not as an explanation of your choice: describe the attackers and their grievance "
               "the way a chronicler would. It is recorded in the world's event log the moment the fight "
               "begins, so it must read as a statement of what is happening rather than as commentary "
               "about the decision.";
    }

    double AmbushBeat::RemainingCooldownGameHours() const
    {
        const auto& cfg = Settings::Get();
        if (cfg.ambushPerBeatCooldownGameHours <= 0) {
            return 0.0;
        }
        double last = 0.0;
        {
            std::scoped_lock lock(g_cooldownMutex);
            last = g_lastCompletionGameHours;
        }
        if (last <= 0.0) {
            return 0.0;
        }
        const double elapsed = EngineUtils::GetCurrentGameHours() - last;
        const double remaining = static_cast<double>(cfg.ambushPerBeatCooldownGameHours) - elapsed;
        return remaining > 0.0 ? remaining : 0.0;
    }

    bool AmbushBeat::IsAvailable(const BeatContext& ctx) const
    {
        if (!g_pointersResolved.load(std::memory_order_acquire) || !g_ambushQuest) {
            return false;
        }
        // Exterior only: the beat spawns a travelling approach, and
        // interiors have neither the room nor the sightlines.
        // Outdoors, on the road or in wilderness — nowhere else. This is
        // the pre-LLM half of the gate: failing here keeps `ambush` out
        // of the candidate list entirely, so the Director is never
        // offered a beat it couldn't legally run. COMPOSE re-checks,
        // because the player can walk into a cave while the request is
        // in flight.
        auto* pc = RE::PlayerCharacter::GetSingleton();
        if (AmbushLocationBlocker(ctx.playerInInterior, pc ? pc->GetCurrentLocation() : nullptr)) {
            return false;
        }
        // Deliberately no check on the quest stage here. A live ambush is
        // already blocked upstream by BeatSystem's in_flight gate, which
        // runs before candidates are gathered — so by the time this is
        // reached, an in-flight stage can only be LEFTOVER: the player
        // died mid-encounter and reloaded a save from before it, or the
        // session crashed. Treating that as busy would make the beat
        // permanently unavailable for the rest of the session, since
        // nothing outside COMPOSE winds the stage back. COMPOSE's
        // StartingQuest opens with RetireQuest for exactly this case.
        if (Settings::Get().debugMode) {
            if (const auto stage = g_ambushQuest->GetCurrentStageID();
                stage == kStageSpawning || stage == kStageEngaged) {
                logger::debug("AmbushBeat: quest left at stage {} by a previous session — "
                              "COMPOSE will retire it before starting",
                              stage);
            }
        }
        if (RemainingCooldownGameHours() > 0.0) {
            return false;
        }
        // No eligible attackers means nothing to spawn. This also
        // covers the missing-group-file case, where zero groups loaded.
        if (AmbushAttackerGroups::EnabledGroupCount() == 0) {
            return false;
        }
        return true;
    }

    void AmbushBeat::OnStart(const BeatContext&, const nlohmann::json& parameters)
    {
        // Plugin thread — param parse and session-state reset only. No
        // engine access here, per IBeat's contract.
        ResetSessionState();

        const auto& cfg = Settings::Get();
        const int count = JsonUtils::ClampParameterInt(parameters,
                                                       "attacker_count",
                                                       cfg.ambushDefaultAttackerCount,
                                                       cfg.ambushMinAttackerCount,
                                                       cfg.ambushMaxAttackerCount);
        // Spawn distance is settings-driven, not an LLM parameter —
        // staging isn't something the model has a useful view of.
        const int distance = cfg.ambushDefaultSpawnDistanceUnits;

        // Free-form LLM strings — sanitize at the extraction site, per
        // docs/LLM_RESPONSE_HANDLING.md.
        std::string group;
        if (parameters.contains("attacker_group") && parameters["attacker_group"].is_string()) {
            group = LLMTextSanitizer::Sanitize(parameters["attacker_group"].get<std::string>());
        }
        std::string narration;
        if (parameters.contains("narration_prose") && parameters["narration_prose"].is_string()) {
            narration = LLMTextSanitizer::Sanitize(parameters["narration_prose"].get<std::string>());
        }

        {
            std::scoped_lock lock(g_stateMutex);
            g_requestedGroupId = group;
            g_requestedCount = count;
            g_spawnDistanceUnits = distance;
            g_narrationProse = narration;
        }

        logger::info("AmbushBeat: OnStart — group='{}' count={} distance={} (from settings) narration={}",
                     group.empty() ? "(unspecified)" : group.c_str(),
                     count,
                     distance,
                     narration.empty() ? "(none)" : "present");
        if (parameters.contains("spawn_distance_units")) {
            logger::debug("AmbushBeat: ignoring spawn_distance_units from the Director — spawn distance is "
                          "settings-driven and the parameter is not offered");
        }
    }

    TickResult AmbushBeat::Tick(const PluginThread::Token& pt, TickMode mode, BeatState state)
    {
        // Freeze only when paused — deliberately unlike the social
        // beats, which also freeze on Combat. An ambush *creates* combat,
        // so freezing on it strands spawned-but-unarmed attackers
        // mid-COMPOSE. Dialogue passes for the same reason: once
        // references exist, half-built state is the worse outcome.
        if (mode == TickMode::Paused) {
            return {};
        }

        switch (state) {
        case BeatState::COMPOSE:
            return TickCompose(pt);
        case BeatState::RUNNING:
            return TickRunning(pt);
        case BeatState::CLEANUP: {
            const bool success = g_composeSucceeded.load(std::memory_order_acquire);
            MainThread::Run(pt, [success](const MainThread::Token& mt) { Cleanup(mt, success); });
            ResetSessionState();
            return TickResult{BeatState::NOT_RUNNING};
        }
        case BeatState::NOT_RUNNING:
        default:
            return {};
        }
    }

    void AmbushBeat::Abort(const MainThread::Token& mt)
    {
        logger::warn("AmbushBeat: Abort() invoked — running terminal cleanup");
        // CLEANUP minus the cooldown stamp, plus the retire CLEANUP
        // defers: Abort's contract is that world-side effects are gone
        // when it returns, corpses included.
        Cleanup(mt, /*stampCooldown=*/false);
        RetireQuest("abort");
        ResetSessionState();
    }

    // ---------------------------------------------------------------------
    // AmbushBeat_Init
    // ---------------------------------------------------------------------

    namespace AmbushBeat_Init
    {
        void Initialize()
        {
            if (g_pointersResolved.exchange(true)) {
                return;
            }

            if (auto* form = RE::TESForm::LookupByEditorID(kAmbushQuestEditorID)) {
                g_ambushQuest = form->As<RE::TESQuest>();
            }
            if (!g_ambushQuest) {
                logger::error("AmbushBeat_Init: quest '{}' did not resolve — the ambush beat is "
                              "disabled for this session",
                              kAmbushQuestEditorID);
                return;
            }

            int bound = 0;
            for (auto* a : g_ambushQuest->aliases) {
                if (!a) {
                    continue;
                }
                const std::string_view name{a->aliasName.c_str()};
                if (name == kPlayerRefAliasName) {
                    g_playerAlias = skyrim_cast<RE::BGSRefAlias*>(a);
                    continue;
                }
                // Attacker01 .. Attacker08 -> slots 0 .. 7.
                if (name.rfind("Attacker", 0) == 0 && name.size() == 10) {
                    const char d0 = name[8];
                    const char d1 = name[9];
                    if (d0 >= '0' && d0 <= '9' && d1 >= '0' && d1 <= '9') {
                        const int index = (d0 - '0') * 10 + (d1 - '0') - 1;
                        if (index >= 0 && index < kSlotCount) {
                            g_attackerAliases[static_cast<std::size_t>(index)] = skyrim_cast<RE::BGSRefAlias*>(a);
                            ++bound;
                        }
                    }
                }
            }

            if (!g_playerAlias || bound != kSlotCount) {
                logger::error("AmbushBeat_Init: alias binding incomplete (PlayerRef={}, attackers {}/{}) "
                              "— the ESP and the plugin disagree about _ne_AmbushQuest's alias layout",
                              g_playerAlias ? "ok" : "MISSING",
                              bound,
                              kSlotCount);
                return;
            }

            logger::info("AmbushBeat_Init: resolved quest=0x{:08X} with {} attacker aliases bound",
                         g_ambushQuest->GetFormID(),
                         bound);
        }
    } // namespace AmbushBeat_Init

    // ---------------------------------------------------------------------
    // AmbushBeat_Persistence
    // ---------------------------------------------------------------------

    namespace AmbushBeat_Persistence
    {
        // v2 appended the per-group cooldown table. v1 records still
        // load; they simply carry no cooldowns, which reads as "every
        // group is ready" — the safe direction for a one-time upgrade.
        constexpr std::uint32_t kRecordVersion = 2;

        void OnSave(SKSE::SerializationInterface* intfc)
        {
            if (!intfc) {
                return;
            }
            if (!intfc->OpenRecord(kRecordTypeId, kRecordVersion)) {
                logger::error("AmbushBeat::OnSave: OpenRecord failed");
                return;
            }
            double last = 0.0;
            {
                std::scoped_lock lock(g_cooldownMutex);
                last = g_lastCompletionGameHours;
            }
            intfc->WriteRecordData(last);

            // The chosen group id, so a post-reload log line can still
            // name what ambushed the player. Deliberately the only other
            // thing here: the quest stage IS the phase and the aliases
            // ARE the roster, so neither needs mirroring into the cosave.
            std::string groupId;
            {
                std::scoped_lock lock(g_stateMutex);
                groupId = g_activeGroupId;
            }
            const auto len = static_cast<std::uint32_t>(groupId.size());
            intfc->WriteRecordData(len);
            if (len > 0) {
                intfc->WriteRecordData(groupId.data(), len);
            }

            // Per-group cooldowns live with the group table but ride in
            // this beat's record — they're beat state, not config.
            AmbushAttackerGroups::SerializeCooldowns(intfc);
        }

        void OnLoad(SKSE::SerializationInterface* intfc, std::uint32_t version, std::uint32_t length)
        {
            if (!intfc) {
                return;
            }
            if (version != 1 && version != kRecordVersion) {
                logger::warn("AmbushBeat::OnLoad: unknown version {} (length={}); clearing", version, length);
                OnRevert();
                return;
            }
            double last = 0.0;
            if (intfc->ReadRecordData(last) != sizeof(last)) {
                logger::error("AmbushBeat::OnLoad: short read on lastCompletionGameHours; clearing");
                OnRevert();
                return;
            }

            // Read the id length separately from the id: a failure here
            // leaves the stream misaligned, so the cooldown table that
            // follows must not be attempted.
            std::uint32_t len = 0;
            std::string groupId;
            bool streamOk = intfc->ReadRecordData(len) == sizeof(len);
            if (streamOk && len > 0) {
                if (len < 4096) {
                    groupId.resize(len);
                    streamOk = intfc->ReadRecordData(groupId.data(), len) == len;
                    if (!streamOk) {
                        groupId.clear();
                    }
                } else {
                    streamOk = false;
                }
            }

            AmbushAttackerGroups::ClearCooldowns();
            if (version >= 2 && streamOk) {
                if (!AmbushAttackerGroups::DeserializeCooldowns(intfc)) {
                    logger::error("AmbushBeat::OnLoad: per-group cooldown deserialize failed; cleared");
                    AmbushAttackerGroups::ClearCooldowns();
                }
            }

            {
                std::scoped_lock lock(g_cooldownMutex);
                g_lastCompletionGameHours = last;
            }
            {
                std::scoped_lock lock(g_stateMutex);
                g_activeGroupId = groupId;
            }
            logger::info("AmbushBeat::OnLoad: restored lastCompletionGameHours={:.2f}, group='{}' (record v{})",
                         last,
                         groupId,
                         version);
        }

        void OnRevert()
        {
            {
                std::scoped_lock lock(g_cooldownMutex);
                g_lastCompletionGameHours = 0.0;
            }
            AmbushAttackerGroups::ClearCooldowns();
            ResetSessionState();
        }
    } // namespace AmbushBeat_Persistence
} // namespace NarrativeEngine
