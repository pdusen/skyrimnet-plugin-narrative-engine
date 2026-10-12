#include <PlotRecipes.h>

#include <logger.h>

#include <algorithm>
#include <array>
#include <unordered_map>

// The engine-bound half of PlotRecipes: reading every recipe the loaded game
// has. The rules a recipe is judged by are in PlotRecipes.cpp.
namespace NarrativeEngine::PlotRecipes
{
    namespace
    {
        std::vector<Recipe> g_recipes;
        bool g_loaded = false;

        // Where smith, set and fletch work is done. Vanilla has others -- the
        // cooking pot, the tanning rack, the Aetherium forge, the staff
        // enchanter -- but nothing a craftsman of these three makes there.
        constexpr std::array<const char*, 3> kBenches{"CraftingSmithingForge",
                                                      "CraftingSmithingSkyforge",
                                                      "DLC1CraftingDawnguard"};

        std::string NameOf(const RE::TESForm* form)
        {
            const char* name = form != nullptr ? form->GetName() : nullptr;
            return name != nullptr ? std::string{name} : std::string{};
        }

        Made MadeOf(RE::TESForm* form)
        {
            if (form->Is(RE::FormType::Ammo)) {
                return Made::Ammo;
            }
            if (const auto* weapon = form->As<RE::TESObjectWEAP>()) {
                const auto type = weapon->weaponData.animationType.get();
                return type == RE::WEAPON_TYPE::kBow || type == RE::WEAPON_TYPE::kCrossbow ? Made::Bow : Made::Weapon;
            }
            if (const auto* armor = form->As<RE::TESObjectARMO>()) {
                using Slot = RE::BIPED_MODEL::BipedObjectSlot;
                constexpr auto kJewelry = static_cast<std::uint32_t>(Slot::kAmulet)
                                          | static_cast<std::uint32_t>(Slot::kRing)
                                          | static_cast<std::uint32_t>(Slot::kCirclet);
                const auto slots = static_cast<std::uint32_t>(armor->bipedModelData.bipedObjectSlots.get());
                return slots != 0 && (slots & ~kJewelry) == 0 ? Made::Jewelry : Made::Armor;
            }
            return Made::Other;
        }

        // The highest Smithing a recipe's perks ask for. A recipe names a
        // perk with HasPerk; the perk names the skill it needs with
        // GetBaseActorValue. Anything else a recipe is conditioned on -- a
        // quest stage, an item in the player's pack -- is about the player,
        // and a craftsman in a plot is not the player.
        int SkillAsked(const RE::BGSConstructibleObject& recipe)
        {
            using Function = RE::FUNCTION_DATA::FunctionID;
            int skill = 0;
            for (auto* item = recipe.conditions.head; item != nullptr; item = item->next) {
                if (item->data.functionData.function.get() != Function::kHasPerk) {
                    continue;
                }
                const auto* perk = static_cast<RE::BGSPerk*>(item->data.functionData.params[0]);
                if (perk == nullptr) {
                    continue;
                }
                for (auto* need = perk->perkConditions.head; need != nullptr; need = need->next) {
                    const auto& data = need->data;
                    if (data.functionData.function.get() != Function::kGetBaseActorValue || data.flags.global) {
                        continue;
                    }
                    const auto value =
                        static_cast<RE::ActorValue>(reinterpret_cast<std::uintptr_t>(data.functionData.params[0]));
                    if (value == RE::ActorValue::kSmithing) {
                        skill = std::max(skill, static_cast<int>(data.comparisonValue.f));
                    }
                }
            }
            return skill;
        }

        void ReadForgeRecipes(RE::TESDataHandler& data, std::vector<Recipe>& out)
        {
            std::vector<const RE::BGSKeyword*> benches;
            for (const char* id : kBenches) {
                if (const auto* bench = RE::TESForm::LookupByEditorID<RE::BGSKeyword>(id)) {
                    benches.push_back(bench);
                } else {
                    logger::warn("PlotRecipes: workbench keyword {} did not resolve", id);
                }
            }

            for (const auto* recipe : data.GetFormArray<RE::BGSConstructibleObject>()) {
                if (recipe == nullptr || recipe->createdItem == nullptr
                    || std::find(benches.begin(), benches.end(), recipe->benchKeyword) == benches.end()) {
                    continue;
                }
                const auto made = MadeOf(recipe->createdItem);
                const auto craft = ForgeCraftOf(made);
                const auto name = NameOf(recipe->createdItem);
                if (!craft || name.empty()) {
                    continue;
                }

                Recipe r;
                r.form = recipe->createdItem->GetFormID();
                r.displayName = Phrase(name, made == Made::Ammo);
                r.craft = *craft;
                r.skill = SkillAsked(*recipe);
                recipe->requiredItems.ForEachContainerObject([&r](RE::ContainerObject& entry) {
                    if (entry.obj != nullptr && entry.count > 0) {
                        r.ingredientValue += static_cast<float>(entry.obj->GetGoldValue() * entry.count);
                        r.ingredientCount += entry.count;
                    }
                    return RE::BSContainer::ForEachResult::kContinue;
                });
                out.push_back(std::move(r));
            }
        }

