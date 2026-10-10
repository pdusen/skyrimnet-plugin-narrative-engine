#include <SKSECosaveIO.h>

#include <EngineMock.h>

#include <SKSE/Interfaces.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// Mocked-engine tests for the production ICosaveIO binding.
//
// This is the adapter that was deliberately left untested when the port was
// introduced, on the grounds that a three-line transcription has nothing in it
// to be wrong. That reasoning was about effort, not reachability: SKSE's
// WriteRecordData, ReadRecordData and ResolveFormID are all out-of-line in
// CommonLibSSE.lib, so the harness stands in for them and the adapter runs here
// unmodified.
//
// What is worth pinning is not the forwarding itself but the two things around
// it: that each call reaches SKSE with the arguments it was given, and that a
// null interface degrades instead of crashing inside a save handler.

namespace
{
    using NarrativeEngine::SKSECosaveIO;
    using NarrativeEngine::Testing::EngineMock;

    // A non-null interface pointer. Every mocked SKSE method answers out of
    // EngineMock and never reads through `this`, so the adapter only needs an
    // address to decide it has an interface at all.
    SKSE::SerializationInterface* FakeInterface()
    {
        alignas(16) static std::byte storage[64]{};
        return reinterpret_cast<SKSE::SerializationInterface*>(storage);
    }

    std::string AsText(const std::vector<std::byte>& bytes)
    {
        return std::string{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    }
} // namespace

TEST_CASE("SKSECosaveIO::WriteBytes", "[SKSECosaveIO][engine]")
{
    // Happy path, re-run per leaf: an interface that accepts writes. Cases
    // override either the interface or SKSE's answer, never both.
    EngineMock engine;
    SKSECosaveIO io{FakeInterface()};
    const char payload[] = "record";

    SECTION("when the interface accepts the write")
    {
        const bool ok = io.WriteBytes(payload, 6);

        SECTION("should report success")
        {
            REQUIRE(ok);
        }

        SECTION("should hand SKSE exactly the bytes it was given")
        {
            REQUIRE(AsText(engine.cosave.written) == "record");
        }
    }

    SECTION("when SKSE refuses the write")
    {
        // A refused write means a truncated record on disk. The adapter has to
        // report it rather than swallow it, even though its own callers
        // currently ignore the result.
        engine.cosave.writeSucceeds = false;

        SECTION("should report failure")
        {
            REQUIRE_FALSE(io.WriteBytes(payload, 6));
        }
    }

    SECTION("when there is no interface")
    {
        SKSECosaveIO nullIo{nullptr};

        SECTION("should report failure rather than dereference it")
        {
            // SKSE is set to accept writes, so a false answer here can only be
            // the null guard firing.
            REQUIRE_FALSE(nullIo.WriteBytes(payload, 6));
        }

        SECTION("should not reach SKSE at all")
        {
            (void)nullIo.WriteBytes(payload, 6);
            REQUIRE(engine.cosave.written.empty());
        }
    }
}

TEST_CASE("SKSECosaveIO::ReadBytes", "[SKSECosaveIO][engine]")
{
    EngineMock engine;
    SKSECosaveIO io{FakeInterface()};
    const char* source = "record";
    engine.cosave.readable.assign(reinterpret_cast<const std::byte*>(source),
                                  reinterpret_cast<const std::byte*>(source) + 6);
    char buffer[16]{};

    SECTION("when the record holds enough bytes")
    {
        const std::uint32_t got = io.ReadBytes(buffer, 6);

        SECTION("should report the full count")
        {
            REQUIRE(got == 6);
        }

        SECTION("should fill the caller's buffer")
        {
            REQUIRE(std::string(buffer, 6) == "record");
        }
    }

    SECTION("when the record is shorter than the request")
    {
        // The short count is the entire failure-detection mechanism for
        // everything that reads a co-save: callers compare what they got
        // against what they asked for, so passing it through unchanged is the
        // adapter's one real job here.
        SECTION("should report the count actually read")
        {
            REQUIRE(io.ReadBytes(buffer, 32) == 6);
        }
    }

    SECTION("when the record is exhausted")
    {
        (void)io.ReadBytes(buffer, 6);

        SECTION("should report zero")
        {
            REQUIRE(io.ReadBytes(buffer, 4) == 0);
        }
    }

    SECTION("when there is no interface")
    {
        SKSECosaveIO nullIo{nullptr};

        SECTION("should report an empty read")
        {
            // Bytes are available, so a zero here is the guard rather than an
            // exhausted stream.
            REQUIRE(nullIo.ReadBytes(buffer, 6) == 0);
        }
    }
}

TEST_CASE("SKSECosaveIO::ResolveFormID", "[SKSECosaveIO][engine]")
{
    EngineMock engine;
    const SKSECosaveIO io{FakeInterface()};
    constexpr std::uint32_t kOldFormID = 0x0001A6A0u;
    std::uint32_t resolved = 0u;

    SECTION("when the form still resolves")
    {
        const bool ok = io.ResolveFormID(kOldFormID, resolved);

        SECTION("should report success")
        {
            REQUIRE(ok);
        }

        SECTION("should write back the id SKSE gave")
        {
            // Not the id that went in: a load order that moved gives the form a
            // different id, and passing the old one through would silently
            // attach saved state to whatever now owns it.
            REQUIRE(resolved == engine.cosave.resolvedFormID);
            REQUIRE(resolved != kOldFormID);
        }

        SECTION("should ask about the id it was given")
        {
            REQUIRE(engine.cosave.resolveRequests.size() == 1);
            REQUIRE(engine.cosave.resolveRequests[0] == kOldFormID);
        }
    }

    SECTION("when the form no longer resolves")
    {
        engine.cosave.resolveSucceeds = false;

        SECTION("should report failure")
        {
            REQUIRE_FALSE(io.ResolveFormID(kOldFormID, resolved));
        }

        SECTION("should leave the caller's id untouched")
        {
            (void)io.ResolveFormID(kOldFormID, resolved);
            REQUIRE(resolved == 0u);
        }
    }

    SECTION("when there is no interface")
    {
        const SKSECosaveIO nullIo{nullptr};

        SECTION("should report failure")
        {
            REQUIRE_FALSE(nullIo.ResolveFormID(kOldFormID, resolved));
        }

        SECTION("should not reach SKSE at all")
        {
            (void)nullIo.ResolveFormID(kOldFormID, resolved);
            REQUIRE(engine.cosave.resolveRequests.empty());
        }
    }
}
