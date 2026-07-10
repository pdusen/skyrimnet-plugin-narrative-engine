#include <Plugin.h>

#include <ActionDispatcher.h>
#include <ActionRegistry.h>
#include <AmbushAction.h>
#include <AsyncDispatch.h>
#include <CombatEventLog.h>
#include <DashboardUIManager.h>
#include <DecisionLog.h>
#include <Decorators.h>
#include <LetterPool.h>
#include <NPCLetterAction.h>
#include <NPCVisitAction.h>
#include <PhaseTracker.h>
#include <PrismaUI.h>
#include <Settings.h>
#include <SkyrimNetAPI.h>
#include <Tick.h>
#include <VisitState.h>
#include <logger.h>

#include <algorithm>
#include <memory>

namespace NarrativeEngine
{
    namespace
    {
        void OnMessage(SKSE::MessagingInterface::Message* msg)
        {
            if (!msg) {
                return;
            }

            switch (msg->type) {
                case SKSE::MessagingInterface::kDataLoaded:
                    logger::info("OnMessage: kDataLoaded");
                    // Settings::Load() runs FIRST so later subsystems can
                    // consult the populated Config when they initialize.
                    Settings::Load();
                    DecisionLog::SetMaxEntries(static_cast<std::size_t>(
                        std::max(0, Settings::Get().decisionLogMaxEntries)));
                    SkyrimNetAPI::Initialize();
                    // Decorators must register AFTER SkyrimNetAPI::Initialize
                    // (it null-checks SkyrimNet availability internally).
                    Decorators::Register();
                    PrismaUI_API::Initialize();
                    // DashboardUIManager creates the PrismaUI view and
                    // registers the hotkey sink; gracefully no-ops when
                    // PrismaUI is unavailable.
                    DashboardUIManager::Initialize();
                    // CombatEventLog hooks SKSE's combat/hit/bleedout
                    // events. Safe to register at kDataLoaded — the event
                    // source holder exists by then.
                    CombatEventLog::Initialize();
                    AsyncDispatch::Start();
                    // ActionDispatcher registers the _ne_ActionCompleted
                    // ModEvent sink. Order matters: AsyncDispatch must be
                    // up because the sink marshals through it.
                    ActionDispatcher::Initialize();
                    // Register shipped actions. The dispatcher must be
                    // live first so its completion sink is in place
                    // before any action can fire.
                    ActionRegistry::Register(std::make_unique<AmbushAction>());
                    ActionRegistry::Register(std::make_unique<NPCLetterAction>());
                    if (Settings::Get().enableNpcVisit) {
                        ActionRegistry::Register(std::make_unique<NPCVisitAction>());
                        // Resolve the Phase 05 CK content (quest,
                        // faction, aliases) so Start's dispatch chain
                        // has warm handles. Must run after the action
                        // has been registered so any registration-time
                        // gating sees the resolved state.
                        NPCVisitAction_Init::Initialize();
                    } else {
                        logger::info("Plugin: NPCVisitAction disabled via bEnableNpcVisit=false");
                    }
                    // Resolve the 20 _ne_PooledLetterNN EditorIDs to Book
                    // FormIDs. Must run AFTER kDataLoaded fires the rest
                    // of the registry chain because TESForm lookups by
                    // EditorID rely on powerofthree's Tweaks (or similar)
                    // having populated the lookup table by now.
                    LetterPool::Initialize();
                    // Install the MinHook detours that route engine
                    // book-body reads through the pool's content cache.
                    LetterPool::InstallHooks();
                    // Register the MenuOpenCloseEvent sink so the pool
                    // can detect when a letter the player opened gets
                    // closed (the "read" lifecycle edge).
                    LetterPool::RegisterMenuEventSink();
                    // TESContainerChangedEvent sink drives MarkDelivered
                    // (courier → player) and the discard / drop recycle
                    // paths.
                    LetterPool::RegisterContainerEventSink();
                    // Warm the WICourier resolution and resolve the 20
                    // _ne_PooledLetterQuestNN per-slot delivery quests
                    // into NPCLetterAction's dispatch cache. Must run
                    // AFTER LetterPool::Initialize so the slot indexing
                    // is consistent across both subsystems.
                    NPCLetterAction_Init::Initialize();
                    break;
                case SKSE::MessagingInterface::kNewGame:
                    logger::info("OnMessage: kNewGame");
                    DecisionLog::Clear();
                    CombatEventLog::OnRevert();
                    ActionDispatcher::OnRevert();
                    AmbushAction_Persistence::OnRevert();
                    NPCLetterAction_Persistence::OnRevert();
                    NPCVisitAction_Persistence::OnRevert();
                    VisitState::OnRevert();
                    PhaseTracker::Reset(PhaseTracker::Phase::Exposition);
                    Tick::Start();
                    break;
                case SKSE::MessagingInterface::kPreLoadGame:
                    logger::info("OnMessage: kPreLoadGame");
                    // Stop the tick driver FIRST so an in-flight tick can't
                    // fire mid-deserialize, then safe-init every subsystem
                    // so OnLoad has a known baseline to overwrite.
                    //
                    // Every persistence module gets its OnRevert() called
                    // here so that OnLoad's dispatch — which only fires
                    // for records actually present in the save — starts
                    // from a clean baseline. Without this, cooldown /
                    // in-flight state can leak across save loads (e.g.
                    // Character A's saved state persists into Character
                    // B's session if B's save doesn't have the record).
                    Tick::Stop();
                    DecisionLog::Clear();
                    CombatEventLog::OnRevert();
                    ActionDispatcher::OnRevert();
                    AmbushAction_Persistence::OnRevert();
                    NPCLetterAction_Persistence::OnRevert();
                    NPCVisitAction_Persistence::OnRevert();
                    VisitState::OnRevert();
                    PhaseTracker::Reset();
                    break;
                case SKSE::MessagingInterface::kPostLoadGame:
                    logger::info("OnMessage: kPostLoadGame");
                    // Rebuild the bleedingOut set from currently-loaded
                    // high-process actors so recovery detection works for
                    // any actor mid-bleedout at save time.
                    CombatEventLog::OnPostLoadGame();
                    Tick::Start();
                    break;
                default:
                    break;
            }
        }

