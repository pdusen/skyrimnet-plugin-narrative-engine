#include <ConsoleCommand.h>

#include <EngineMock.h>

#include <catch2/catch_test_macros.hpp>

#include <string>

// Mocked-engine tests for the programmatic console-command runner.
//
// The module builds a transient Script form through the engine's form factory,
// sets its command text, and compiles it — the same path the `~` console takes
// when the player types. All of that is engine access with no pure core, so the
// engine is mocked and the production source compiles here unchanged.
//
// Two of its three branches are the ones a running game essentially never
// shows: a missing form factory and a factory that refuses to build. Both are
// documented as shutdown-only, which is another way of saying nobody has ever
// seen them work.
//
// This is also the harness's first fabricated object that takes a VIRTUAL call.
// `ConcreteFormFactory::Create()` forwards to the virtual `CreateImpl()`, and
// the Script form is later `delete`d through its virtual destructor, so both
// objects are built with real vtables. See testsupport/FakeVTable.h.

namespace
{
    using NarrativeEngine::Testing::EngineMock;
    namespace ConsoleCommand = NarrativeEngine::ConsoleCommand;

    constexpr std::string_view kCommand = "startquest _ne_VisitQuest";
} // namespace

TEST_CASE("ConsoleCommand::Run", "[ConsoleCommand][engine]")
{
    // Happy path, re-run per leaf: a form factory that exists and builds. Each
    // failure case below removes exactly one of those.
    EngineMock engine;

    SECTION("when the engine builds the script")
    {
        const bool ran = ConsoleCommand::Run(kCommand);

        SECTION("should report success")
        {
            REQUIRE(ran);
        }

        SECTION("should give the script the command it was asked to run")
        {
            REQUIRE(engine.console.commandsSet.size() == 1);
            REQUIRE(engine.console.commandsSet[0] == kCommand);
        }

        SECTION("should compile it once")
        {
            REQUIRE(engine.console.compileTargets.size() == 1);
        }

        SECTION("should compile it against no target reference")
        {
            // A null target is the console's global scope, which is what
            // `startquest`-style commands need. A stray target ref would scope
            // the command to that object and silently do nothing useful.
            REQUIRE(engine.console.compileTargets[0] == nullptr);
        }
    }

    SECTION("when no script form factory is available")
    {
        engine.console.formFactoryPresent = false;

        SECTION("should report failure")
        {
            // The factory would build happily if it existed, so a false answer
            // here can only be the missing-factory guard.
            REQUIRE_FALSE(ConsoleCommand::Run(kCommand));
        }

        SECTION("should not compile anything")
        {
            (void)ConsoleCommand::Run(kCommand);
            REQUIRE(engine.console.compileTargets.empty());
        }
    }

    SECTION("when the factory refuses to build a script")
    {
        engine.console.scriptCreationSucceeds = false;

        SECTION("should report failure")
        {
            REQUIRE_FALSE(ConsoleCommand::Run(kCommand));
        }

        SECTION("should not set a command on anything")
        {
            // The null check has to come before SetCommand, or this path
            // dereferences the form the factory just declined to make.
            (void)ConsoleCommand::Run(kCommand);
            REQUIRE(engine.console.commandsSet.empty());
        }
    }
}
