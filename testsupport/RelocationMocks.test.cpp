#include "RelocationMocks.h"

#include "EngineMock.h"

#include <catch2/catch_test_macros.hpp>

#include <RE/Skyrim.h>

#include <cstdint>
#include <string>

// A self-test for the relocation layer, not for any production module.
//
// It lives beside the harness rather than in src/ because it has no production
// counterpart to sit next to: what it exercises is testsupport itself. If these
// fail, every mocked-engine test that touches a string or allocates an engine
// object is standing on sand, and the failure will otherwise show up as a
// segfault somewhere unrelated.
//
// See RelocationMocks.h for how a relocated inline function gets intercepted.

namespace
{
    using NarrativeEngine::Testing::EngineMock;
} // namespace

TEST_CASE("RelocationMocks stand in for BSFixedString", "[testsupport]")
{
    // The layer only answers while a mock is installed, because EngineMock is
    // what sets the module base the relocation arithmetic depends on.
    EngineMock engine;

    SECTION("when a string is constructed from a literal")
    {
        const RE::BSFixedString name{"Ysolda"};

        SECTION("should read back the text it was given")
        {
            REQUIRE(std::string{name.c_str()} == "Ysolda");
        }

        SECTION("should report the right length")
        {
            // length() reads the pool-entry header sitting immediately before
            // the character data, so getting this right proves the stand-in
            // reproduced the layout rather than just parking a pointer.
            REQUIRE(name.length() == 6);
        }
    }

    SECTION("when a string is empty")
    {
        const RE::BSFixedString empty{""};

        SECTION("should read back as empty")
        {
            REQUIRE(std::string{empty.c_str()}.empty());
        }
    }

    SECTION("when a string is copied")
    {
        const RE::BSFixedString original{"Carlotta"};
        const RE::BSFixedString copy{original};

        SECTION("should leave both usable")
        {
            // The copy constructor is inline engine code that increments the
            // header's refcount, and each destructor decrements it. A stand-in
            // that ignored the refcount would free the block on the first
            // destruction and leave the other dangling.
            REQUIRE(std::string{original.c_str()} == "Carlotta");
            REQUIRE(std::string{copy.c_str()} == "Carlotta");
        }
    }

    SECTION("when a string goes out of scope")
    {
        SECTION("should release without faulting")
        {
            {
                const RE::BSFixedString scoped{"Riverwood"};
            }
            SUCCEED("destructor ran through the mocked pool release");
        }
    }
}

TEST_CASE("RelocationMocks stand in for the memory manager", "[testsupport]")
{
    EngineMock engine;

    SECTION("when an engine type is heap-allocated")
    {
        SECTION("should allocate through the mocked allocator")
        {
            // Engine types redefine operator new to route through Bethesda's
            // allocator rather than the CRT's, so this is the path every
            // `new` on an RE:: type takes. MakeFunctionArguments is the
            // reason it matters here: the Papyrus dispatch cannot build its
            // argument list without it.
            auto* args = RE::MakeFunctionArguments(std::int32_t{7});
            REQUIRE(args != nullptr);
            delete args;
        }
    }
}
