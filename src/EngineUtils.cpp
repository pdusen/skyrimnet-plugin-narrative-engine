#include <EngineUtils.h>

#include <RE/Skyrim.h>

namespace NarrativeEngine::EngineUtils
{
    double GetCurrentGameHours()
    {
        auto* calendar = RE::Calendar::GetSingleton();
        return calendar ? static_cast<double>(calendar->GetHoursPassed()) : 0.0;
    }

    double GetCurrentGameHours(const MainThread::Token&)
    {
        return GetCurrentGameHours();
    }

    bool IsGamePaused()
    {
        auto* ui = RE::UI::GetSingleton();
        return ui != nullptr && ui->GameIsPaused();
    }

    bool IsGamePaused(const MainThread::Token&)
    {
        return IsGamePaused();
    }

    bool IsPlayerInCombat()
    {
        auto* pc = RE::PlayerCharacter::GetSingleton();
        return pc != nullptr && pc->IsInCombat();
    }

    bool IsPlayerInCombat(const MainThread::Token&)
    {
        return IsPlayerInCombat();
    }

    bool IsPlayerInDialogue()
    {
        auto* ui = RE::UI::GetSingleton();
        if (!ui)
            return false;
        return ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME);
    }

    bool IsPlayerInDialogue(const MainThread::Token&)
    {
        return IsPlayerInDialogue();
    }

    namespace
    {
        // The one safe way to reach the fast-travel event source.
        // `AsTESFastTravelEndEventSource()` is CommonLibSSE-NG's
        // runtime-aware accessor: it hands back the relocated member on
        // SE/AE and a null pointer on VR, where the holder simply has no
        // such base class. The templated
        // `ScriptEventSourceHolder::AddEventSink<TESFastTravelEndEvent>`
        // routes through the same accessor but immediately dereferences
        // the result, so on VR it faults on a null + 0x48 read.
        RE::BSTEventSource<RE::TESFastTravelEndEvent>* GetFastTravelEndEventSource()
        {
            auto* holder = RE::ScriptEventSourceHolder::GetSingleton();
            return holder ? holder->AsTESFastTravelEndEventSource() : nullptr;
        }
    } // namespace

    bool AddFastTravelEndSink(RE::BSTEventSink<RE::TESFastTravelEndEvent>* sink)
    {
        auto* source = GetFastTravelEndEventSource();
        if (!source || !sink)
            return false;
        source->AddEventSink(sink);
        return true;
    }

    bool RemoveFastTravelEndSink(RE::BSTEventSink<RE::TESFastTravelEndEvent>* sink)
    {
        auto* source = GetFastTravelEndEventSource();
        if (!source || !sink)
            return false;
        source->RemoveEventSink(sink);
        return true;
    }
} // namespace NarrativeEngine::EngineUtils
