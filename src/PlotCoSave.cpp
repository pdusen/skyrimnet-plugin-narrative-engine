#include <PlotSerialize.h>

#include <logger.h>

#include <SKSE/SKSE.h>

// The SKSE half of plot persistence, deliberately in its own translation
// unit.
//
// PlotSerialize.cpp holds the byte format and nothing else: no SKSE, no
// logging, no engine. That separation is not tidiness — it is what lets a
// probe compile and RUN the reader and writer to prove the round trip, which
// is the only property of a save format that matters and the only one that
// cannot be checked from inside a running game without save/quit/reload.
//
// Everything that genuinely needs SKSE lives here, and it is thin enough to
// read in one sitting.
namespace NarrativeEngine::PlotSerialize
{
    bool SkseSink::Write(const void* data, std::size_t size)
    {
        return m_intfc != nullptr && m_intfc->WriteRecordData(data, static_cast<std::uint32_t>(size));
    }

    bool SkseSource::Read(void* data, std::size_t size)
    {
        return m_intfc != nullptr && m_intfc->ReadRecordData(data, static_cast<std::uint32_t>(size)) == size;
    }

    bool SkseSource::ResolveFormID(RE::FormID saved, RE::FormID& out)
    {
        return m_intfc != nullptr && m_intfc->ResolveFormID(saved, out);
    }

    // --- SKSE entry points -------------------------------------------

    void OnSave(SKSE::SerializationInterface* intfc)
    {
        if (intfc == nullptr) {
            return;
        }
        if (!intfc->OpenRecord(kRecordTypeId, kRecordVersion)) {
            logger::error("PlotSerialize::OnSave: OpenRecord failed");
            return;
        }

        // From the published snapshot, never the live state: one
        // consistent instant, and no wait on the plot worker.
        const auto snapshot = Plots::Snapshot();
        SkseSink sink(intfc);
        if (!WriteState(*snapshot, sink)) {
            logger::error("PlotSerialize::OnSave: write failed");
            return;
        }
        logger::info("PlotSerialize::OnSave: wrote {} plot(s)", snapshot->plots.size());
    }

    void OnLoad(SKSE::SerializationInterface* intfc, std::uint32_t version)
    {
        if (intfc == nullptr) {
            return;
        }

        PlotState loaded;
        SkseSource source(intfc);
        std::size_t dropped = 0;
        if (!ReadState(loaded, source, version, dropped)) {
            // ReadState is deliberately silent — it is pure so that it
            // can be probed — so the reason is reconstructed here.
            if (version != kRecordVersion) {
                logger::warn("PlotSerialize::OnLoad: record version {} != {}; discarding", version, kRecordVersion);
            } else {
                logger::warn("PlotSerialize::OnLoad: record malformed or truncated; discarding");
            }
            Plots::StageLoadedState(PlotState{});
            return;
        }
        if (dropped > 0) {
            logger::warn("PlotSerialize::OnLoad: dropped {} plot(s) whose forms no longer resolve", dropped);
        }
        logger::info("PlotSerialize::OnLoad: restored {} plot(s)", loaded.plots.size());
        Plots::StageLoadedState(std::move(loaded));
    }

    void OnRevert()
    {
        Plots::StageLoadedState(PlotState{});
    }
} // namespace NarrativeEngine::PlotSerialize
