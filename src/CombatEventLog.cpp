#include <CombatEventLog.h>

#include <EventLogUtil.h>
#include <logger.h>
#include <MainThread.h>
#include <Settings.h>
#include <SkyrimNetEvents.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace NarrativeEngine::CombatEventLog
{
    namespace
    {
        constexpr std::uint32_t kRecordVersion = 1;

        // Storage ring buffer cap. Overridable via Settings if the user adds
        // [CombatEvents] iMaxStored to NarrativeEngine.ini.
        constexpr std::size_t kDefaultMaxStored = 256;

        // Distance gate in engine units (~90 ft for 6000). Overridable via
        // Settings [CombatEvents] iHitRadiusUnits.
        constexpr float kDefaultHitRadiusUnits = 6000.0f;

        enum class Kind : std::uint8_t
        {
            CombatStart = 0,
            CombatEnd = 1,
            Hit = 2,
            Collapse = 3,
            RegainFooting = 4,
        };

        const char* KindName(Kind k)
        {
            switch (k) {
            case Kind::CombatStart:
                return "combat_start";
            case Kind::CombatEnd:
                return "combat_end";
            case Kind::Hit:
                return "hit";
            case Kind::Collapse:
                return "collapse";
            case Kind::RegainFooting:
                return "regain_footing";
            }
            return "unknown";
        }

        struct InternalEvent
        {
            Kind kind = Kind::Hit;
            double localTime = 0.0;        // Unix-epoch seconds
            double gameTime = 0.0;         // SkyrimNet-compatible time-of-day seconds
            std::string actorName;         // for Hit: damage-source label (may be empty for bare fallback)
            std::string targetName;        // unused for combat_start/end, collapse, regain_footing
            bool actorIsNamedActor = true; // false for environmental hits (poison, traps, ...)
        };

        std::mutex g_mutex;
        std::vector<InternalEvent> g_events;
        double g_currentEncounterStartRealTime = 0.0;
        std::unordered_map<RE::FormID, std::string> g_bleedingOut;

        // Session-only pending queue drained by EventHistoryWriter.
        // Stores raw events with the emit-time in-game timestamp;
        // DrainHistoryTail renders bodies at drain time.
        struct PendingHistoryItem
        {
            InternalEvent event;
            std::string inGameTimestamp;
        };
        std::vector<PendingHistoryItem> g_pendingHistory;

        // Tracks the player's IsInCombat as observed on the last Poll() call.
        // Flip from false → true emits combat_start; true → false emits
        // combat_end. Seeded in OnPostLoadGame, reset in OnRevert.
        bool g_playerInCombatLast = false;

        // Sink-registration tracking so Initialize() is idempotent.
        bool g_sinksRegistered = false;

        std::size_t MaxStored()
        {
            const int v = Settings::Get().combatEventsMaxStored;
            if (v <= 0)
                return kDefaultMaxStored;
            return static_cast<std::size_t>(v);
        }

        float HitRadiusUnits()
        {
            const int v = Settings::Get().combatEventsHitRadiusUnits;
            if (v <= 0)
                return kDefaultHitRadiusUnits;
            return static_cast<float>(v);
        }

        // Wrappers around the RE singletons so the sinks can be tested
        // logically without the engine in the loop. These all need to be
        // safe from non-main threads; the singletons themselves are.
        RE::Actor* GetPlayer()
        {
            return RE::PlayerCharacter::GetSingleton();
        }

        // Best-effort player position lookup. The engine updates this on the
        // main thread; reading it from a sink thread can race, but the
        // distance gate is fuzzy enough that meter-level jitter doesn't
        // matter. Returns a zero point if the player isn't available.
        RE::NiPoint3 PlayerPos()
        {
            if (auto* pc = GetPlayer()) {
                return pc->GetPosition();
            }
            return {0.0f, 0.0f, 0.0f};
        }

        bool WithinRadius(const RE::NiPoint3& p, float r)
        {
            return p.GetDistance(PlayerPos()) <= r;
        }

        // Resolve a usable display name for an actor reference. Returns
        // empty string if the actor is null, has no name, or has only an
        // empty string. Falling back to nothing here cascades into the
        // event being skipped (for targets / collapse actors) or to the
        // bare "X took damage" form (for environmental damage labels).
        std::string ActorDisplayName(RE::Actor* actor)
        {
            if (!actor)
                return {};
            const char* n = actor->GetDisplayFullName();
            if (!n || !*n)
                return {};
            return std::string(n);
        }

        std::string ActorDisplayName(RE::TESObjectREFR* ref)
        {
            if (!ref)
                return {};
            return ActorDisplayName(ref->As<RE::Actor>());
        }

        // -------- damage-source resolution -------------------------------
        // Cascade per PHASE_02_COMBAT_EVENTS.md "Damage source resolution":
        //   1. named-actor cause   → actor name
        //   2. identifiable source → "poison" / activator name / "a trap" /
        //                            "a spell trap"
        //   3. total fallback      → empty (renderer drops the "from ..."
        //                            clause)
        // The keyword/editor-ID lookups are deliberately small to start; they
        // can grow as we observe what actually shows up in logs.

        bool ContainsIgnoreCase(std::string_view haystack, std::string_view needle)
        {
            if (needle.empty())
                return true;
            const auto tolower = [](char c) -> char {
                return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
            };
            if (needle.size() > haystack.size())
                return false;
            for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
                bool match = true;
                for (std::size_t j = 0; j < needle.size(); ++j) {
                    if (tolower(haystack[i + j]) != tolower(needle[j])) {
                        match = false;
                        break;
                    }
                }
                if (match)
                    return true;
            }
            return false;
        }

        std::string ToLower(std::string s)
        {
            for (auto& c : s) {
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');
            }
            return s;
        }

        // Returns true if the magic item's name or any associated effect
        // hints at poison / venom. Cheap heuristic — the LLM doesn't care
        // about fidelity to the underlying spell name; "poison" is enough.
        bool MagicItemLooksLikePoison(RE::MagicItem* magic)
        {
            if (!magic)
                return false;
            if (const char* n = magic->GetFullName(); n && *n) {
                std::string_view name(n);
                if (ContainsIgnoreCase(name, "poison") || ContainsIgnoreCase(name, "venom")) {
                    return true;
                }
            }
            // Effect list pass — many poisons have unhelpful names like
            // "Frostbite Venom" or "Bandit Poison" but their effects are
            // canonically Damage Health w/ delivery=ingested.
            for (auto* effect : magic->effects) {
                if (!effect || !effect->baseEffect)
                    continue;
                if (const char* en = effect->baseEffect->GetFullName(); en && *en) {
                    std::string_view name(en);
                    if (ContainsIgnoreCase(name, "poison") || ContainsIgnoreCase(name, "venom")) {
                        return true;
                    }
                }
            }
            return false;
        }

        // Light pluralization for activator display names: if the name is
        // a single noun, lowercase it; otherwise pass through. The intent is
        // "boulders" / "spikes" — we keep it heuristic and accept that we'll
        // sometimes get "Falling Rocks Trap" verbatim, which is fine.
        std::string LowercaseActivatorLabel(const std::string& raw)
        {
            if (raw.empty())
                return raw;
            return ToLower(raw);
        }

        // Resolve a damage-source label for a hit event. Returns:
        //   { label, isNamedActor }
        // where label is "" only in the bare-fallback case.
        struct SourceLabel
        {
            std::string label;
            bool isNamedActor = false;
        };

        SourceLabel ResolveHitSource(const RE::TESHitEvent& evt)
        {
            // 1. Named actor cause.
            if (evt.cause) {
                if (auto name = ActorDisplayName(evt.cause.get()); !name.empty()) {
                    return {std::move(name), true};
                }
            }

            // 2. Identifiable form via event->source. source is a FormID
            // (weapon, spell, projectile-spawner depending on context). Look
            // it up and branch on the form type.
            if (evt.source != 0) {
                if (auto* form = RE::TESForm::LookupByID(evt.source)) {
                    if (auto* magic = form->As<RE::MagicItem>()) {
                        if (MagicItemLooksLikePoison(magic)) {
                            return {"poison", false};
                        }
                        return {"a spell trap", false};
                    }
                    if (auto* acti = form->As<RE::TESObjectACTI>()) {
                        if (const char* n = acti->GetFullName(); n && *n) {
                            return {LowercaseActivatorLabel(n), false};
                        }
                        return {"a trap", false};
                    }
                    // Weapon / explosion / etc. with no living wielder —
                    // typical of dart / swinging-blade / pressure-plate
                    // traps. The cause is null because it's the trap firing,
                    // not an actor.
                    if (form->As<RE::TESObjectWEAP>() || form->As<RE::BGSExplosion>()) {
                        return {"a trap", false};
                    }
                }
            }

            // 3. Total fallback.
            return {{}, false};
        }

        // ----------- storage helpers ------------------------------------

        void PushLocked(InternalEvent evt)
        {
            // Feed the history-writer's pending queue first. Timestamp
            // captured NOW so the log preserves emit-time ordering
            // regardless of when the writer's flush cadence runs.
            // Gated on the master switch so the queue can't grow
            // unbounded on long sessions with the writer off.
            if (Settings::Get().eventHistoryEnabled) {
                PendingHistoryItem item;
                item.event = evt;
                item.inGameTimestamp = EventLogUtil::CurrentInGameTimestamp();
                g_pendingHistory.push_back(std::move(item));
            }
            g_events.push_back(std::move(evt));
            while (g_events.size() > MaxStored()) {
                g_events.erase(g_events.begin());
            }
        }

        // Emit a regain_footing event for an actor that recovered. Caller
        // holds g_mutex.
        void EmitRegainFootingLocked(const std::string& name)
        {
            InternalEvent e;
            e.kind = Kind::RegainFooting;
            e.localTime = EventLogUtil::NowUnixSeconds();
            e.gameTime = EventLogUtil::NowGameTimeSeconds();
            e.actorName = name;
            e.actorIsNamedActor = true;
            PushLocked(std::move(e));
        }

        // ----------- rendered text --------------------------------------

        // Body-only rendering — same output as RenderText but without
        // the "[N ago]" prefix. Used by the history-writer drain path,
        // which prepends an absolute in-game timestamp instead.
        std::string RenderBody(const InternalEvent& e)
        {
            switch (e.kind) {
            case Kind::CombatStart:
                return e.actorName + " enters combat";
            case Kind::CombatEnd:
                return e.actorName + " leaves combat";
            case Kind::Hit:
                if (e.actorIsNamedActor) {
                    return e.actorName + " strikes " + e.targetName;
                }
                if (!e.actorName.empty()) {
                    return e.targetName + " took damage from " + e.actorName;
                }
                return e.targetName + " took damage";
            case Kind::Collapse:
                return e.actorName + " collapses";
            case Kind::RegainFooting:
                return e.actorName + " regains their footing";
            }
            return {};
        }

        std::string RenderText(const InternalEvent& e, double currentGameTimeSeconds)
        {
            const double delta = currentGameTimeSeconds - e.gameTime;
            return "[" + SkyrimNetEvents::FormatRelativeGameTime(delta < 0.0 ? 0.0 : delta) + "] " + RenderBody(e);
        }

        // ----------- sinks ----------------------------------------------

        // Note: player combat-state changes are NOT detected via
        // TESCombatEvent. That event does fire reliably for NPCs, but it
        // doesn't fire (or doesn't fire with `actor == player`) for the
        // player's own state flips — observed empirically in playtest where
        // hits and bleedout were captured fine but combat_start/end never
        // appeared. We poll player->IsInCombat() in Poll() instead.

        struct HitSink : public RE::BSTEventSink<RE::TESHitEvent>
        {
            RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* a_event,
                                                  RE::BSTEventSource<RE::TESHitEvent>* /*src*/) override
            {
                if (!a_event || !a_event->target) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                auto* targetActor = a_event->target->As<RE::Actor>();
                if (!targetActor) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                std::string targetName = ActorDisplayName(targetActor);
                if (targetName.empty()) {
                    return RE::BSEventNotifyControl::kContinue;
                }

                // Self-damage skip applies only when cause is a real actor
                // (concentration spell on self, etc.). Environmental hits
                // have null causes and should pass through.
                if (a_event->cause && a_event->cause.get() == a_event->target.get()) {
                    return RE::BSEventNotifyControl::kContinue;
                }

                const float radius = HitRadiusUnits();
                const bool targetInRange = WithinRadius(targetActor->GetPosition(), radius);

                bool causeIsActor = false;
                bool causeInRange = false;
                if (a_event->cause) {
                    if (auto* causeActor = a_event->cause->As<RE::Actor>()) {
                        if (!ActorDisplayName(causeActor).empty()) {
                            causeIsActor = true;
                            causeInRange = WithinRadius(causeActor->GetPosition(), radius);
                        }
                    }
                }

                // Distance gate: target must be in range OR (cause is a
                // named actor and the cause is in range). For environmental
                // hits (no actor cause), only the target's distance counts.
                if (!targetInRange && !(causeIsActor && causeInRange)) {
                    return RE::BSEventNotifyControl::kContinue;
                }

                const SourceLabel src = ResolveHitSource(*a_event);

                InternalEvent e;
                e.kind = Kind::Hit;
                e.localTime = EventLogUtil::NowUnixSeconds();
                e.gameTime = EventLogUtil::NowGameTimeSeconds();
                e.actorName = src.label; // may be empty (bare fallback)
                e.targetName = std::move(targetName);
                e.actorIsNamedActor = src.isNamedActor;

                {
                    std::scoped_lock lock(g_mutex);
                    PushLocked(std::move(e));
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        struct BleedoutSink : public RE::BSTEventSink<RE::TESEnterBleedoutEvent>
        {
            RE::BSEventNotifyControl ProcessEvent(const RE::TESEnterBleedoutEvent* a_event,
                                                  RE::BSTEventSource<RE::TESEnterBleedoutEvent>* /*src*/) override
            {
                if (!a_event || !a_event->actor) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                auto* actor = a_event->actor->As<RE::Actor>();
                if (!actor) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                std::string name = ActorDisplayName(actor);
                if (name.empty()) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                if (!WithinRadius(actor->GetPosition(), HitRadiusUnits())) {
                    return RE::BSEventNotifyControl::kContinue;
                }

                InternalEvent e;
                e.kind = Kind::Collapse;
                e.localTime = EventLogUtil::NowUnixSeconds();
                e.gameTime = EventLogUtil::NowGameTimeSeconds();
                e.actorName = name;
                e.actorIsNamedActor = true;

                {
                    std::scoped_lock lock(g_mutex);
                    g_bleedingOut[actor->GetFormID()] = name;
                    PushLocked(std::move(e));
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        HitSink* GetHitSink()
        {
            static HitSink s;
            return &s;
        }
        BleedoutSink* GetBleedoutSink()
        {
            static BleedoutSink s;
            return &s;
        }

        // Plain-data snapshot the plugin-thread Poll consumes. Filled
        // in the single MainThread::Run hop that opens Poll(); every
        // engine read the Poll body used to do inline is bundled here.
        struct PollSnapshot
        {
            // Player half — populated only when the player singleton
            // resolved.
            bool playerResolved = false;
            bool playerInCombat = false;
            std::string playerDisplayName;

            // Per-tracked-actor half — one entry per FormID passed
            // into the main-thread hop from g_bleedingOut. `exists`
            // is false when the form failed to resolve (unloaded /
            // despawned).
            struct ActorState
            {
                RE::FormID formID = 0;
                bool exists = false;
                bool isDead = false;
                bool isBleedingOut = false;
                bool inRadius = false;
            };
            std::vector<ActorState> actors;
        };

        // Main-thread body — fills a PollSnapshot from live engine
        // state. Called inside MainThread::Run from Poll. All engine
        // touches are bundled here so the plugin-thread Poll body
        // pays for exactly one main-thread hop per pass regardless
        // of how many bleedout actors are being tracked.
        PollSnapshot BuildPollSnapshotOnMain(const std::vector<RE::FormID>& trackedIds, float radius)
        {
            PollSnapshot s;
            auto* player = GetPlayer();
            if (!player) {
                return s;
            }
            s.playerResolved = true;
            s.playerInCombat = player->IsInCombat();
            s.playerDisplayName = ActorDisplayName(player);
            if (s.playerDisplayName.empty()) {
                s.playerDisplayName = "Player";
            }

            const RE::NiPoint3 playerPos = player->GetPosition();
            s.actors.reserve(trackedIds.size());
            for (auto fid : trackedIds) {
                PollSnapshot::ActorState a;
                a.formID = fid;
                auto* form = RE::TESForm::LookupByID(fid);
                auto* actor = form ? form->As<RE::Actor>() : nullptr;
                if (!actor) {
                    s.actors.push_back(a);
                    continue;
                }
                a.exists = true;
                a.isDead = actor->IsDead();
                if (auto* state = actor->AsActorState()) {
                    a.isBleedingOut = state->IsBleedingOut();
                }
                a.inRadius = actor->GetPosition().GetDistance(playerPos) <= radius;
                s.actors.push_back(a);
            }
            return s;
        }

        // Plugin-thread combat-state diff. Consumes the pre-fetched
        // snapshot and emits combat_start / combat_end on flip. Caller
        // holds g_mutex.
        void ProcessPlayerCombatLocked(const PollSnapshot& snap)
        {
            if (!snap.playerResolved) {
                return;
            }
            if (snap.playerInCombat == g_playerInCombatLast) {
                return;
            }

            InternalEvent e;
            e.kind = snap.playerInCombat ? Kind::CombatStart : Kind::CombatEnd;
            e.localTime = EventLogUtil::NowUnixSeconds();
            e.gameTime = EventLogUtil::NowGameTimeSeconds();
            e.actorName = snap.playerDisplayName;
            e.actorIsNamedActor = true;

            if (snap.playerInCombat) {
                g_currentEncounterStartRealTime = e.localTime;
            } else {
                g_currentEncounterStartRealTime = 0.0;
            }
            PushLocked(std::move(e));
            g_playerInCombatLast = snap.playerInCombat;
        }
    } // namespace

    void Initialize()
    {
        if (g_sinksRegistered)
            return;
        auto* src = RE::ScriptEventSourceHolder::GetSingleton();
        if (!src) {
            logger::error("CombatEventLog: ScriptEventSourceHolder unavailable; sinks not registered");
            return;
        }
        src->AddEventSink<RE::TESHitEvent>(GetHitSink());
        src->AddEventSink<RE::TESEnterBleedoutEvent>(GetBleedoutSink());
        g_sinksRegistered = true;
        logger::info("CombatEventLog: sinks registered (hit, bleedout); player combat-state polled");
    }

    void Shutdown()
    {
        if (!g_sinksRegistered)
            return;
        if (auto* src = RE::ScriptEventSourceHolder::GetSingleton()) {
            src->RemoveEventSink<RE::TESHitEvent>(GetHitSink());
            src->RemoveEventSink<RE::TESEnterBleedoutEvent>(GetBleedoutSink());
        }
        g_sinksRegistered = false;
    }

    void OnPhaseAdvanced()
    {
        std::scoped_lock lock(g_mutex);
        if (g_currentEncounterStartRealTime > 0.0) {
            const double cutoff = g_currentEncounterStartRealTime;
            std::erase_if(g_events, [cutoff](const InternalEvent& e) { return e.localTime < cutoff; });
        } else {
            g_events.clear();
        }
    }

    void Poll(const PluginThread::Token& pt)
    {
        const float radius = HitRadiusUnits();

        // 1. Plugin-thread: snapshot the FormIDs we're currently
        //    tracking, so the main-thread hop below knows which
        //    actors to query. Held briefly under g_mutex.
        std::vector<RE::FormID> trackedIds;
        {
            std::scoped_lock lock(g_mutex);
            trackedIds.reserve(g_bleedingOut.size());
            for (const auto& [fid, name] : g_bleedingOut) {
                trackedIds.push_back(fid);
            }
        }

        // 2. Main-thread hop: single Run for the player combat state
        //    + per-tracked-actor liveness/bleedout/proximity. Bundled
        //    into a plain-data PollSnapshot so the diff below runs
        //    entirely on the plugin thread against mutex-guarded
        //    state.
        //
        //    Deliberately outside g_mutex — MainThread::Run blocks the
        //    plugin thread until the main-thread lambda returns, so
        //    holding g_mutex across it would prevent main-thread
        //    readers (GetRenderedTail) from making progress.
        const auto snap = MainThread::Run(pt, [&trackedIds, radius](const MainThread::Token&) {
            return BuildPollSnapshotOnMain(trackedIds, radius);
        });

        // 3. Plugin-thread: consume the snapshot, mutate g_bleedingOut,
        //    emit events. Re-take g_mutex for the mutation phase.
        std::scoped_lock lock(g_mutex);

        ProcessPlayerCombatLocked(snap);

        std::vector<std::string> recoveredNames;
        for (const auto& a : snap.actors) {
            auto it = g_bleedingOut.find(a.formID);
            if (it == g_bleedingOut.end()) {
                // Sink dropped this entry between the snapshot and
                // now — nothing to reconcile.
                continue;
            }
            if (!a.exists) {
                // Unloaded / despawned — no recovery to report.
                g_bleedingOut.erase(it);
                continue;
            }
            if (a.isDead) {
                // Death already in SkyrimNet's event log; drop silently.
                g_bleedingOut.erase(it);
                continue;
            }
            if (!a.isBleedingOut) {
                if (a.inRadius) {
                    recoveredNames.push_back(it->second);
                }
                // Out-of-range recovery drops silently — player can't
                // witness it, so it isn't narratively significant.
                g_bleedingOut.erase(it);
                continue;
            }
            // Still bleeding out — leave in the set for the next Poll.
        }
        for (auto& n : recoveredNames) {
            EmitRegainFootingLocked(n);
        }
    }

    nlohmann::json GetRenderedTail(double currentGameTimeSeconds)
    {
        std::vector<InternalEvent> snapshot;
        {
            std::scoped_lock lock(g_mutex);
            snapshot = g_events;
        }
        std::sort(snapshot.begin(), snapshot.end(), [](const InternalEvent& a, const InternalEvent& b) {
            return a.localTime < b.localTime;
        });

        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : snapshot) {
            nlohmann::json j;
            // Single shared `type` for every combat event we emit — terse,
            // and the dashboard / LLM don't need a finer-grained label.
            // `ne_kind` retains the specific kind for the merge step and
            // any future renderer that wants to branch.
            j["type"] = "combat_event";
            j["ne_kind"] = KindName(e.kind);
            j["localTime"] = e.localTime;
            j["gameTime"] = e.gameTime;
            j["originatingActorName"] = e.actorName;
            j["targetActorName"] = e.targetName;
            j["ne_actor_is_named"] = e.actorIsNamedActor;
            j["text"] = RenderText(e, currentGameTimeSeconds);
            arr.push_back(std::move(j));
        }
        return arr;
    }

    void OnPostLoadGame()
    {
        // Seed g_bleedingOut from high-process actors that are currently
        // bleeding out. Misses actors that entered bleedout while unloaded —
        // acceptable, since their collapse event was either pruned by the
        // OnSave path or already in our restored payload.
        auto* pl = RE::ProcessLists::GetSingleton();
        std::unordered_map<RE::FormID, std::string> seeded;
        if (pl) {
            const float radius = HitRadiusUnits();
            pl->ForEachHighActor([&](RE::Actor* actor) {
                if (!actor)
                    return RE::BSContainer::ForEachResult::kContinue;
                if (actor->IsDead())
                    return RE::BSContainer::ForEachResult::kContinue;
                if (!actor->AsActorState()->IsBleedingOut()) {
                    return RE::BSContainer::ForEachResult::kContinue;
                }
                // Distance check is "is the player here now?" not "was the
                // player here when they collapsed." Good enough — we're only
                // using this to know whom to watch for recovery.
                if (!WithinRadius(actor->GetPosition(), radius)) {
                    return RE::BSContainer::ForEachResult::kContinue;
                }
                std::string name = ActorDisplayName(actor);
                if (name.empty())
                    return RE::BSContainer::ForEachResult::kContinue;
                seeded[actor->GetFormID()] = std::move(name);
                return RE::BSContainer::ForEachResult::kContinue;
            });
        }

        // Seed g_playerInCombatLast so the first Poll() doesn't emit a
        // bogus combat_start for a player that was already in combat at
        // the save. (If they were in combat then, the encounter-start
        // anchor on disk should already cover it.)
        const bool playerInCombatNow = [] {
            auto* pc = GetPlayer();
            return pc && pc->IsInCombat();
        }();

        std::scoped_lock lock(g_mutex);
        g_bleedingOut = std::move(seeded);
        g_playerInCombatLast = playerInCombatNow;
    }

    // -------- persistence --------------------------------------------

    void OnSave(SKSE::SerializationInterface* intfc)
    {
        if (!intfc)
            return;

        // Prune before snapshotting so on-disk == in-memory rules. Same
        // logic as OnPhaseAdvanced — never persist events from a previous
        // encounter.
        OnPhaseAdvanced();

        std::vector<InternalEvent> snapshot;
        double encounterStart = 0.0;
        {
            std::scoped_lock lock(g_mutex);
            snapshot = g_events;
            encounterStart = g_currentEncounterStartRealTime;
            (void)encounterStart; // currently recoverable from events; reserved for future schema
        }

        if (!intfc->OpenRecord(kRecordTypeId, kRecordVersion)) {
            logger::error("CombatEventLog::OnSave: OpenRecord failed");
            return;
        }
        const auto count = static_cast<std::uint32_t>(snapshot.size());
        intfc->WriteRecordData(count);
        for (const auto& e : snapshot) {
            const auto kindByte = static_cast<std::uint8_t>(e.kind);
            const auto namedByte = static_cast<std::uint8_t>(e.actorIsNamedActor ? 1 : 0);
            intfc->WriteRecordData(kindByte);
            intfc->WriteRecordData(e.localTime);
            intfc->WriteRecordData(e.gameTime);
            EventLogUtil::WriteString(intfc, e.actorName);
            EventLogUtil::WriteString(intfc, e.targetName);
            intfc->WriteRecordData(namedByte);
        }
    }

    void OnLoad(SKSE::SerializationInterface* intfc, std::uint32_t version, std::uint32_t length)
    {
        if (!intfc)
            return;
        if (version != kRecordVersion) {
            logger::warn("CombatEventLog::OnLoad: unknown version {} (length={}); clearing", version, length);
            OnRevert();
            return;
        }
        std::uint32_t count = 0;
        if (intfc->ReadRecordData(count) != sizeof(count)) {
            logger::error("CombatEventLog::OnLoad: failed to read count");
            OnRevert();
            return;
        }

        std::vector<InternalEvent> loaded;
        loaded.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            InternalEvent e;
            std::uint8_t kindByte = 0;
            std::uint8_t namedByte = 0;
            if (intfc->ReadRecordData(kindByte) != sizeof(kindByte)
                || intfc->ReadRecordData(e.localTime) != sizeof(e.localTime)
                || intfc->ReadRecordData(e.gameTime) != sizeof(e.gameTime)) {
                logger::error("CombatEventLog::OnLoad: short read on record {}/{}", i, count);
                OnRevert();
                return;
            }
            if (!EventLogUtil::ReadString(intfc, e.actorName) || !EventLogUtil::ReadString(intfc, e.targetName)) {
                logger::error("CombatEventLog::OnLoad: string read failure on record {}/{}", i, count);
                OnRevert();
                return;
            }
            if (intfc->ReadRecordData(namedByte) != sizeof(namedByte)) {
                logger::error("CombatEventLog::OnLoad: short read on namedByte for record {}/{}", i, count);
                OnRevert();
                return;
            }
            if (kindByte > static_cast<std::uint8_t>(Kind::RegainFooting)) {
                logger::warn("CombatEventLog::OnLoad: record {} invalid kind {}, skipping", i, kindByte);
                continue;
            }
            e.kind = static_cast<Kind>(kindByte);
            e.actorIsNamedActor = (namedByte != 0);
            loaded.push_back(std::move(e));
        }

        // Recover g_currentEncounterStartRealTime from the loaded events:
        // the newest player combat_start with no subsequent combat_end is
        // the in-progress encounter's start.
        double encounterStart = 0.0;
        for (auto it = loaded.rbegin(); it != loaded.rend(); ++it) {
            if (it->kind == Kind::CombatEnd) {
                break; // newest event is a combat_end → no in-progress encounter
            }
            if (it->kind == Kind::CombatStart) {
                encounterStart = it->localTime;
                break;
            }
        }

        {
            std::scoped_lock lock(g_mutex);
            g_events = std::move(loaded);
            g_currentEncounterStartRealTime = encounterStart;
            // g_bleedingOut is rebuilt in OnPostLoadGame (called separately
            // via the SKSE messaging interface), not here.
        }
        logger::info("CombatEventLog::OnLoad: restored {} record(s), encounterStart={:.1f}", count, encounterStart);
    }

    void OnRevert()
    {
        std::scoped_lock lock(g_mutex);
        g_events.clear();
        g_currentEncounterStartRealTime = 0.0;
        g_bleedingOut.clear();
        g_playerInCombatLast = false;
        g_pendingHistory.clear();
    }

    std::vector<EventLogUtil::HistoryEntry> DrainHistoryTail()
    {
        std::vector<PendingHistoryItem> drained;
        {
            std::scoped_lock lock(g_mutex);
            drained.swap(g_pendingHistory);
        }
        std::vector<EventLogUtil::HistoryEntry> out;
        out.reserve(drained.size());
        for (const auto& item : drained) {
            EventLogUtil::HistoryEntry h;
            h.localTime = item.event.localTime;
            h.inGameTimestamp = item.inGameTimestamp;
            h.sourceKind = std::string("internal/combat_event/") + KindName(item.event.kind);
            h.body = RenderBody(item.event);
            out.push_back(std::move(h));
        }
        return out;
    }
} // namespace NarrativeEngine::CombatEventLog
