// Substrate for the mocked-engine test executable.
//
// CommonLibSSE-NG's headers are full of inline code that reaches two runtime
// facilities the library itself supplies out-of-line: `REX::W32` (its private
// re-declaration of the Win32 API, so its headers need not include windows.h)
// and `REL` (the address library, which maps IDs to addresses inside a loaded
// SkyrimSE.exe). Compiling any production TU against those headers emits
// references to both, whether or not the code under test uses them.
//
// The mocked-engine tests deliberately do NOT link CommonLibSSE.lib — that is
// what lets EngineMock.cpp define the RE:: functions instead. So these symbols
// have to come from somewhere, and this file is that somewhere.
//
// Two different policies here, and the difference matters:
//
//   * REX::W32 forwards to the real Win32 API. These are genuine OS calls with
//     nothing Skyrim-specific about them, and a test process can make them.
//
//   * REL aborts. Reaching the address library means production code tried to
//     call an engine function that nothing has stood in for, and the honest
//     outcome is a loud, named death rather than a plausible-looking default.
//     A test that trips one of these is telling you to add a mock to
//     EngineMock.cpp, and the message says so.
//
// Nothing here is ever compiled into the shipped DLL — this directory is not
// on the plugin target's source list.

#include "RelocationMocks.h"

#include <REL/Relocation.h>
#include <REX/W32/KERNEL32.h>
#include <REX/W32/USER32.h>

#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <vector>

namespace
{
    [[noreturn]] void AbortUnmocked(const char* what)
    {
        std::fprintf(stderr,
                     "\n[EngineMock] FATAL: production code reached '%s'.\n"
                     "  The address library is not available in a test process. Something under\n"
                     "  test called an engine function that no mock stands in for, so CommonLibSSE\n"
                     "  fell through to relocation. Add a definition for that function to\n"
                     "  testsupport/EngineMock.cpp.\n\n",
                     what);
        std::fflush(stderr);
        std::abort();
    }
} // namespace

// ---------------------------------------------------------------------------
// REX::W32 — forward to the real thing
// ---------------------------------------------------------------------------

namespace REX::W32
{
    HMODULE GetCurrentModule() noexcept
    {
        return reinterpret_cast<HMODULE>(::GetModuleHandleW(nullptr));
    }

    HANDLE GetCurrentProcess() noexcept
    {
        return reinterpret_cast<HANDLE>(::GetCurrentProcess());
    }

    std::uint32_t GetEnvironmentVariableW(const wchar_t* a_name, wchar_t* a_buf, std::uint32_t a_bufLen) noexcept
    {
        return ::GetEnvironmentVariableW(a_name, a_buf, a_bufLen);
    }

    std::uint32_t GetModuleFileNameW(HMODULE a_module, wchar_t* a_name, std::uint32_t a_nameLen) noexcept
    {
        return ::GetModuleFileNameW(reinterpret_cast<::HMODULE>(a_module), a_name, a_nameLen);
    }

    HMODULE GetModuleHandleW(const wchar_t* a_name) noexcept
    {
        return reinterpret_cast<HMODULE>(::GetModuleHandleW(a_name));
    }

    std::int32_t MultiByteToWideChar(std::uint32_t a_codePage,
                                     std::uint32_t a_flags,
                                     const char* a_src,
                                     std::int32_t a_srcLen,
                                     wchar_t* a_dst,
                                     std::int32_t a_dstLen) noexcept
    {
        return ::MultiByteToWideChar(a_codePage, a_flags, a_src, a_srcLen, a_dst, a_dstLen);
    }

    std::int32_t WideCharToMultiByte(std::uint32_t a_codePage,
                                     std::uint32_t a_flags,
                                     const wchar_t* a_src,
                                     std::int32_t a_srcLen,
                                     char* a_dst,
                                     std::int32_t a_dstLen,
                                     const char* a_default,
                                     std::int32_t* a_defaultLen)
    {
        return ::WideCharToMultiByte(
            a_codePage, a_flags, a_src, a_srcLen, a_dst, a_dstLen, a_default, reinterpret_cast<::LPBOOL>(a_defaultLen));
    }

