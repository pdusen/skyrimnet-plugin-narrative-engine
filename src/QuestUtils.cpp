#include <QuestUtils.h>

namespace NarrativeEngine::QuestUtils
{
    using namespace std::string_view_literals;

    bool VMDispatchQuestSetStage(RE::TESQuest* quest, std::uint32_t stage)
    {
        return VMDispatchOnQuest(
            quest, "Quest"sv, "SetStage"sv,
            static_cast<std::int32_t>(stage));
    }
}
