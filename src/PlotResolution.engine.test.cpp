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
