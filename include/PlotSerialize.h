#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <PlotState.h>

#include <RE/Skyrim.h>

namespace SKSE
{
    class SerializationInterface;
}

// PlotSerialize — the co-save format for plot state, expressed against a
// byte sink and a byte source rather than against SKSE directly.
//
// The indirection is the whole point of the design. Round-trip fidelity
// is the one property that matters here and it is the one property that
// cannot be checked without running the code, so the reader and the
// writer take an interface a probe can implement over a
// std::vector<std::byte>. The SKSE adapters below are thin enough to
// read in one sitting, which is what makes it reasonable to verify the
// format against the in-memory adapter and trust the other.
//
// FormID resolution sits on the SOURCE rather than being called from
// inside the reader for the same reason: a probe supplies its own
// resolver — identity, or one that deliberately drops a form — and the
// "mastermind no longer resolves" path becomes testable instead of
// theoretical.
//
// See docs/implementation/PHASE_14_FACTION_PLOTS.md step 3.
namespace NarrativeEngine::PlotSerialize
{
    // SKSE co-save record type. Frozen — changing it orphans every
    // previously-saved payload.
    inline constexpr std::uint32_t kRecordTypeId = 'NEPL';

    // Bumped whenever the byte layout changes. A record written by a
    // newer version than the reader understands is DISCARDED rather than
    // guessed at: a half-understood plot is worse than no plot, because
    // it goes on to write memories about a scheme nobody planned.
    //
    // v2 (step 6) adds each step's roll history and the four numbers
    // that sized it. Nothing has shipped, so no migration is owed.
    inline constexpr std::uint32_t kRecordVersion = 2;

    class ByteSink
    {
    public:
        virtual ~ByteSink() = default;
        virtual bool Write(const void* data, std::size_t size) = 0;
    };

    class ByteSource
    {
    public:
        virtual ~ByteSource() = default;
        virtual bool Read(void* data, std::size_t size) = 0;

        // Map a saved FormID onto this session's load order. Returns
        // false when the form is gone — an uninstalled mod, a changed
        // load order — which the reader treats as "drop the plot", not
        // as "keep it and hope".
        virtual bool ResolveFormID(RE::FormID saved, RE::FormID& out) = 0;
    };

    // Write `state` to `sink`. Returns false on the first failed write;
    // a partially written record is the sink's problem to discard.
    bool WriteState(const PlotState& state, ByteSink& sink);

    // Read into `state`. On any failure `state` is left EMPTY rather
    // than partially populated — a truncated record must not resurrect
    // half a world.
    //
    // `droppedPlots` reports how many plots were discarded because a
    // FormID no longer resolved, so the caller can log it rather than
    // having the loss be silent.
    bool ReadState(PlotState& state, ByteSource& source, std::uint32_t version, std::size_t& droppedPlots);

    // --- Adapters ----------------------------------------------------

    // Over a byte vector. Used by probes, and the reason the interfaces
    // exist.
    class VectorSink final : public ByteSink
    {
    public:
        bool Write(const void* data, std::size_t size) override;
        [[nodiscard]] const std::vector<std::uint8_t>& Bytes() const noexcept
        {
            return m_bytes;
        }

    private:
        std::vector<std::uint8_t> m_bytes;
    };

    class VectorSource final : public ByteSource
    {
    public:
        // `resolver` maps a saved FormID to a live one; returning false
        // simulates a form that no longer exists. Defaults to identity.
        using Resolver = bool (*)(RE::FormID, RE::FormID&);

        explicit VectorSource(std::vector<std::uint8_t> bytes, Resolver resolver = nullptr);

        bool Read(void* data, std::size_t size) override;
        bool ResolveFormID(RE::FormID saved, RE::FormID& out) override;

    private:
        std::vector<std::uint8_t> m_bytes;
        std::size_t m_cursor = 0;
        Resolver m_resolver = nullptr;
    };

    // Over SKSE's serialization interface. The record must already be
    // open (writing) or current (reading).
    class SkseSink final : public ByteSink
    {
    public:
        explicit SkseSink(SKSE::SerializationInterface* intfc) : m_intfc(intfc) {}
        bool Write(const void* data, std::size_t size) override;

    private:
        SKSE::SerializationInterface* m_intfc = nullptr;
    };

    class SkseSource final : public ByteSource
    {
    public:
        explicit SkseSource(SKSE::SerializationInterface* intfc) : m_intfc(intfc) {}
        bool Read(void* data, std::size_t size) override;
        bool ResolveFormID(RE::FormID saved, RE::FormID& out) override;

    private:
        SKSE::SerializationInterface* m_intfc = nullptr;
    };

    // --- SKSE entry points -------------------------------------------

    // Writes from the PUBLISHED SNAPSHOT, not the live state, so the
    // saved image is one consistent instant. Never blocks on the plot
    // worker; a save is as fast as writing the bytes.
    void OnSave(SKSE::SerializationInterface* intfc);

    // Reads the current record into the live state and republishes.
    void OnLoad(SKSE::SerializationInterface* intfc, std::uint32_t version);

    // Clears live and published state. The caller cancels outstanding
    // plot work FIRST — see Plugin.cpp.
    void OnRevert();
} // namespace NarrativeEngine::PlotSerialize
