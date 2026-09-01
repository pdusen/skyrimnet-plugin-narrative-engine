#include <PlotSerialize.h>

#include <cstring>
#include <type_traits>

namespace NarrativeEngine::PlotSerialize
{
    namespace
    {
        // Strings are length-prefixed with a 32-bit count. A cap is
        // applied on read so a corrupt length cannot ask for a
        // gigabyte before the read fails.
        constexpr std::uint32_t kMaxStringBytes = 4096;
        // Likewise for collection counts. Generous against
        // iPlotMaxConcurrent and iPlotStepHistoryCap, tight enough that
        // garbage is rejected rather than allocated.
        constexpr std::uint32_t kMaxCollection = 100000;

        template <typename T> bool WritePod(ByteSink& sink, const T& value)
        {
            static_assert(std::is_trivially_copyable_v<T>, "WritePod is for trivially copyable types only");
            return sink.Write(&value, sizeof(T));
        }

        template <typename T> bool ReadPod(ByteSource& source, T& value)
        {
            static_assert(std::is_trivially_copyable_v<T>, "ReadPod is for trivially copyable types only");
            return source.Read(&value, sizeof(T));
        }

        bool WriteString(ByteSink& sink, const std::string& value)
        {
            const auto size = static_cast<std::uint32_t>(value.size());
            if (!WritePod(sink, size)) {
                return false;
            }
            return size == 0 || sink.Write(value.data(), size);
        }

        bool ReadString(ByteSource& source, std::string& value)
        {
            std::uint32_t size{};
            if (!ReadPod(source, size) || size > kMaxStringBytes) {
                return false;
            }
            value.assign(size, '\0');
            return size == 0 || source.Read(value.data(), size);
        }

        // Roll history. Capped on read so a corrupt count cannot ask for
        // an unbounded allocation; a step that genuinely ran for
        // thousands of ticks is already a bug worth failing on.
        constexpr std::uint32_t kMaxRolls = 4096;

        bool WriteRolls(ByteSink& sink, const std::vector<PlotModel::TickRecord>& rolls)
        {
            if (!WritePod(sink, static_cast<std::uint32_t>(rolls.size()))) {
                return false;
            }
            for (const auto& r : rolls) {
                if (!WritePod(sink, r.progressAdded) || !WritePod(sink, r.progressAfter)
                    || !WritePod(sink, static_cast<std::uint8_t>(r.held))
                    || !WritePod(sink, static_cast<std::uint8_t>(r.caught))) {
                    return false;
                }
            }
            return true;
        }

        bool ReadRolls(ByteSource& source, std::vector<PlotModel::TickRecord>& rolls)
        {
            std::uint32_t count{};
            if (!ReadPod(source, count) || count > kMaxRolls) {
                return false;
            }
            rolls.clear();
            rolls.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i) {
                PlotModel::TickRecord r;
                std::uint8_t held{};
                std::uint8_t caught{};
                if (!ReadPod(source, r.progressAdded) || !ReadPod(source, r.progressAfter) || !ReadPod(source, held)
                    || !ReadPod(source, caught)) {
                    return false;
                }
                r.held = held != 0;
                r.caught = caught != 0;
                rolls.push_back(r);
            }
            return true;
        }

        bool WriteStep(ByteSink& sink, const PlotModel::Step& step)
        {
            // The step TYPE goes out as its manifest id, not as the
            // enum's numeric value. Reordering the enum must not
            // silently reinterpret every saved step.
            const std::string typeId(PlotModel::TypeId(step.type));
            return WriteString(sink, typeId) && WritePod(sink, step.target) && WriteString(sink, step.targetName)
                   && WritePod(sink, step.actor) && WriteString(sink, step.actorName)
                   && WritePod(sink, static_cast<std::uint8_t>(step.state))
                   && WritePod(sink, static_cast<std::uint8_t>(step.outcome)) && WritePod(sink, step.budget)
                   && WritePod(sink, step.elapsed) && WritePod(sink, step.threshold) && WritePod(sink, step.progress)
                   && WritePod(sink, step.heldTicks) && WritePod(sink, step.sizingTravel)
                   && WritePod(sink, step.sizingImportance) && WritePod(sink, step.sizingCompetence)
                   && WritePod(sink, step.sizingSuitability) && WriteRolls(sink, step.rolls);
        }