    bool TerminateProcess(HANDLE, std::uint32_t) noexcept
    {
        // CommonLibSSE calls this from `SKSE::stl::report_and_fail`. In the game
        // that is the right answer; in a test it would take the whole suite down
        // with no report, so die through the same named path as everything else.
        AbortUnmocked("SKSE::stl::report_and_fail -> TerminateProcess");
    }

    std::int32_t MessageBoxW(HWND, const wchar_t* a_text, const wchar_t*, std::uint32_t) noexcept
    {
        std::fprintf(stderr, "[EngineMock] report_and_fail: %ls\n", a_text ? a_text : L"(no message)");
        return 0;
    }
} // namespace REX::W32

// ---------------------------------------------------------------------------
// REL — the address library, which a test process has no business reaching
// ---------------------------------------------------------------------------

namespace REL
{
    // Definitions for the two singletons CommonLibSSE declares but defines in
    // its .lib. A static data member definition sits in class scope, so it can
    // reach the private default constructors these two have.
    Module Module::_instance;
    IDDatabase IDDatabase::_instance;

    void Module::load_segments()
    {
        AbortUnmocked("REL::Module::load_segments");
    }

    // Called by Module::mock() before it fills the singleton in, and by
    // IDDatabase's destructor. Both are reachable in a test, and neither has
    // anything to release here: nothing was ever loaded or mapped. No-ops
    // rather than aborts, unlike the loaders above.
    void Module::clear() {}
} // namespace REL

namespace REL::detail
{
    void memory_map::close() {}
} // namespace REL::detail

namespace REL
{

    // Serve a synthetic address table instead of a real address-library file.
    //
    // These are member definitions, which is the whole trick: `mapping_t` and
    // `_id2offset` are both private, and only a member may name them.
    //
    // The module base is zero (EngineMock's REL::Module::mock() leaves it
    // there), so each "offset" below is the absolute address of one of our
    // stand-ins and `REL::Relocation` resolves straight to it. The table must
    // be sorted by id because `id2offset` binary-searches it. See
    // RelocationMocks.h for the reasoning and the limits.
    bool IDDatabase::load_file(stl::zwstring, Version, std::uint8_t, bool)
    {
        static std::vector<mapping_t> table = [] {
            std::vector<mapping_t> t;
            for (const auto& mock : NarrativeEngine::Testing::RelocationMockTable()) {
                t.push_back(mapping_t{mock.id, static_cast<std::uint64_t>(mock.address)});
            }
            // Off VR, id2offset does NOT check that the entry its binary
            // search landed on carries the id it was asked for -- it just
            // returns that entry's offset. An unregistered id would therefore
            // resolve to some neighbouring stand-in and call it with the wrong
            // signature, which is a silent crash rather than a diagnosis.
            //
            // Poisoning closes that hole completely. For every registered id X
            // we also register X-1 pointing at the abort handler. Ask for an
            // unregistered Y and the search cannot reach a real entry: any Y
            // below a real X satisfies Y <= X-1, so the poison at X-1 is found
            // first. Ask for a registered X and the exact match still wins.
            // A final sentinel catches every id above the whole table.
            const auto poison = reinterpret_cast<std::uint64_t>(&NarrativeEngine::Testing::UnregisteredRelocation);
            std::vector<std::uint64_t> registered;
            registered.reserve(t.size());
            for (const auto& entry : t)
                registered.push_back(entry.id);
            for (const auto id : registered) {
                // Skip when X-1 is itself registered, which would shadow it.
                if (id > 0 && std::find(registered.begin(), registered.end(), id - 1) == registered.end()) {
                    t.push_back(mapping_t{id - 1, poison});
                }
            }
            t.push_back(mapping_t{(std::numeric_limits<std::uint64_t>::max)(), poison});
            std::sort(t.begin(), t.end(), [](const mapping_t& a, const mapping_t& b) { return a.id < b.id; });
            return t;
        }();
        _id2offset = std::span<mapping_t>(table);
        return true;
    }

    bool IDDatabase::load_csv(stl::zwstring a_filename, Version a_version, bool a_failOnError)
    {
        // The VR path into the same table.
        return load_file(a_filename, a_version, 1, a_failOnError);
    }

    std::optional<Version> GetFileVersion(stl::zwstring)
    {
        AbortUnmocked("REL::GetFileVersion");
    }
} // namespace REL
