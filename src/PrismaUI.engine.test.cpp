#include <PrismaUI.h>

#include <FakePrismaUI.h>

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <cstdint>
#include <string>

// Tests for the wrapper over PrismaUI's runtime-loaded modder API.
//
// The wrapper's whole job is to be safe when PrismaUI is not installed. It is a
// soft dependency, so most users of the mod run without it, and every function
// here has to no-op rather than call through a null interface pointer. That
// half is the one worth pinning: a missed guard is a crash on startup for
// everyone who does not have the dependency, and it is invisible to anyone who
// does.
//
// There is no seam between the wrapper and the DLL — the upstream header does
// GetModuleHandle("PrismaUI.dll") and GetProcAddress("RequestPluginAPI")
// inline. So instead of pretending there is one, the harness builds a real DLL
// under that name (testsupport/FakePrismaUI.cpp, built as the PrismaUIFake
// target beside the test executable) and lets the wrapper find it exactly as it
// would in game. That covers the interface-request handshake itself, which is
// the part most likely to break when PrismaUI changes.
//
// One constraint shapes the file. Initialize caches its result in a process
// global with no way to unset it, which is right for the plugin — PrismaUI
// cannot appear halfway through a session — but means a process gets exactly
// one answer, and Catch2 does not run test cases in declaration order by
// default. So the uninitialised case skips itself, loudly, if something in the
// same process got there first, rather than quietly asserting nothing. Under
// ctest it never skips: catch_discover_tests gives every TEST_CASE its own
// process, which is how the suite is actually run.

namespace
{
    namespace Prisma = NarrativeEngine::PrismaUI_API;
    using NarrativeEngine::Testing::FakePrismaState;
    using NarrativeEngine::Testing::FakePrismaStateFunc;
    using NarrativeEngine::Testing::kFakePrismaStateExport;
    using Prisma::kInvalidView;

    // Loads the stand-in DLL and hands back its state. Idempotent: LoadLibrary
    // on an already-loaded module just bumps the refcount, and the wrapper's
    // own GetModuleHandle then finds it.
    FakePrismaState& LoadFakePrismaUI()
    {
        HMODULE module = ::LoadLibraryW(L"PrismaUI.dll");
        REQUIRE(module != nullptr);
        auto* accessor = reinterpret_cast<FakePrismaStateFunc>(
            reinterpret_cast<void*>(::GetProcAddress(module, kFakePrismaStateExport)));
        REQUIRE(accessor != nullptr);
        auto* state = accessor();
        REQUIRE(state != nullptr);
        return *state;
    }

    // A handle no view was ever created under, for the invalid-handle guards.
    constexpr Prisma::ViewHandle kStrayView = 0xDEADBEEFu;
} // namespace

TEST_CASE("PrismaUI before anything has loaded it", "[PrismaUI][engine]")
{
    // This is the only case that needs the wrapper to hold no interface, and
    // Initialize cannot be undone. Skipping rather than asserting keeps a
    // shuffled single-process run honest: the case reports that it did not run,
    // instead of passing on guards it never reached.
    if (Prisma::IsAvailable()) {
        SKIP("PrismaUI was already initialised in this process; run under ctest, which isolates each case");
    }

    SECTION("when PrismaUI is not installed")
    {
        SECTION("should say it is unavailable")
        {
            REQUIRE_FALSE(Prisma::IsAvailable());
        }

        SECTION("should refuse to create a view")
        {
            REQUIRE(Prisma::CreateView("dashboard/index.html") == kInvalidView);
        }

        SECTION("should call no view a live one")
        {
            REQUIRE_FALSE(Prisma::IsValid(kStrayView));
        }

        SECTION("should report every view as hidden")
        {
            // The default that makes a dashboard toggle behave: toggling a view
            // that does not exist must not read as "it is showing, hide it".
            REQUIRE(Prisma::IsHidden(kStrayView));
        }

        SECTION("should report nothing focused")
        {
            // Read by the input path to decide whether the game should swallow
            // a keypress. Answering true with no PrismaUI would eat the
            // player's input for the rest of the session.
            REQUIRE_FALSE(Prisma::HasAnyActiveFocus());
        }

        SECTION("should let every void wrapper be called safely")
        {
            // None of these can report anything; the point is that a plugin
            // running without the soft dependency reaches all of them during
            // normal operation and none may dereference the null interface.
            Prisma::Destroy(kStrayView);
            Prisma::Show(kStrayView);
            Prisma::Hide(kStrayView);
            Prisma::Focus(kStrayView, true, true);
            Prisma::Unfocus(kStrayView);
            Prisma::InvokeJS(kStrayView, "updateFullState", "{}");
            Prisma::RegisterJSListener(kStrayView, "onAction", [](const char*) {});
            SUCCEED("every wrapper is safe with no interface");
        }
    }
}

