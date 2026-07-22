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
} // namespace NarrativeEngine::EngineUtils