        // Any potion or poison two ingredients share an effect for. Making
        // one in Skyrim is mixing two ingredients that carry its effect, so
        // that pair is its recipe -- the cheapest pair, as a craftsman would.
        void ReadPotions(RE::TESDataHandler& data, std::vector<Recipe>& out)
        {
            // What each effect's ingredients are worth, by effect.
            std::unordered_map<const RE::EffectSetting*, std::vector<float>> carriers;
            for (auto* ingredient : data.GetFormArray<RE::IngredientItem>()) {
                if (ingredient == nullptr) {
                    continue;
                }
                const auto value = static_cast<float>(ingredient->GetGoldValue());
                for (const auto* effect : ingredient->effects) {
                    if (effect != nullptr && effect->baseEffect != nullptr) {
                        carriers[effect->baseEffect].push_back(value);
                    }
                }
            }

            for (auto* potion : data.GetFormArray<RE::AlchemyItem>()) {
                if (potion == nullptr || potion->IsFood()) {
                    continue;
                }
                const auto name = NameOf(potion);
                if (name.empty()) {
                    continue;
                }
                // The costliest effect is what names a potion in the game.
                const RE::Effect* costliest = nullptr;
                for (const auto* effect : potion->effects) {
                    if (effect != nullptr && effect->baseEffect != nullptr
                        && (costliest == nullptr || effect->cost > costliest->cost)) {
                        costliest = effect;
                    }
                }
                if (costliest == nullptr) {
                    continue;
                }
                const auto found = carriers.find(costliest->baseEffect);
                if (found == carriers.end() || found->second.size() < 2) {
                    continue;
                }
                auto prices = found->second;
                std::partial_sort(prices.begin(), prices.begin() + 2, prices.end());

                Recipe r;
                r.form = potion->GetFormID();
                r.displayName = Phrase(name, false);
                r.craft = Craft::Concoct;
                r.ingredientValue = prices[0] + prices[1];
                r.ingredientCount = 2;
                out.push_back(std::move(r));
            }
        }

        // An enchanted variant and the base item it names as its template:
        // "Iron Sword of Burning" from "Iron Sword". Its ingredients are that
        // base item and a soul, and the soul is worth what the enchantment
        // adds to the item, which is how a stronger enchantment costs more.
        template <typename T> void ReadEnchanted(RE::TESDataHandler& data, T* T::* base, std::vector<Recipe>& out)
        {
            for (auto* item : data.GetFormArray<T>()) {
                if (item == nullptr || item->formEnchanting == nullptr || !item->GetPlayable()) {
                    continue;
                }
                const auto* from = item->*base;
                if (from == nullptr) {
                    continue;
                }
                const auto name = NameOf(item);
                const auto fromName = NameOf(from);
                // A unique piece made on a common template does not bear its
                // name, and is not something a court wizard makes.
                if (fromName.empty() || name.size() <= fromName.size() || !name.starts_with(fromName)) {
                    continue;
                }
                const auto baseValue = static_cast<float>(from->GetGoldValue());
                const auto soul = std::max(0.0f, static_cast<float>(item->GetGoldValue()) - baseValue);

                Recipe r;
                r.form = item->GetFormID();
                r.displayName = Phrase(name, false);
                r.craft = Craft::Enchant;
                r.ingredientValue = baseValue + soul;
                r.ingredientCount = 2;
                out.push_back(std::move(r));
            }
        }
    } // namespace

    void Load()
    {
        if (g_loaded) {
            return;
        }
        g_loaded = true;

        auto* data = RE::TESDataHandler::GetSingleton();
        if (data == nullptr) {
            logger::warn("PlotRecipes: no data handler; no crafting step can be planned");
            return;
        }

        std::vector<Recipe> read;
        ReadForgeRecipes(*data, read);
        ReadPotions(*data, read);
        ReadEnchanted(*data, &RE::TESObjectWEAP::templateWeapon, read);
        ReadEnchanted(*data, &RE::TESObjectARMO::templateArmor, read);
        g_recipes = Deduplicated(std::move(read));

        std::array<std::size_t, kCraftCount> counts{};
        for (const auto& recipe : g_recipes) {
            ++counts[static_cast<std::size_t>(recipe.craft)];
        }
        logger::info("PlotRecipes: loaded {} recipe(s) -- smith {}, concoct {}, enchant {}, set {}, fletch {}.",
                     g_recipes.size(),
                     counts[static_cast<std::size_t>(Craft::Smith)],
                     counts[static_cast<std::size_t>(Craft::Concoct)],
                     counts[static_cast<std::size_t>(Craft::Enchant)],
                     counts[static_cast<std::size_t>(Craft::Set)],
                     counts[static_cast<std::size_t>(Craft::Fletch)]);
    }

    const std::vector<Recipe>& Recipes()
    {
        return g_recipes;
    }
} // namespace NarrativeEngine::PlotRecipes
