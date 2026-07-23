#include <AliasWalkFilter.h>

#include <logger.h>
#include <Settings.h>

#include <RE/B/BGSBaseAlias.h>
#include <RE/B/BGSScene.h>
#include <RE/B/BSAtomic.h>
#include <RE/E/ExtraAliasInstanceArray.h>
#include <RE/T/TESFile.h>
#include <RE/T/TESQuest.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace NarrativeEngine::AliasWalkFilter
{
    namespace
    {
        // Our own ESP name. Any quest whose origin file matches this
        // is self-excluded from the "spoken-for" check — otherwise a
        // sender who's currently mid-visit would flag themselves via
        // the visit quest's own alias fill.
        //
        // Case-insensitive compare (Skyrim's path handling is not
        // case-consistent across systems). Stored as a bare `const
        // char*` — the compare hits the field directly, no
        // BSFixedString allocation.
        constexpr const char* kOwnPluginFile = "NarrativeEngine.esp";

        // True iff `quest` was authored in our own ESP. Cheap file-name
        // compare so we don't have to enumerate every NE quest
        // EditorID (visit / letter / any future actions).
        bool IsOwnQuest(const RE::TESQuest* quest)
        {
            if (!quest)
                return false;
            const auto* file = quest->GetFile(0);
            if (!file)
                return false;
            // TESFile::fileName is a fixed-size char array; direct
            // stricmp is safe. Length-cap defensively at
            // sizeof(fileName) in case the array isn't null-terminated
            // for some absurd reason.
            return _strnicmp(file->fileName, kOwnPluginFile, sizeof(file->fileName)) == 0;
        }

        // "Running-enough" test for a quest referenced by an alias
        // instance. TESQuest::IsRunning is unreliable on its own (it
        // shares the kEnabled bit with IsEnabled, so it returns true
        // for never-started-but-enabled quests). Combine with
        // IsStopped/IsCompleted to filter down to "actually active
        // right now". A quest that's stopped or completed shouldn't
        // still have an actor filled into its aliases in the normal
        // flow — but ExtraAliasInstanceArray entries can occasionally
        // linger past a Reset(), and we want to reject those.
        bool IsForeignQuestRunning(const RE::TESQuest* quest)
        {
            if (!quest)
                return false;
            if (quest->IsStopped())
                return false;
            if (quest->IsCompleted())
                return false;
            // Belt-and-braces: still require IsRunning. Combined with
            // the two negative checks above this converges to "quest
            // is genuinely active".
            return quest->IsRunning();
        }
    } // namespace

    bool IsActorStoryActive(RE::Actor* actor, std::string* reasonOut, bool debug)
    {
        if (!actor)
            return false;

        const auto formId = actor->GetFormID();

        // Scene participation is the cheapest, cleanest signal: if
        // the actor is mid-scene, whoever authored that scene is
        // driving them right now. No alias walk needed.
        if (auto* scene = actor->GetCurrentScene()) {
            const auto* sceneQuest = scene->parentQuest;
            if (!IsOwnQuest(sceneQuest)) {
                if (debug) {
                    logger::debug("AliasWalkFilter: actor 0x{:X} is in scene 0x{:X} "
                                  "owned by quest '{}' (0x{:X}) — story-active",
                                  formId,
                                  scene->GetFormID(),
                                  sceneQuest ? sceneQuest->GetFormEditorID() : "?",
                                  sceneQuest ? sceneQuest->GetFormID() : 0u);
                }
                if (reasonOut) {
                    char buf[96];
                    std::snprintf(buf, sizeof(buf), "scene:0x%X", scene->GetFormID());
                    *reasonOut = buf;
                }
                return true;
            }
            if (debug) {
                logger::debug("AliasWalkFilter: actor 0x{:X} in scene 0x{:X} but "
                              "owning quest is our own ESP — self-exclude, keep walking",
                              formId,
                              scene->GetFormID());
            }
        }

        // Alias-instance walk. TESObjectREFR::extraList carries an
        // ExtraAliasInstanceArray listing every (quest, alias) pair
        // the ref is currently plugged into, plus (for BGSRefAlias)
        // the array of AI packages that alias contributes.
        auto* aliasArray = actor->extraList.GetByType<RE::ExtraAliasInstanceArray>();
        if (!aliasArray) {
            if (debug) {
                logger::debug("AliasWalkFilter: actor 0x{:X} has no "
                              "ExtraAliasInstanceArray — not story-active",
                              formId);
            }
            return false;
        }

        RE::BSReadLockGuard lock(aliasArray->lock);

        const std::size_t n = aliasArray->aliases.size();
        // Aggregate diagnostics — walked at the end to give a single
        // summary line per actor. Independent of the story-active
        // decision; useful for post-mortem debugging of alias-fill
        // failures downstream.
        int foreignRunningReservedCount = 0;
        int foreignRunningQuestObjectCount = 0;
        int foreignRunningTotal = 0;
        std::string firstReserverTag;
        if (debug) {
            logger::debug("AliasWalkFilter: actor 0x{:X} — walking {} alias entries", formId, n);
        }

        // Decode helper for BGSBaseAlias::FLAGS bits we care about
        // when diagnosing "why can't this actor be filled into our
        // Sender alias." Written into a compact tag string that
        // appears on each per-entry line and in the walk summary.
        auto decodeFlags = [](const RE::BGSBaseAlias* alias) -> std::string {
            using F = RE::BGSBaseAlias::FLAGS;
            std::string out;
            if (!alias)
                return out;
            const auto& fl = alias->flags;
            auto add = [&](const char* tag) {
                if (!out.empty())
                    out.push_back('|');
                out.append(tag);
            };
            if (fl.any(F::kReserves))
                add("RESERVES");
            if (fl.any(F::kAllowReserved))
                add("allowReserved");
            if (fl.any(F::kQuestObject))
                add("QUESTOBJ");
            if (fl.any(F::kOptional))
                add("optional");
            if (fl.any(F::kEssential))
                add("essential");
            if (fl.any(F::kProtected))
                add("protected");
            if (fl.any(F::kAllowDead))
                add("allowDead");
            if (fl.any(F::kAllowDisabled))
                add("allowDisabled");
            if (fl.any(F::kLoadedOnly))
                add("loadedOnly");
            return out;
        };

        for (std::size_t i = 0; i < n; ++i) {
            const auto* entry = aliasArray->aliases[i];
            if (!entry)
                continue;

            const auto* quest = entry->quest;
            const auto* alias = entry->alias;

            const char* qEid = (quest && quest->GetFormEditorID()) ? quest->GetFormEditorID() : "?";
            const std::uint32_t qId = quest ? quest->GetFormID() : 0u;
            const char* aName = (alias && !alias->aliasName.empty()) ? alias->aliasName.c_str() : "?";
            const std::uint32_t aId = alias ? alias->aliasID : 0u;
            const std::size_t pkgN = (entry->instancedPackages) ? entry->instancedPackages->size() : 0u;
            const std::uint32_t rawFlags = alias ? alias->flags.underlying() : 0u;
            const std::string decoded = decodeFlags(alias);

            if (IsOwnQuest(quest)) {
                if (Settings::Get().traceMode) {
                    logger::trace("AliasWalkFilter:  [{}] quest='{}' (0x{:X}) alias='{}' "
                                  "(id={}) flags=0x{:X}[{}] — SELF (our ESP), skip",
                                  i,
                                  qEid,
                                  qId,
                                  aName,
                                  aId,
                                  rawFlags,
                                  decoded);
                }
                continue;
            }

            if (!IsForeignQuestRunning(quest)) {
                if (Settings::Get().traceMode) {
                    logger::trace("AliasWalkFilter:  [{}] quest='{}' (0x{:X}) alias='{}' "
                                  "(id={}) flags=0x{:X}[{}] — quest not running, skip",
                                  i,
                                  qEid,
                                  qId,
                                  aName,
                                  aId,
                                  rawFlags,
                                  decoded);
                }
                continue;
            }

            // Track diagnostic aggregates on every foreign-running
            // entry — needed for the summary regardless of whether
            // this specific entry ends up returning story-active.
            ++foreignRunningTotal;
            const bool aliasReserves = alias && alias->flags.any(RE::BGSBaseAlias::FLAGS::kReserves);
            const bool aliasQuestObj = alias && alias->flags.any(RE::BGSBaseAlias::FLAGS::kQuestObject);
            if (aliasReserves) {
                ++foreignRunningReservedCount;
                if (firstReserverTag.empty()) {
                    char buf[128];
                    std::snprintf(buf, sizeof(buf), "%s(0x%X)/%s(id=%u)", qEid, qId, aName, aId);
                    firstReserverTag = buf;
                }
            }
            if (aliasQuestObj)
                ++foreignRunningQuestObjectCount;

            // Reservation gate. A foreign quest holding this ref
            // with the kReserves flag will cause Skyrim's alias-fill
            // machinery to skip the ref during our Sender FMR (unless
            // our alias has kAllowReserved, which it doesn't). That
            // manifests as the exact "EnsureQuestStarted returns false
            // with all three aliases unfilled" failure we've seen
            // repeatedly on Brelyna (held by DialogueWinterholdCollege's
            // Jzargo alias). Reject the candidate upstream so the LLM
            // never picks them and we never dispatch a doomed fill.
            //
            // We reject even when pkgN == 0 — Skyrim's fill-skip logic
            // doesn't care whether the reserving alias dispenses a
            // package, only whether it's flagged kReserves. A tracking
            // fill with kReserves blocks us just as thoroughly as a
            // package-dispensing one.
            if (aliasReserves) {
                if (Settings::Get().traceMode) {
                    logger::trace("AliasWalkFilter:  [{}] quest='{}' (0x{:X}) alias='{}' "
                                  "(id={}) flags=0x{:X}[{}] — RESERVED (kReserves), skip "
                                  "candidate",
                                  i,
                                  qEid,
                                  qId,
                                  aName,
                                  aId,
                                  rawFlags,
                                  decoded);
                }
                if (reasonOut) {
                    char buf[160];
                    std::snprintf(buf, sizeof(buf), "reserved-by:%s(0x%X)/%s(id=%u)", qEid, qId, aName, aId);
                    *reasonOut = buf;
                }
                return true;
            }

            if (pkgN == 0) {
                // The quest is running and owns this actor via alias,
                // but the alias doesn't dispense a package to them.
                // Common for pure "tracking" fills — a radiant quest
                // may point at an NPC for a kill-target objective
                // without giving them any behavior. Not a story-active
                // signal on its own.
                if (Settings::Get().traceMode) {
                    logger::trace("AliasWalkFilter:  [{}] quest='{}' (0x{:X}) alias='{}' "
                                  "(id={}) flags=0x{:X}[{}] — running, no instanced "
                                  "packages, skip",
                                  i,
                                  qEid,
                                  qId,
                                  aName,
                                  aId,
                                  rawFlags,
                                  decoded);
                }
                continue;
            }

            // Foreign, running, has an alias-supplied package for
            // this actor. That's a scripted-behavior fill — leave the
            // actor alone.
            if (Settings::Get().traceMode) {
                logger::trace("AliasWalkFilter:  [{}] quest='{}' (0x{:X}) alias='{}' "
                              "(id={}) flags=0x{:X}[{}] pkgs={} — STORY-ACTIVE",
                              i,
                              qEid,
                              qId,
                              aName,
                              aId,
                              rawFlags,
                              decoded,
                              pkgN);
            }
            if (reasonOut) {
                char buf[160];
                std::snprintf(buf, sizeof(buf), "story-alias:%s(0x%X)/%s(id=%u)/pkgs=%zu", qEid, qId, aName, aId, pkgN);
                *reasonOut = buf;
            }
            return true;
        }

        // Walk-complete summary — always emitted at debug so we can
        // eyeball whether this actor was blocked by a reservation
        // or a quest-object hold even when the story-active check
        // returned false.
        if (debug) {
            const std::string reserverExample =
                firstReserverTag.empty() ? std::string{} : " (first: " + firstReserverTag + ")";
            logger::debug("AliasWalkFilter: actor 0x{:X} — walk complete, foreign_running={}"
                          ", reserved_entries={}{}, questObject_entries={}",
                          formId,
                          foreignRunningTotal,
                          foreignRunningReservedCount,
                          reserverExample,
                          foreignRunningQuestObjectCount);
        }

        return false;
    }
} // namespace NarrativeEngine::AliasWalkFilter