TEST_CASE("PrismaUI::Initialize", "[PrismaUI][engine]")
{
    // Deliberately does NOT reset the fake. Initialize performs its handshake
    // exactly once per process and caches the result, so the record of that one
    // handshake is the only evidence there will ever be — zeroing the counters
    // between leaf paths would erase it and leave every assertion below reading
    // a fresh zero.
    auto& fake = LoadFakePrismaUI();

    SECTION("when PrismaUI is installed")
    {
        const bool ok = Prisma::Initialize();

        SECTION("should report success")
        {
            REQUIRE(ok);
        }

        SECTION("should ask for the interface version it implements against")
        {
            // V1 is 0 in the upstream enum. Asking for a version an installed
            // PrismaUI does not have gets a null interface back and silently
            // disables the dashboard, with an info-level line that reads
            // identically to "PrismaUI is not installed" — so the number is
            // worth pinning rather than assuming.
            REQUIRE(fake.interfaceRequests >= 1);
            REQUIRE(fake.lastRequestedVersion == 0);
        }

        SECTION("should then say it is available")
        {
            REQUIRE(Prisma::IsAvailable());
        }
    }

    SECTION("when Initialize is called again")
    {
        REQUIRE(Prisma::Initialize());
        const int afterFirst = fake.interfaceRequests;

        SECTION("should reuse the cached interface")
        {
            // Called from kDataLoaded, which can arrive more than once across a
            // session. Re-running the handshake would not be harmless: it hands
            // back a second interface pointer while views created through the
            // first are still live.
            REQUIRE(Prisma::Initialize());
            REQUIRE(fake.interfaceRequests == afterFirst);
        }
    }
}

