#include <PlotResolution.h>

#include <catch2/catch_test_macros.hpp>

// Tests for the craft profile's place among the resolution rows: that a
// crafting step is sized, saved and gated like any other kind of step.

namespace
{
    namespace PlotModel = NarrativeEngine::PlotModel;
    namespace PlotResolution = NarrativeEngine::PlotResolution;
    using PlotModel::StepType;
} // namespace

TEST_CASE("PlotResolution craft profile", "[PlotResolution][engine]")
{
    SECTION("when a crafting step is sized")
    {
        SECTION("should make a bigger job more work and give it longer")
        {
            REQUIRE(PlotResolution::SizeThreshold(StepType::Craft, 0.9, 0.0)
                    > PlotResolution::SizeThreshold(StepType::Craft, 0.2, 0.0));
            REQUIRE(PlotResolution::SizeBudget(StepType::Craft, 0.9)
                    > PlotResolution::SizeBudget(StepType::Craft, 0.2));
        }

        SECTION("should be winnable on a good roll")
        {
            const float threshold = PlotResolution::SizeThreshold(StepType::Craft, 0.5, 0.0);
            REQUIRE(threshold > 0.0f);
            REQUIRE(PlotResolution::MinimumViableBudget(threshold, 1.5, 5.0, 0.45) >= 1);
        }
    }

    SECTION("when a step is eased by its preparations")
    {
        SECTION("should leave its share of the work for each, and nothing for none")
        {
            REQUIRE(PlotResolution::EaseThreshold(20.0f, 0, 0.8) == 20.0f);
            REQUIRE(PlotResolution::EaseThreshold(20.0f, 1, 0.8) == 16.0f);
            REQUIRE(PlotResolution::EaseThreshold(20.0f, 2, 0.8) > 12.79f);
            REQUIRE(PlotResolution::EaseThreshold(20.0f, 2, 0.8) < 12.81f);
        }

        SECTION("should never leave less than a quarter of the work")
        {
            REQUIRE(PlotResolution::EaseThreshold(20.0f, 50, 0.0) == 5.0f);
        }
    }

    SECTION("when a step's preparations ease its chance of a mishap")
    {
        SECTION("should leave their share of it, never under a quarter")
        {
            REQUIRE(PlotResolution::EaseFactor(0, 0.8) == 1.0);
            REQUIRE(PlotResolution::EaseFactor(1, 0.8) == 0.8);
            REQUIRE(PlotResolution::EaseFactor(50, 0.0) == 0.25);
        }

        SECTION("should never catch an agent whose chance they took away")
        {
            PlotResolution::MishapInputs certain;
            certain.chanceBase = 1.0;
            std::uint64_t rng = 1;
            REQUIRE(PlotResolution::RollMishap(certain, rng));
            certain.easing = 0.0;
            for (int i = 0; i < 100; ++i) {
                REQUIRE_FALSE(PlotResolution::RollMishap(certain, rng));
            }
        }
    }

    SECTION("when the person a step is done to is on guard")
    {
        SECTION("should make the step harder, and undo only so much of its preparation")
        {
            REQUIRE(PlotResolution::Pressure(0, 0.8, true, 1.25) == 1.25);
            REQUIRE(PlotResolution::Pressure(1, 0.8, true, 1.25) == 1.0);
            REQUIRE(PlotResolution::Pressure(1, 0.8, false, 1.25) == 0.8);
        }

        SECTION("should raise the chance of a mishap")
        {
            PlotResolution::MishapInputs half;
            half.chanceBase = 0.5;
            half.competence = 0.0;
            half.easing = 2.0;
            std::uint64_t rng = 7;
            for (int i = 0; i < 50; ++i) {
                REQUIRE(PlotResolution::RollMishap(half, rng));
            }
        }

        SECTION("should lower the step's estimated odds, by its agent's being seen and not by more work")
        {
            PlotResolution::Estimate theft;
            theft.type = StepType::Acquire;
            auto guarded = theft;
            guarded.onGuard = true;
            const PlotResolution::Tuning tuning;
            REQUIRE(PlotResolution::EstimateSuccess(guarded, tuning) < PlotResolution::EstimateSuccess(theft, tuning));

            auto workOnly = tuning;
            workOnly.mishapEnabled = false;
            REQUIRE(PlotResolution::EstimateSuccess(guarded, workOnly)
                    == PlotResolution::EstimateSuccess(theft, workOnly));
        }
    }

    SECTION("when a step's odds are estimated")
    {
        const PlotResolution::Tuning tuning;
        PlotResolution::Estimate theft;
        theft.type = StepType::Acquire;
        theft.importance = 0.6;

        SECTION("should give the same answer every time")
        {
            REQUIRE(PlotResolution::EstimateSuccess(theft, tuning) == PlotResolution::EstimateSuccess(theft, tuning));
        }

        SECTION("should rise with each preparation, by more than the eased work alone gives")
        {
            auto prepared = theft;
            prepared.preparations = 2;
            const double before = PlotResolution::EstimateSuccess(theft, tuning);
            const double after = PlotResolution::EstimateSuccess(prepared, tuning);
            REQUIRE(after > before);

            // With no mishaps at all, only the eased work counts.
            auto workOnly = tuning;
            workOnly.mishapEnabled = false;
            const double workBefore = PlotResolution::EstimateSuccess(theft, workOnly);
            const double workAfter = PlotResolution::EstimateSuccess(prepared, workOnly);
            REQUIRE(after - before > workAfter - workBefore);
        }

        SECTION("should fall as the target matters more")
        {
            auto jarl = theft;
            jarl.importance = 1.0;
            auto farmhand = theft;
            farmhand.importance = 0.25;
            REQUIRE(PlotResolution::EstimateSuccess(jarl, tuning) < PlotResolution::EstimateSuccess(farmhand, tuning));
        }

        SECTION("should stay between 0 and 1")
        {
            const double odds = PlotResolution::EstimateSuccess(theft, tuning);
            REQUIRE(odds >= 0.0);
            REQUIRE(odds <= 1.0);
        }
    }

    SECTION("when its type is written and read back")
    {
        SECTION("should come back as itself")
        {
            StepType type = StepType::Locate;
            REQUIRE(PlotModel::ParseStepType(PlotModel::TypeId(StepType::Craft), type));
            REQUIRE(type == StepType::Craft);
        }
    }

    SECTION("when the player is near")
    {
        SECTION("should not hold it, as work at a bench is nothing to hide")
        {
            REQUIRE_FALSE(PlotModel::IsConspicuous(StepType::Craft));
        }
    }
}

TEST_CASE("PlotResolution scales a step's budget", "[PlotResolution][engine]")
{
    SECTION("when the budget is scaled down")
    {
        SECTION("should give a step fewer ticks, in proportion")
        {
            REQUIRE(PlotResolution::SizeBudget(StepType::Discredit, 1.0, 0.5)
                    < PlotResolution::SizeBudget(StepType::Discredit, 1.0));
            REQUIRE(PlotResolution::SizeBudget(StepType::Discredit, 1.0, 1.0)
                    == PlotResolution::SizeBudget(StepType::Discredit, 1.0));
        }

        SECTION("should lower a weak agent's estimated odds, by their running out of time")
        {
            PlotResolution::Estimate weak;
            weak.type = StepType::Discredit;
            weak.importance = 1.0;
            weak.competence = 0.3;
            weak.suitability = 0.3;
            PlotResolution::Tuning roomy;
            roomy.mishapEnabled = false;
            auto tight = roomy;
            tight.budgetScale = 0.5;
            REQUIRE(PlotResolution::EstimateSuccess(weak, roomy, 300) == 1.0);
            REQUIRE(PlotResolution::EstimateSuccess(weak, tight, 300) < 1.0);
        }
    }
}
