#include <CombatEventLog.h>

#include <logger.h>
#include <Settings.h>
#include <SkyrimNetEvents.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
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

        double NowUnixSeconds()
        {
            return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
        }

        double NowGameTimeOfDaySeconds()
        {
            // Match the units SkyrimNet uses for per-event `gameTime`:
            // time-of-day in seconds [0..86400). Calendar->GetHour() returns
            // fractional hours since midnight.
            if (auto* cal = RE::Calendar::GetSingleton()) {
                const float h = cal->GetHour();
                return static_cast<double>(h) * 3600.0;
            }
            return 0.0;
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
            e.localTime = NowUnixSeconds();
            e.gameTime = NowGameTimeOfDaySeconds();
            e.actorName = name;
            e.actorIsNamedActor = true;
            PushLocked(std::move(e));
        }

        // ----------- rendered text --------------------------------------

        std::string RenderText(const InternalEvent& e, double currentGameTimeSeconds)
        {
            std::string body;
            switch (e.kind) {
            case Kind::CombatStart:
                body = e.actorName + " enters combat";
                break;
            case Kind::CombatEnd:
                body = e.actorName + " leaves combat";
                break;
            case Kind::Hit:
                if (e.actorIsNamedActor) {
                    body = e.actorName + " strikes " + e.targetName;
                } else if (!e.actorName.empty()) {
                    body = e.targetName + " took damage from " + e.actorName;
                } else {
                    body = e.targetName + " took damage";
                }
                break;
            case Kind::Collapse:
                body = e.actorName + " collapses";
                break;
            case Kind::RegainFooting:
                body = e.actorName + " regains their footing";
                break;
            }

            const double delta = currentGameTimeSeconds - e.gameTime;
            return "[" + SkyrimNetEvents::FormatRelativeGameTime(delta < 0.0 ? 0.0 : delta) + "] " + body;
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
                e.localTime = NowUnixSeconds();
                e.gameTime = NowGameTimeOfDaySeconds();
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
                e.localTime = NowUnixSeconds();
                e.gameTime = NowGameTimeOfDaySeconds();
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

        // Player combat-state poll. Runs on the main thread under our mutex
        // already held by the caller. Detects IsInCombat() flips and pushes
        // combat_start / combat_end, updating g_currentEncounterStartRealTime
        // to match.
        void PollPlayerCombatLocked()
        {
            auto* player = GetPlayer();
            if (!player)
                return;
            const bool nowInCombat = player->IsInCombat();
            if (nowInCombat == g_playerInCombatLast)
                return;

            InternalEvent e;
            e.kind = nowInCombat ? Kind::CombatStart : Kind::CombatEnd;
            e.localTime = NowUnixSeconds();
            e.gameTime = NowGameTimeOfDaySeconds();
            e.actorName = ActorDisplayName(player);
            if (e.actorName.empty())
                e.actorName = "Player";
            e.actorIsNamedActor = true;

            if (nowInCombat) {
                g_currentEncounterStartRealTime = e.localTime;
            } else {
                g_currentEncounterStartRealTime = 0.0;
            }
            PushLocked(std::move(e));
            g_playerInCombatLast = nowInCombat;
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

    void Poll()
    {
        // Main thread: safe to call engine lookups / state queries.
        std::scoped_lock lock(g_mutex);

        // 1. Player combat-state poll.
        PollPlayerCombatLocked();

        // 2. Bleedout recovery walk.
        const float radius = HitRadiusUnits();
        std::vector<std::string> recoveredNames;
        for (auto it = g_bleedingOut.begin(); it != g_bleedingOut.end();) {
            auto* form = RE::TESForm::LookupByID(it->first);
            auto* actor = form ? form->As<RE::Actor>() : nullptr;
            if (!actor) {
                // Unloaded / despawned — no recovery to report.
                it = g_bleedingOut.erase(it);
                continue;
            }
            if (actor->IsDead()) {
                // Death already in SkyrimNet's event log; drop silently.
                it = g_bleedingOut.erase(it);
                continue;
            }
            if (!actor->AsActorState()->IsBleedingOut()) {
                if (WithinRadius(actor->GetPosition(), radius)) {
                    recoveredNames.push_back(it->second);
                }
                // Out-of-range recovery drops silently — player can't
                // witness it, so it isn't narratively significant.
                it = g_bleedingOut.erase(it);
                continue;
            }
            ++it;
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

    namespace
    {
        void WriteString(SKSE::SerializationInterface* intfc, const std::string& s)
        {
            const auto len = static_cast<std::uint32_t>(s.size());
            intfc->WriteRecordData(len);
            if (len > 0)
                intfc->WriteRecordData(s.data(), len);
        }

        bool ReadString(SKSE::SerializationInterface* intfc, std::string& out)
        {
            std::uint32_t len = 0;
            if (intfc->ReadRecordData(len) != sizeof(len))
                return false;
            out.resize(len);
            if (len > 0 && intfc->ReadRecordData(out.data(), len) != len)
                return false;
            return true;
        }
    } // namespace

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
            WriteString(intfc, e.actorName);
            WriteString(intfc, e.targetName);
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
            if (!ReadString(intfc, e.actorName) || !ReadString(intfc, e.targetName)) {
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
    }
} // namespace NarrativeEngine::CombatEventLog
