#include <PlotResolution.h>

#include <algorithm>
#include <cmath>

namespace NarrativeEngine::PlotResolution
{
    namespace
    {
        std::uint64_t NextRandom(std::uint64_t& state) noexcept
        {
            std::uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
            return z ^ (z >> 31);
        }

        double NextUniform(std::uint64_t& state) noexcept
        {
            return static_cast<double>(NextRandom(state) >> 11) * (1.0 / 9007199254740992.0);
        }

        // How long this KIND of step takes before travel is considered.
        // A stakeout is inherently slower than handing something over.
        int BaseBudgetTicks(PlotModel::StepType type)
        {
            switch (type) {
            case PlotModel::StepType::Deliver:
                return 2;
            case PlotModel::StepType::Locate:
                return 3;
            case PlotModel::StepType::Acquire:
                return 4;
            case PlotModel::StepType::Conceal:
                return 3;
            case PlotModel::StepType::Suborn:
                return 5;
            case PlotModel::StepType::Sabotage:
                return 4;
            case PlotModel::StepType::Discredit:
                return 6;
            case PlotModel::StepType::Surveil:
                return 6;
            case PlotModel::StepType::Count:
                break;
            }
            return 4;
        }

        // How much work this KIND of step is before the target is
        // considered.
        float BaseThreshold(PlotModel::StepType type)
        {
            switch (type) {
            case PlotModel::StepType::Locate:
                return 8.0f;
            case PlotModel::StepType::Deliver:
                return 6.0f;
            case PlotModel::StepType::Surveil:
                return 10.0f;
            case PlotModel::StepType::Acquire:
                return 14.0f;
            case PlotModel::StepType::Conceal:
                return 9.0f;
            case PlotModel::StepType::Suborn:
                return 16.0f;
            case PlotModel::StepType::Sabotage:
                return 15.0f;
            case PlotModel::StepType::Discredit:
                return 18.0f;
            case PlotModel::StepType::Count:
                break;
            }
            return 10.0f;
        }
    } // namespace

    int MinimumViableBudget(double rollMaxFraction)
    {
        const double ceiling = std::clamp(rollMaxFraction, 0.0001, 1.0);
        return static_cast<int>(std::ceil(1.0 / ceiling));
    }

    int SizeBudget(PlotModel::StepType type, double travelDistanceNorm, double rollMaxFraction)
    {
        const double distance = std::clamp(travelDistanceNorm, 0.0, 1.0);
        // Crossing the province roughly triples the time a step takes.
        // Travel is the only thing that stretches a budget, which is
        // what makes "who is nearest" matter to casting without any rule
        // saying so.
        constexpr double kMaxTravelMultiplier = 3.0;
        const double multiplier = 1.0 + distance * (kMaxTravelMultiplier - 1.0);
        const auto ticks = static_cast<int>(static_cast<double>(BaseBudgetTicks(type)) * multiplier + 0.5);

        // Floored at the fewest ticks in which the threshold could be
        // reached at all. Below that a step is not hard, it is
        // unwinnable - and an unwinnable step fails silently and looks
        // exactly like bad luck, which is the worst way to be wrong.
        return std::max(MinimumViableBudget(rollMaxFraction), ticks);
    }

    float SizeThreshold(PlotModel::StepType type, double targetImportance)
    {
        const double importance = std::clamp(targetImportance, 0.0, 1.0);
        // A jarl is roughly two and a half times the work of a nobody.
        constexpr double kMaxImportanceMultiplier = 2.5;
        const double multiplier = 1.0 + importance * (kMaxImportanceMultiplier - 1.0);
        return static_cast<float>(static_cast<double>(BaseThreshold(type)) * multiplier);
    }

    double Suitability(PlotModel::StepType type, double speech, double sneak, double pickpocket)
    {
        const double sp = std::clamp(speech, 0.0, 1.0);
        const double sn = std::clamp(sneak, 0.0, 1.0);
        const double pp = std::clamp(pickpocket, 0.0, 1.0);

        switch (type) {
        // Talking work.
        case PlotModel::StepType::Suborn:
        case PlotModel::StepType::Discredit:
        case PlotModel::StepType::Deliver:
            return sp;
        // Finding out, which is half asking around and half not being
        // seen asking.
        case PlotModel::StepType::Locate:
            return 0.5 * sp + 0.5 * sn;
        // Watching without being watched.
        case PlotModel::StepType::Surveil:
        case PlotModel::StepType::Conceal:
        case PlotModel::StepType::Sabotage:
            return sn;
        // Taking something off someone.
        case PlotModel::StepType::Acquire:
            return 0.5 * sn + 0.5 * pp;
        case PlotModel::StepType::Count:
            break;
        }
        return 0.5;
    }

