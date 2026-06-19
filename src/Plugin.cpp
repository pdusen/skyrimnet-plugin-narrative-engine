#include <Plugin.h>

#include <AsyncDispatch.h>
#include <CombatEventLog.h>
#include <DashboardUIManager.h>
#include <DecisionLog.h>
#include <Decorators.h>
#include <PhaseTracker.h>
#include <PrismaUI.h>
#include <Settings.h>
#include <SkyrimNetAPI.h>
#include <Tick.h>
#include <logger.h>

#include <algorithm>

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
                    break;
                case SKSE::MessagingInterface::kNewGame:
                    logger::info("OnMessage: kNewGame");
                    DecisionLog::Clear();
                    CombatEventLog::OnRevert();
                    PhaseTracker::Reset(PhaseTracker::Phase::Exposition);
                    Tick::Start();
                    break;
                case SKSE::MessagingInterface::kPreLoadGame:
                    logger::info("OnMessage: kPreLoadGame");
                    // Stop the tick driver FIRST so an in-flight tick can't
                    // fire mid-deserialize, then safe-init every subsystem
                    // so OnLoad has a known baseline to overwrite.
                    Tick::Stop();
                    DecisionLog::Clear();
                    CombatEventLog::OnRevert();
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
