#pragma once

#include <cstdint>
#include <span>

// Stand-ins for CommonLibSSE functions the linker cannot reach.
//
// The mocked-engine harness replaces out-of-line CommonLibSSE functions by
// simply defining them, because those are ordinary symbols the linker resolves.
// A second, quieter kind exists: functions declared INLINE in a CommonLibSSE
// header whose body resolves an address through the address library at RUNTIME:
//
//     inline BSFixedString* ctor8(const char* a_data)
//     {
//         using func_t = decltype(&BSFixedString::ctor8);
//         REL::Relocation<func_t> func{ RELOCATION_ID(67819, 69161) };
//         return func(this, a_data);
//     }
//
// Nothing about that is a link-time symbol, so the linker never mentions it and
// the usual approach cannot touch it. It surfaces only when a test runs and the
// REL stub aborts. `RE::BSFixedString` is the case that matters most — anything
// naming a Papyrus function, a ModEvent, or a form editor ID builds one.
//
// HOW THE SUBSTITUTION WORKS
//
// `REL::Relocation` resolves an ID to `Module::base() + IDDatabase::id2offset(id)`.
// Three facts make that interceptable:
//
//   * `REL::Module::mock()` sets the module base, and EngineMock calls it with
//     the default base of 0.
//   * `mapping_t::offset` is a full 64 bits, not the 32-bit offset the on-disk
//     address library uses.
//   * `IDDatabase::_id2offset` is a plain `std::span`, and
//     CommonLibSSERuntimeStubs.cpp defines `IDDatabase::load_file` itself — a
//     member definition, so it can assign that private span.
//
// So with base 0, an "offset" IS an absolute address, and pointing one at a
// function of ours makes `Relocation` resolve to it. No executable memory, no
// hand-assembled thunks: the table below is `{id, (uintptr_t)&OurFunction}`.
// CommonLibSSE's own source notes that "member functions == free functions in
// x64", which is why a free function taking the object as its first argument
// stands in correctly for a relocated member.
//
// LIMITS WORTH KNOWING
//
// `id2offset` binary-searches the table and, off VR, does NOT verify that the
// entry it landed on has the id it asked for. An id nobody registered that
// sorts between two registered ones therefore resolves to the wrong function
// and calls it. A sentinel catches every id above the highest registered one,
// which is the common shape, but the gap case is real: keep this table small,
// and treat a mysterious crash inside engine code as a missing registration.
namespace NarrativeEngine::Testing
{
    struct RelocationMock
    {
        // The address-library id for one runtime. A function used on both SE
        // and AE is registered twice, once per id, because which one
        // `RELOCATION_ID` asks for depends on the runtime EngineMock claims.
        std::uint64_t id;

        // Address of the stand-in, stored as the "offset" the database hands
        // back. Absolute, because the mocked module base is zero.
        std::uintptr_t address;
    };

    // Every registered stand-in, in no particular order. The database sorts a
    // copy by id, which is what the binary search in `id2offset` requires.
    std::span<const RelocationMock> RelocationMockTable();

    // Called when a relocation resolves past everything registered. Aborts with
    // a message naming the situation, because the alternative is jumping into
    // whatever happened to be next in the table.
    [[noreturn]] void UnregisteredRelocation();
} // namespace NarrativeEngine::Testing
