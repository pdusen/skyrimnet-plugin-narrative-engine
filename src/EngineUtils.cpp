#include <EngineUtils.h>

#include <RE/Skyrim.h>

namespace NarrativeEngine::EngineUtils
{
    double GetCurrentGameHours()
    {
        auto* calendar = RE::Calendar::GetSingleton();
        return calendar ? static_cast<double>(calendar->GetHoursPassed()) : 0.0;
    }

    bool IsGamePaused()
    {
        auto* ui = RE::UI::GetSingleton();
        return ui != nullptr && ui->GameIsPaused();
    }

    bool IsPlayerInCombat()
    {
        auto* pc = RE::PlayerCharacter::GetSingleton();
        return pc != nullptr && pc->IsInCombat();
    }

    bool IsPlayerInDialogue()
    {
        auto* ui = RE::UI::GetSingleton();
        if (!ui) return false;
        return ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME);
    }
}
