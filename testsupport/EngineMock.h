#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace RE
{
    class Calendar;
    class PlayerCharacter;
    class ScriptEventSourceHolder;
    class UI;
} // namespace RE

// EngineMock — stands in for the CommonLibSSE functions production code calls,
// so a test can drive engine-coupled code with no Skyrim process.
//
// How this works, because it is not the usual mocking story:
//
// Most of CommonLibSSE is header-only — struct layouts, member offsets, and
// inline helpers all compile straight into our translation unit. Only some
// functions are out-of-line, and those live in CommonLibSSE.lib, where each one
// looks its address up in the loaded SkyrimSE.exe through the address library.
// Those are the functions a test process cannot run, and they are exactly the
// ones we replace.
//
// The mocked-engine test target therefore compiles production sources against
// the REAL CommonLibSSE headers and then simply does not link CommonLibSSE.lib.
// testsupport/EngineMock.cpp provides its own definitions for the engine
// functions instead, and the MSVC linker takes them. Nothing about production
// code changes — `EngineUtils.cpp` is compiled here byte-for-byte as the DLL
// compiles it, calls `RE::Calendar::GetSingleton()` exactly as it always did,
// and reaches our definition rather than Bethesda's.
//
// The linker is the safety net. Any engine function the code under test touches
// that nobody has mocked is an unresolved external and the build fails by name.
// There is no way to accidentally call into real engine code, and no way to
// silently miss a dependency.
//
// Usage — construct one, set what the engine should report, call the code:
//
//     EngineMock engine;
//     engine.calendar.hoursPassed = 42.5f;
//     REQUIRE(EngineUtils::GetCurrentGameHours() == 42.5);
//
// Construction installs the mock and destruction removes it, so ordinary scope
// is the reset. Only one may be alive at a time.
namespace NarrativeEngine::Testing
{
    // Which Skyrim build CommonLibSSE should believe it is running against.
    // This is not cosmetic: CommonLibSSE branches on it inside inline header
    // code, so it decides which of several real code paths production takes —
    // `ScriptEventSourceHolder::AsTESFastTravelEndEventSource()` returns null on
    // VR and a relocated member everywhere else, which is the entire reason
    // EngineUtils wraps it.
    enum class Runtime
    {
        SE,
        AE,
        VR
    };

    class EngineMock
    {
    public:
        explicit EngineMock(Runtime runtime = Runtime::AE);
        ~EngineMock();

        EngineMock(const EngineMock&) = delete;
        EngineMock& operator=(const EngineMock&) = delete;

        // `present == false` makes the corresponding GetSingleton() return
        // nullptr — the "engine subsystem isn't up yet" state that happens
        // during early plugin lifecycle and is otherwise impossible to
        // reproduce deliberately.
        struct CalendarState
        {
            bool present = true;
            float hoursPassed = 0.0f;
            int getHoursPassedCalls = 0;
        } calendar;

        struct UIState
        {
            bool present = true;
            bool gameIsPaused = false;
            std::vector<std::string> openMenus;
            std::vector<std::string> isMenuOpenQueries;
        } ui;

        struct PlayerState
        {
            bool present = true;
            bool inCombat = false;
        } player;

        struct EventSourceState
        {
            bool holderPresent = true;
        } events;

        Runtime runtime() const
        {
            return runtime_;
        }

        // The installed instance, or nullptr when none is alive. Used by the
        // mocked engine functions in EngineMock.cpp.
        static EngineMock* Current();

    private:
        Runtime runtime_;
    };
} // namespace NarrativeEngine::Testing