        // `ok` is set false when a FormID in this step no longer
        // resolves. The step is still read (the stream must stay in
        // sync) but the caller drops the plot it belongs to.
        bool ReadStep(ByteSource& source, PlotModel::Step& step, bool& resolved)
        {
            std::string typeId;
            if (!ReadString(source, typeId) || !PlotModel::ParseStepType(typeId, step.type)) {
                return false;
            }

            RE::FormID savedTarget{};
            if (!ReadPod(source, savedTarget)) {
                return false;
            }
            if (savedTarget != 0 && !source.ResolveFormID(savedTarget, step.target)) {
                step.target = 0;
                resolved = false;
            }

            if (!ReadString(source, step.targetName)) {
                return false;
            }

            RE::FormID savedActor{};
            if (!ReadPod(source, savedActor)) {
                return false;
            }
            if (savedActor != 0 && !source.ResolveFormID(savedActor, step.actor)) {
                step.actor = 0;
                resolved = false;
            }

            std::uint8_t state{};
            std::uint8_t outcome{};
            if (!ReadString(source, step.actorName) || !ReadPod(source, state) || !ReadPod(source, outcome)
                || !ReadPod(source, step.budget) || !ReadPod(source, step.elapsed) || !ReadPod(source, step.threshold)
                || !ReadPod(source, step.progress) || !ReadPod(source, step.heldTicks)
                || !ReadPod(source, step.sizingTravel) || !ReadPod(source, step.sizingImportance)
                || !ReadPod(source, step.sizingCompetence) || !ReadPod(source, step.sizingSuitability)
                || !ReadRolls(source, step.rolls)) {
                return false;
            }
            step.state = static_cast<PlotModel::StepState>(state);
            step.outcome = static_cast<PlotModel::StepOutcome>(outcome);
            return true;
        }

        bool WriteStepVector(ByteSink& sink, const std::vector<PlotModel::Step>& steps)
        {
            if (!WritePod(sink, static_cast<std::uint32_t>(steps.size()))) {
                return false;
            }
            for (const auto& step : steps) {
                if (!WriteStep(sink, step)) {
                    return false;
                }
            }
            return true;
        }