    float RollProgress(const RollInputs& inputs, std::uint64_t& rng)
    {
        const double competence = std::clamp(inputs.competence, 0.0, 1.0);
        const double suitability = std::clamp(inputs.suitability, 0.0, 1.0);

        // The actor's contribution: half competence, half fit for this
        // kind of work. Neither alone should dominate — a brilliant mage
        // is not thereby a good burglar, and a natural burglar who is
        // hopeless is still hopeless.
        const double actor = 0.5 * competence + 0.5 * suitability;

        // The random half. An actor's quality shifts the CENTRE of the
        // band they roll in rather than replacing the roll, so a good
        // actor still has bad days and a poor one still has good ones.
        const double roll = NextUniform(rng);
        const double blended = 0.5 * actor + 0.5 * roll;

        const double lo = std::max(0.0, static_cast<double>(inputs.rollMinFraction));
        const double hi = std::max(lo, static_cast<double>(inputs.rollMaxFraction));
        const double fraction = lo + blended * (hi - lo);

        const double threshold = std::max(0.0001, static_cast<double>(inputs.threshold));
        const double progress = fraction * threshold;

        // Clamped at both ends against the threshold itself. The floor
        // stops an unblocked step stalling forever; the ceiling stops a
        // step clearing its threshold in one tick, which would collapse
        // the race back into the single roll it replaced.
        const double floorValue = lo * threshold;
        const double ceilValue = hi * threshold;
        return static_cast<float>(std::clamp(progress, floorValue, ceilValue));
    }

    bool RollMishap(const MishapInputs& inputs, std::uint64_t& rng)
    {
        if (!inputs.enabled || !(inputs.chanceBase > 0.0)) {
            return false;
        }
        const double competence = std::clamp(inputs.competence, 0.0, 1.0);

        // Conspicuous work is likelier to be noticed; a capable actor is
        // less likely to be the one it happens to. Competence halves the
        // chance at best rather than eliminating it — nobody is beyond
        // being caught, which is the same clamp-the-ends argument the
        // progress roll makes.
        double chance = inputs.chanceBase;
        if (inputs.conspicuous) {
            chance *= 2.5;
        }
        chance *= 1.0 - 0.5 * competence;

        return NextUniform(rng) < std::clamp(chance, 0.0, 1.0);
    }

    PlotModel::TickRecord AdvanceStep(PlotModel::Step& step, const TickInputs& inputs, std::uint64_t& rng)
    {
        PlotModel::TickRecord record;

        if (step.IsTerminal()) {
            record.progressAfter = step.progress;
            return record;
        }

        // Every path below this point records a tick, so the append is
        // done through a guard rather than repeated at each return.
        struct Recorder
        {
            PlotModel::Step& step;
            PlotModel::TickRecord& record;
            ~Recorder()
            {
                step.rolls.push_back(record);
            }
        } recorder{step, record};

        // `elapsed` advances on EVERY tick, including a held one. A
        // conspicuous step blocked by the player's presence is not
        // paused; it is burning its budget while making no headway,
        // which is what gives the player's presence teeth without
        // needing a rule of its own.
        ++step.elapsed;

        if (inputs.blockedByPresence) {
            record.held = true;
            ++step.heldTicks;
        } else {
            record.progressAdded = RollProgress(inputs.roll, rng);
            step.progress += record.progressAdded;

            // The mishap roll only applies to a tick the actor was
            // actually working. Getting caught while sitting still
            // waiting for the player to leave would be nonsense.
            if (RollMishap(inputs.mishap, rng)) {
                record.caught = true;
                step.state = PlotModel::StepState::Failed;
                step.outcome = PlotModel::StepOutcome::FailedCaught;
                record.progressAfter = step.progress;
                return record;
            }
        }

        record.progressAfter = step.progress;

        if (step.progress >= step.threshold) {
            step.state = PlotModel::StepState::Succeeded;
            step.outcome = PlotModel::StepOutcome::Succeeded;
            return record;
        }

        // Checked AFTER the success test, so a step that reaches its
        // threshold on its very last tick succeeds rather than timing
        // out. The alternative punishes an actor for finishing exactly
        // on schedule.
        if (step.elapsed >= step.budget) {
            step.state = PlotModel::StepState::Failed;
            step.outcome = PlotModel::StepOutcome::FailedTimeout;
        }
        return record;
    }
} // namespace NarrativeEngine::PlotResolution
