#include <SKSECosaveIO.h>

#include <SKSE/Interfaces.h>

// Straight transcription onto SKSE's co-save calls — three forwarding bodies
// with no logic of their own beyond the null guard. That is deliberate: the
// adapter is the part of the persistence path that unit tests cannot reach, so
// everything that could be wrong belongs on the other side of the port, where
// SenderCooldownTable.test.cpp can drive it.

namespace NarrativeEngine
{
    bool SKSECosaveIO::WriteBytes(const void* data, std::uint32_t length)
    {
        return intfc_ != nullptr && intfc_->WriteRecordData(data, length);
    }

    std::uint32_t SKSECosaveIO::ReadBytes(void* out, std::uint32_t length)
    {
        return intfc_ ? intfc_->ReadRecordData(out, length) : 0u;
    }

    bool SKSECosaveIO::ResolveFormID(std::uint32_t oldFormID, std::uint32_t& newFormID) const
    {
        return intfc_ != nullptr && intfc_->ResolveFormID(oldFormID, newFormID);
    }
} // namespace NarrativeEngine