        bool ReadStepVector(ByteSource& source, std::vector<PlotModel::Step>& steps, bool& resolved)
        {
            std::uint32_t count{};
            if (!ReadPod(source, count) || count > kMaxCollection) {
                return false;
            }
            steps.clear();
            steps.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i) {
                PlotModel::Step step;
                if (!ReadStep(source, step, resolved)) {
                    return false;
                }
                steps.push_back(std::move(step));
            }
            return true;
        }
    } // namespace

    bool WriteState(const PlotState& state, ByteSink& sink)
    {
        if (!WritePod(sink, state.nextPlotId) || !WritePod(sink, state.simGameDay) || !WritePod(sink, state.rngState)) {
            return false;
        }

        if (!WritePod(sink, static_cast<std::uint32_t>(state.plots.size()))) {
            return false;
        }
        for (const auto& plot : state.plots) {
            const std::string objectiveId(PlotModel::TypeId(plot.objectiveType));
            if (!WritePod(sink, plot.id) || !WritePod(sink, plot.mastermind) || !WriteString(sink, plot.mastermindName)
                || !WriteString(sink, plot.ambition) || !WriteString(sink, objectiveId)
                || !WritePod(sink, plot.objectiveTarget) || !WriteString(sink, plot.objectiveTargetName)
                || !WriteStepVector(sink, plot.plan) || !WritePod(sink, static_cast<std::uint32_t>(plot.cursor))
                || !WriteStepVector(sink, plot.history) || !WritePod(sink, plot.adaptations)
                || !WritePod(sink, static_cast<std::uint8_t>(plot.status))
                || !WritePod(sink, static_cast<std::uint8_t>(plot.outcome)) || !WritePod(sink, plot.bornOnGameDay)
                || !WritePod(sink, plot.endedOnGameDay) || !WriteString(sink, plot.concession)) {
                return false;
            }
        }

        if (!WritePod(sink, static_cast<std::uint32_t>(state.occupancy.size()))) {
            return false;
        }
        for (const auto& [formId, row] : state.occupancy) {
            if (!WritePod(sink, formId) || !WritePod(sink, row.plotId)
                || !WritePod(sink, static_cast<std::uint8_t>(row.role))
                || !WritePod(sink, row.mastermindAvailableAtGameDay) || !WritePod(sink, row.actorAvailableAtGameDay)) {
                return false;
            }
        }

        // Counters are session-scoped diagnostics, deliberately NOT
        // saved: they describe what this session did, and carrying them
        // across a load would make "plots born" a number nobody can
        // reason about.
        return true;
    }

    bool ReadState(PlotState& state, ByteSource& source, std::uint32_t version, std::size_t& droppedPlots)
    {
        state = PlotState{};
        droppedPlots = 0;

        // A record from a version this build does not understand is
        // DISCARDED rather than guessed at. The caller logs it; this
        // function stays free of anything that needs the plugin's
        // logging plumbing, because that is what keeps it probeable.
        if (version != kRecordVersion) {
            return false;
        }

        const auto fail = [&state]() {
            // Leave nothing behind. A truncated record must not
            // resurrect half a world — the plots it did manage to read
            // would go on to write memories about a scheme whose
            // remaining steps were never loaded.
            state = PlotState{};
            return false;
        };

        if (!ReadPod(source, state.nextPlotId) || !ReadPod(source, state.simGameDay)
            || !ReadPod(source, state.rngState)) {
            return fail();
        }

        std::uint32_t plotCount{};
        if (!ReadPod(source, plotCount) || plotCount > kMaxCollection) {
            return fail();
        }

        for (std::uint32_t i = 0; i < plotCount; ++i) {
            PlotModel::Plot plot;
            bool resolved = true;

            std::string objectiveId;
            RE::FormID savedMastermind{};
            if (!ReadPod(source, plot.id) || !ReadPod(source, savedMastermind)) {
                return fail();
            }
            if (!source.ResolveFormID(savedMastermind, plot.mastermind)) {
                plot.mastermind = 0;
                resolved = false;
            }

            std::uint32_t cursor{};
            std::uint8_t plotStatus{};
            std::uint8_t plotOutcome{};
            RE::FormID savedObjectiveTarget{};
            if (!ReadString(source, plot.mastermindName) || !ReadString(source, plot.ambition)
                || !ReadString(source, objectiveId) || !PlotModel::ParseStepType(objectiveId, plot.objectiveType)
                || !ReadPod(source, savedObjectiveTarget)) {
                return fail();
            }
            if (savedObjectiveTarget != 0 && !source.ResolveFormID(savedObjectiveTarget, plot.objectiveTarget)) {
                plot.objectiveTarget = 0;
                resolved = false;
            }
            if (!ReadString(source, plot.objectiveTargetName) || !ReadStepVector(source, plot.plan, resolved)
                || !ReadPod(source, cursor) || !ReadStepVector(source, plot.history, resolved)
                || !ReadPod(source, plot.adaptations) || !ReadPod(source, plotStatus) || !ReadPod(source, plotOutcome)
                || !ReadPod(source, plot.bornOnGameDay) || !ReadPod(source, plot.endedOnGameDay)
                || !ReadString(source, plot.concession)) {
                return fail();
            }
            plot.cursor = cursor;
            plot.status = static_cast<PlotModel::PlotStatus>(plotStatus);
            plot.outcome = static_cast<PlotModel::PlotOutcome>(plotOutcome);

            // A plot whose mastermind is gone has no author. Dropping it
            // is the honest outcome: there is nobody left to adapt the
            // plan, and every memory it would go on to write would name
            // someone who does not exist in this load order.
            if (!resolved) {
                ++droppedPlots;
                continue;
            }
            state.plots.push_back(std::move(plot));
        }

        std::uint32_t occupancyCount{};
        if (!ReadPod(source, occupancyCount) || occupancyCount > kMaxCollection) {
            return fail();
        }
        for (std::uint32_t i = 0; i < occupancyCount; ++i) {
            RE::FormID savedNpc{};
            PlotModel::Occupancy row;
            std::uint8_t role{};
            if (!ReadPod(source, savedNpc) || !ReadPod(source, row.plotId) || !ReadPod(source, role)
                || !ReadPod(source, row.mastermindAvailableAtGameDay)
                || !ReadPod(source, row.actorAvailableAtGameDay)) {
                return fail();
            }
            row.role = static_cast<PlotModel::Role>(role);

            RE::FormID npc{};
            if (!source.ResolveFormID(savedNpc, npc)) {
                // The NPC is gone; so is any reason to hold their slot
                // or their cooldown.
                continue;
            }
            // An occupancy row pointing at a plot that was dropped above
            // would keep an NPC engaged forever in a plot that no longer
            // exists. Free it rather than carrying the leak forward.
            if (row.plotId != 0 && state.FindPlot(row.plotId) == nullptr) {
                row.plotId = 0;
            }
            state.occupancy[npc] = row;
        }

        return true;
    }

    // --- Adapters ----------------------------------------------------

    bool VectorSink::Write(const void* data, std::size_t size)
    {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        m_bytes.insert(m_bytes.end(), bytes, bytes + size);
        return true;
    }

    VectorSource::VectorSource(std::vector<std::uint8_t> bytes, Resolver resolver)
        : m_bytes(std::move(bytes)), m_resolver(resolver)
    {}

    bool VectorSource::Read(void* data, std::size_t size)
    {
        if (m_cursor + size > m_bytes.size()) {
            return false;
        }
        std::memcpy(data, m_bytes.data() + m_cursor, size);
        m_cursor += size;
        return true;
    }

    bool VectorSource::ResolveFormID(RE::FormID saved, RE::FormID& out)
    {
        if (m_resolver != nullptr) {
            return m_resolver(saved, out);
        }
        out = saved;
        return true;
    }
} // namespace NarrativeEngine::PlotSerialize
