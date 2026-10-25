#include <PlotRecipes.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <unordered_map>

// The engine-free half of PlotRecipes: everything a recipe is judged by once
// it has been read. The reading is PlotRecipesLoad.cpp.
namespace NarrativeEngine::PlotRecipes
{
    std::string_view CraftId(Craft craft) noexcept
    {
        switch (craft) {
        case Craft::Smith:
            return "smith";
        case Craft::Concoct:
            return "concoct";
        case Craft::Enchant:
            return "enchant";
        case Craft::Set:
            return "set";
        case Craft::Fletch:
            return "fletch";
        case Craft::Count:
            break;
        }
        return {};
    }

    RE::ActorValue SkillOf(Craft craft) noexcept
    {
        switch (craft) {
        case Craft::Concoct:
            return RE::ActorValue::kAlchemy;
        case Craft::Enchant:
            return RE::ActorValue::kEnchanting;
        case Craft::Smith:
        case Craft::Set:
        case Craft::Fletch:
        case Craft::Count:
            break;
        }
        return RE::ActorValue::kSmithing;
    }

    std::optional<Craft> ForgeCraftOf(Made made) noexcept
    {
        switch (made) {
        case Made::Weapon:
        case Made::Armor:
            return Craft::Smith;
        case Made::Bow:
        case Made::Ammo:
            return Craft::Fletch;
        case Made::Jewelry:
            return Craft::Set;
        case Made::Other:
            break;
        }
        return std::nullopt;
    }

    std::vector<Recipe> Deduplicated(std::vector<Recipe> recipes)
    {
        const auto better = [](const Recipe& a, const Recipe& b) {
            if (a.skill != b.skill) {
                return a.skill < b.skill;
            }
            return a.ingredientValue < b.ingredientValue;
        };

        // Where each craft-and-name first stands, so the survivor keeps the
        // first one's place in the list.
        std::unordered_map<std::string, std::size_t> at;
        std::vector<Recipe> out;
        for (auto& recipe : recipes) {
            const auto key = std::format("{}:{}", static_cast<int>(recipe.craft), recipe.displayName);
            const auto [it, fresh] = at.try_emplace(key, out.size());
            if (fresh) {
                out.push_back(std::move(recipe));
            } else if (better(recipe, out[it->second])) {
                out[it->second] = std::move(recipe);
            }
        }
        return out;
    }

    double Importance(float ingredientValue, int ingredientCount) noexcept
    {
        // Worth on a log scale, because ingredient prices span three orders
        // of magnitude: an iron dagger's are worth about ten septims and a
        // set of Daedric armor's about a thousand. Two thousand reads as the
        // top of the range.
        constexpr double kDearest = 2000.0;
        // Ten ingredients is a big job: Daedric armor takes nine.
        constexpr double kMostIngredients = 10.0;

        const double value = std::max(0.0, static_cast<double>(ingredientValue));
        const double worth = std::clamp(std::log10(1.0 + value) / std::log10(1.0 + kDearest), 0.0, 1.0);
        const double count = std::clamp(static_cast<double>(std::max(0, ingredientCount)) / kMostIngredients, 0.0, 1.0);
        return 0.75 * worth + 0.25 * count;
    }
} // namespace NarrativeEngine::PlotRecipes