TEST_CASE("PrismaUI drives a live view", "[PrismaUI][engine]")
{
    // Happy path, re-run per leaf: PrismaUI installed, interface obtained, one
    // view created. Each case then drives one wrapper and reads back what the
    // DLL was actually told.
    auto& fake = LoadFakePrismaUI();
    fake.Reset();
    REQUIRE(Prisma::Initialize());
    const auto view = Prisma::CreateView("NarrativeEngine/dashboard/index.html");
    REQUIRE(view != kInvalidView);

    SECTION("when a view is created")
    {
        SECTION("should pass the html path through unchanged")
        {
            // PrismaUI resolves this itself, relative to Data/PrismaUI/views.
            // A wrapper that absolutised it first would hand PrismaUI a path it
            // cannot resolve, and the only symptom is a blank dashboard.
            REQUIRE(std::string{fake.lastHtmlPath} == "NarrativeEngine/dashboard/index.html");
        }

        SECTION("should hand back the handle PrismaUI made")
        {
            REQUIRE(view == fake.lastView);
        }
    }

    SECTION("when PrismaUI refuses to create the view")
    {
        fake.createViewFailsWith = 1;

        SECTION("should report an invalid view")
        {
            REQUIRE(Prisma::CreateView("missing.html") == kInvalidView);
        }
    }

    SECTION("when the view is shown and hidden")
    {
        SECTION("should forward both to PrismaUI")
        {
            Prisma::Show(view);
            Prisma::Hide(view);
            REQUIRE(fake.showCalls == 1);
            REQUIRE(fake.hideCalls == 1);
        }
    }

    SECTION("when the view is focused")
    {
        SECTION("should forward both focus flags")
        {
            // Show alone does not render the overlay above the game on most
            // setups; the view needs input focus too. Both flags change what
            // the player sees, so passing either wrongly is a visible bug with
            // no log line.
            Prisma::Focus(view, true, true);
            REQUIRE(fake.focusCalls == 1);
            REQUIRE(fake.lastPauseGame);
            REQUIRE(fake.lastDisableFocusMenu);
        }

        SECTION("should forward flags that are off")
        {
            Prisma::Focus(view, false, false);
            REQUIRE(fake.focusCalls == 1);
            REQUIRE_FALSE(fake.lastPauseGame);
            REQUIRE_FALSE(fake.lastDisableFocusMenu);
        }

        SECTION("should forward the unfocus")
        {
            Prisma::Unfocus(view);
            REQUIRE(fake.unfocusCalls == 1);
        }
    }

    SECTION("when JavaScript is invoked")
    {
        SECTION("should pass the function name and its argument")
        {
            // The dashboard's whole state arrives this way, as one JSON string
            // through a globally-exposed function.
            Prisma::InvokeJS(view, "updateFullState", R"({"phase":"Climax"})");
            REQUIRE(fake.interopCalls == 1);
            REQUIRE(std::string{fake.lastFunctionName} == "updateFullState");
            REQUIRE(std::string{fake.lastArgument} == R"({"phase":"Climax"})");
        }
    }

    SECTION("when a JS listener is registered")
    {
        SECTION("should pass the function name")
        {
            Prisma::RegisterJSListener(view, "onDispatchToggle", [](const char*) {});
            REQUIRE(fake.registerListenerCalls == 1);
            REQUIRE(std::string{fake.lastFunctionName} == "onDispatchToggle");
        }

        SECTION("should refuse a null callback")
        {
            // Handing PrismaUI a null callback would register a listener it
            // calls into on the next UI event.
            Prisma::RegisterJSListener(view, "onDispatchToggle", nullptr);
            REQUIRE(fake.registerListenerCalls == 0);
        }
    }

    SECTION("when the view's state is queried")
    {
        SECTION("should report what PrismaUI says about validity")
        {
            fake.isValidAnswer = false;
            REQUIRE_FALSE(Prisma::IsValid(view));
        }

        SECTION("should report what PrismaUI says about visibility")
        {
            // Set to the opposite of the no-PrismaUI default, so a wrapper that
            // short-circuited would give the other answer.
            fake.isHiddenAnswer = false;
            REQUIRE_FALSE(Prisma::IsHidden(view));
        }

        SECTION("should report what PrismaUI says about focus")
        {
            fake.hasAnyActiveFocusAnswer = true;
            REQUIRE(Prisma::HasAnyActiveFocus());
        }
    }

    SECTION("when the view is destroyed")
    {
        SECTION("should forward it")
        {
            Prisma::Destroy(view);
            REQUIRE(fake.destroyCalls == 1);
        }
    }
}

TEST_CASE("PrismaUI guards an invalid view handle", "[PrismaUI][engine]")
{
    // The other half of every guard. With PrismaUI present, an invalid handle
    // must still stop at the wrapper rather than be forwarded — the handle is
    // whatever a failed CreateView returned, and PrismaUI has no view for it.
    auto& fake = LoadFakePrismaUI();
    fake.Reset();
    REQUIRE(Prisma::Initialize());

    SECTION("when the handle is the invalid one")
    {
        SECTION("should forward none of the view calls")
        {
            Prisma::Destroy(kInvalidView);
            Prisma::Show(kInvalidView);
            Prisma::Hide(kInvalidView);
            Prisma::Focus(kInvalidView, true, true);
            Prisma::Unfocus(kInvalidView);
            Prisma::InvokeJS(kInvalidView, "updateFullState", "{}");
            Prisma::RegisterJSListener(kInvalidView, "onAction", [](const char*) {});
            REQUIRE(fake.destroyCalls == 0);
            REQUIRE(fake.showCalls == 0);
            REQUIRE(fake.hideCalls == 0);
            REQUIRE(fake.focusCalls == 0);
            REQUIRE(fake.unfocusCalls == 0);
            REQUIRE(fake.interopCalls == 0);
            REQUIRE(fake.registerListenerCalls == 0);
        }

        SECTION("should call it neither valid nor visible")
        {
            // Both answers are the wrapper's own, not PrismaUI's: the fake is
            // set to say the opposite of each, so an unguarded call through
            // would give the other result.
            fake.isValidAnswer = true;
            fake.isHiddenAnswer = false;
            REQUIRE_FALSE(Prisma::IsValid(kInvalidView));
            REQUIRE(Prisma::IsHidden(kInvalidView));
        }
    }
}
