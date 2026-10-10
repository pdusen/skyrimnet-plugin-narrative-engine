#pragma once

#include <CosaveIO.h>

#include <cstdint>

namespace SKSE
{
    class SerializationInterface;
}

namespace NarrativeEngine
{
    // The production binding of ICosaveIO onto SKSE's real co-save stream.
    //
    // Lives on the DLL side of the seam — this is the one translation unit in
    // the persistence path that includes SKSE, and every persistent subsystem
    // reaches the co-save through it. Construct one inside an SKSE save/load
    // callback, after the caller has opened its record, and hand it to whatever
    // needs to read or write:
    //
    //     if (!intfc->OpenRecord(kRecordTypeId, kRecordVersion)) return;
    //     SKSECosaveIO io{intfc};
    //     table.Serialize(io);
    //
    // Holds the interface pointer without owning it; SKSE owns the stream and
    // it is only valid for the duration of the callback, so an SKSECosaveIO
    // must not outlive the callback that made it.
    //
    // A null interface is tolerated (writes fail, reads come back empty) rather
    // than asserted, because the alternative is a crash inside a save handler.
    // In practice SKSE never passes null and every callback null-checks before
    // constructing this anyway.
    class SKSECosaveIO final : public ICosaveIO
    {
    public:
        explicit SKSECosaveIO(SKSE::SerializationInterface* intfc) : intfc_(intfc) {}

        bool WriteBytes(const void* data, std::uint32_t length) override;
        std::uint32_t ReadBytes(void* out, std::uint32_t length) override;
        bool ResolveFormID(std::uint32_t oldFormID, std::uint32_t& newFormID) const override;

    private:
        SKSE::SerializationInterface* intfc_;
    };
} // namespace NarrativeEngine