        void OnSave(SKSE::SerializationInterface* intfc)
        {
            logger::debug("OnSave");
            PhaseTracker::OnSave(intfc);
            DecisionLog::OnSave(intfc);
            CombatEventLog::OnSave(intfc);
            ActionDispatcher::OnSave(intfc);
            AmbushAction_Persistence::OnSave(intfc);
            NPCLetterAction_Persistence::OnSave(intfc);
            NPCVisitAction_Persistence::OnSave(intfc);
            LetterPool::OnSave(intfc);
            VisitState::OnSave(intfc);
            // Future subsystems append their OnSave calls here.
        }

        void OnLoad(SKSE::SerializationInterface* intfc)
        {
            logger::debug("OnLoad");
            if (!intfc) {
                return;
            }

            std::uint32_t type{};
            std::uint32_t version{};
            std::uint32_t length{};
            while (intfc->GetNextRecordInfo(type, version, length)) {
                switch (type) {
                    case PhaseTracker::kRecordTypeId:
                        PhaseTracker::OnLoad(intfc, version, length);
                        break;
                    case DecisionLog::kRecordTypeId:
                        DecisionLog::OnLoad(intfc, version, length);
                        break;
                    case CombatEventLog::kRecordTypeId:
                        CombatEventLog::OnLoad(intfc, version, length);
                        break;
                    case ActionDispatcher::kRecordTypeId:
                        ActionDispatcher::OnLoad(intfc, version, length);
                        break;
                    case AmbushAction_Persistence::kRecordTypeId:
                        AmbushAction_Persistence::OnLoad(intfc, version, length);
                        break;
                    case NPCLetterAction_Persistence::kRecordTypeId:
                        NPCLetterAction_Persistence::OnLoad(intfc, version, length);
                        break;
                    case NPCVisitAction_Persistence::kRecordTypeId:
                        NPCVisitAction_Persistence::OnLoad(intfc, version, length);
                        break;
                    case LetterPool::kRecordTypeId:
                        LetterPool::OnLoad(intfc, version, length);
                        break;
                    case VisitState::kRecordTypeId:
                        VisitState::OnLoad(intfc, version, length);
                        break;
                    default:
                        // Unknown record — likely from a newer build or a
                        // removed subsystem. GetNextRecordInfo's next call
                        // skips past the data, so just continuing is correct.
                        logger::warn(
                            "OnLoad: skipping unrecognized co-save record (type=0x{:08X}, version={}, length={})",
                            type, version, length);
                        break;
                }
            }
        }

        void OnRevert(SKSE::SerializationInterface*)
        {
            logger::debug("OnRevert");
            PhaseTracker::OnRevert();
            DecisionLog::OnRevert();
            CombatEventLog::OnRevert();
            ActionDispatcher::OnRevert();
            AmbushAction_Persistence::OnRevert();
            NPCLetterAction_Persistence::OnRevert();
            NPCVisitAction_Persistence::OnRevert();
            LetterPool::OnRevert();
            VisitState::OnRevert();
            // Future subsystems append their OnRevert calls here.
        }
    }

    bool Startup(const SKSE::LoadInterface* skse)
    {
        SKSE::Init(skse);
        SetupLog();
        logger::info("NarrativeEngine starting up.");

        auto* messaging = SKSE::GetMessagingInterface();
        if (!messaging || !messaging->RegisterListener(OnMessage)) {
            logger::error("Failed to register SKSE messaging listener.");
            return false;
        }

        auto* serialization = SKSE::GetSerializationInterface();
        if (!serialization) {
            logger::error("SKSE serialization interface unavailable.");
            return false;
        }
        serialization->SetUniqueID(kCoSaveUniqueID);
        serialization->SetSaveCallback(OnSave);
        serialization->SetLoadCallback(OnLoad);
        serialization->SetRevertCallback(OnRevert);

        logger::info("NarrativeEngine startup complete (co-save unique ID = 0x{:08X}).", kCoSaveUniqueID);
        return true;
    }
}
