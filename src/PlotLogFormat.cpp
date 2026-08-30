#include <PlotLog.h>

#include <format>

// The pure half of the plot trace: line formatting, with no file, no
// settings and no engine behind it.
//
// Split out for the reason PlotSerialize and PlotFactionRoster both
// learned: the property worth verifying here is that the format is
// ANALYSABLE — that a run's outcomes can be counted back out of it — and
// that cannot be checked if reaching the formatter requires an open file
// and a running game.
namespace NarrativeEngine::PlotLog
{
    namespace
    {
        // A name that is never silently blank. An anonymous line is worse
        // than a verbose one: it cannot be correlated with anything.
        std::string_view Named(const std::string& name, std::string_view fallback)
        {
            return name.empty() ? fallback : std::string_view(name);
        }
    } // namespace

    std::string FormatTick(std::uint32_t tickNumber, double gameDay, std::size_t activePlots, std::size_t budget)
    {
        return std::format("TICK  #{} day={:.2f} active={}/{}", tickNumber, gameDay, activePlots, budget);
    }

    std::string FormatBorn(const PlotModel::Plot& plot, double weight, std::size_t considered)
    {
        return std::format("BORN  plot={} \"{}\" mastermind=\"{}\" weight={:.2f} considered={} steps={}",
                           plot.id,
                           PlotModel::Title(plot),
                           Named(plot.mastermindName, "?"),
                           weight,
                           considered,
                           plot.plan.size());
    }

    std::string FormatDispatch(const PlotModel::Plot& plot,
                               const PlotModel::Step& step,
                               int rung,
                               std::size_t considered,
                               std::size_t rejected)
    {
        // The four sizing inputs are on this line and nowhere else.
        // Without them a later RESOLVE says what happened but not what
        // the actor was ever up against.
        return std::format("DISPATCH plot={} step=\"{}\" actor=\"{}\" rung={} budget={} threshold={:.1f} "
                           "travel={:.2f} importance={:.2f} competence={:.2f} suitability={:.2f} "
                           "considered={} rejected={}",
                           plot.id,
                           PlotModel::Label(step),
                           Named(step.actorName, "?"),
                           rung,
                           step.budget,
                           step.threshold,
                           step.sizingTravel,
                           step.sizingImportance,
                           step.sizingCompetence,
                           step.sizingSuitability,
                           considered,
                           rejected);
    }

    std::string FormatRoll(const PlotModel::Plot& plot,
                           const PlotModel::Step& step,
                           const PlotModel::TickRecord& record)
    {
        return std::format("ROLL  plot={} step=\"{}\" tick={}/{} added={:.2f} progress={:.2f}/{:.1f} ({:.0f}%){}",
                           plot.id,
                           PlotModel::Label(step),
                           step.elapsed,
                           step.budget,
                           record.progressAdded,
                           record.progressAfter,
                           step.threshold,
                           step.ProgressFraction() * 100.0,
                           record.held ? " HELD" : (record.caught ? " CAUGHT" : ""));
    }

    std::string FormatResolve(const PlotModel::Plot& plot, const PlotModel::Step& step)
    {
        // A whole session's step outcomes are countable from these lines
        // alone, which is the property the step's probe asserts.
        return std::format("RESOLVE plot={} step=\"{}\" outcome={} actor=\"{}\" progress={:.2f}/{:.1f} ({:.0f}%) "
                           "ticks={}/{} held={}",
                           plot.id,
                           PlotModel::Label(step),
                           PlotModel::StepOutcomeId(step.outcome),
                           Named(step.actorName, "?"),
                           step.progress,
                           step.threshold,
                           step.ProgressFraction() * 100.0,
                           step.elapsed,
                           step.budget,
                           step.heldTicks);
    }

    std::string FormatAdapt(const PlotModel::Plot& plot, int adaptations, int cap, std::size_t tail)
    {
        return std::format("ADAPT plot={} \"{}\" adaptation={}/{} newTail={}",
                           plot.id,
                           PlotModel::Title(plot),
                           adaptations,
                           cap,
                           tail);
    }

    std::string FormatEnd(const PlotModel::Plot& plot)
    {
        return std::format("END   plot={} \"{}\" status={} outcome={} mastermind=\"{}\" days={:.2f} "
                           "steps={} adaptations={}",
                           plot.id,
                           PlotModel::Title(plot),
                           PlotModel::PlotStatusId(plot.status),
                           PlotModel::PlotOutcomeId(plot.outcome),
                           Named(plot.mastermindName, "?"),
                           plot.endedOnGameDay - plot.bornOnGameDay,
                           plot.history.size(),
                           plot.adaptations);
    }
} // namespace NarrativeEngine::PlotLog
