#include <LetterSmokeTest.h>

#include <AsyncDispatch.h>
#include <SkyrimNetAPI.h>
#include <logger.h>

// RE/Skyrim.h (pulled in via PCH) doesn't include RE/Offsets.h, only
// RE/Offsets_VTABLE.h. Pull it in explicitly so RE::Offset::TESDescription
// is in scope below. (Namespace is `RE::Offset` singular, not `RE::Offsets`.)
#include <RE/Offsets.h>

// MinHook is the function-entry detour library SkyrimNet (and chattelsys)
// use for engine hooks. It handles the prologue disassembly + trampoline
// construction internally — we just hand it (target, hook, &orig_ptr).
#include <MinHook.h>

// BookMenu::OpenBookMenu is the actual gate through which the rendered
// book body reaches the Scaleform UI — the empirical test against the
// GetDescription hook showed BookMenu does NOT use that public API at
// all. Hooking OpenBookMenu lets us substitute the BSString that's about
// to hit the page renderer.
#include <RE/B/BookMenu.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

namespace NarrativeEngine::LetterSmokeTest
{
    namespace
    {
        // -- Configuration -----------------------------------------------------

        constexpr const char* kBookEditorID         = "_ne_CourierLetter01";
        constexpr const char* kPromptName           = "narrative_engine_letter_smoke";
        // Empty variant ⇒ SkyrimNet's base default LLM (per the plugin
        // manifest: "Without a variant entry, the call falls back to
        // SkyrimNet's default Dialogue LLM").
        constexpr const char* kPromptVariant        = "";
        // Title prefix — DeliverLetterToPlayer appends the iteration
        // counter (e.g. "Test Letter 1", "Test Letter 2", ...) so each
        // loop's letter is visually distinct in the inventory list. The
        // old `kTestLetterTitle` constant kept every iteration showing
        // "Test Letter 1" which made it look like the rename had failed.
        constexpr const char* kTestLetterTitlePrefix = "Test Letter ";
        // Raised from 10s to 15s so the test gives the save-loaded
        // world time to settle (SkyrimNet warm-up, cell streaming, the
        // player getting their bearings) before the first letter pops.
        constexpr int         kInitialDelaySeconds   = 15;
        // After Book Menu closes, sleep this long before STARTING the
        // post-read flow.
        constexpr int         kPostReadDelaySeconds  = 15;
        // Inserted between major state transitions so each step is
        // independently observable in-game: 5s before yanking the letter
        // from inventory, 5s before kicking off the next LLM call, 5s
        // before delivering the next letter after the LLM responds.
        constexpr int         kPreRemovalDelaySeconds  = 5;
        constexpr int         kPreRequestDelaySeconds  = 5;
        constexpr int         kPreDeliveryDelaySeconds = 5;
        // How many leading chars of LLM/cached-body text to dump in logs.
        // Long enough to confirm the salutation + opening sentence arrived;
        // short enough not to flood the log on a 140-word letter.
        constexpr std::size_t kBodyPreviewChars     = 240;

        // -- State -------------------------------------------------------------

        enum class State
        {
            kIdle,
            kRequestingLLM,
            kInPlayerInventory,
            kPostReadCooldown,
        };

        const char* StateName(State s)
        {
            switch (s) {
                case State::kIdle:               return "Idle";
                case State::kRequestingLLM:      return "RequestingLLM";
                case State::kInPlayerInventory:  return "InPlayerInventory";
                case State::kPostReadCooldown:   return "PostReadCooldown";
            }
            return "<unknown>";
        }

        std::mutex       g_stateMutex;
        State            g_state          = State::kIdle;
        RE::FormID       g_testBookFormID = 0;
        std::atomic<int> g_iteration      = 0;
        bool             g_initialized    = false;

        // Diagnostics on the GetDescription hook. We don't log every hook
        // call (the engine fires GetDescription frequently for many forms);
        // instead we accumulate counters and only log when our specific form
        // matches. The counters surface if the hook is alive at all.
        std::atomic<std::uint64_t> g_hookCallCount      = 0;
        std::atomic<std::uint64_t> g_hookMatchCount     = 0;
        std::atomic<std::uint64_t> g_hookEmptyBodyCount = 0;

        // The cached body the GetDescription hook substitutes when the
        // engine reads our test book. Separate mutex so the hook can spin
        // through without contending with the state machine.
        std::mutex  g_letterBodyMutex;
        std::string g_currentLetterBody;

        // -- Forward decls (anonymous-namespace helpers) -----------------------

        void RequestLetter();
        void OnLetterReceived(std::string text);
        void OnPostReadDelayElapsed();

        // Returns a single-line snippet of the body (first N non-newline-
        // collapsed chars + ellipsis if truncated). Used in logs.
        std::string BodyPreview(const std::string& body, std::size_t limit = kBodyPreviewChars)
        {
            std::string out;
            out.reserve(std::min(body.size(), limit) + 4);
            for (char c : body) {
                if (out.size() >= limit) break;
                if (c == '\n' || c == '\r') {
                    if (!out.empty() && out.back() != ' ') out.push_back(' ');
                } else {
                    out.push_back(c);
                }
            }
            if (body.size() > out.size()) {
                out.append("...");
            }
            return out;
        }

        // Wraps the LLM-returned body (or a fallback) in Skyrim's
        // handwritten-font tag. The font wrap is purely a renderer
        // directive, so the LLM doesn't need to emit it — we add it here
        // exactly once, with a consistent trim of leading/trailing
        // whitespace so blank lines from the model don't pad the page.
        std::string WrapInFontTag(std::string body)
        {
            const auto firstNonWs = body.find_first_not_of(" \t\r\n");
            const auto lastNonWs  = body.find_last_not_of(" \t\r\n");
            if (firstNonWs == std::string::npos) {
                body.clear();
            } else {
                body = body.substr(firstNonWs, lastNonWs - firstNonWs + 1);
            }
            return "<font face='$HandwrittenFont'>\n" + body + "\n</font>";
        }

        // -- GetDescription hook ----------------------------------------------
        //
        // TESObjectBOOK actually inherits TWO TESDescription instances:
        //   • the inherited TESDescription at offset 0xA8 — the DESC
        //     subrecord, labeled "Book Text" in the CK (the long body
        //     rendered by BookMenu).
        //   • a member `itemCardDescription` at offset 0x128 — the CNAM
        //     subrecord, labeled "Description" in the CK (the short
        //     tooltip shown on the item card).
        //
        // Both share the same GetDescription(BSString&, TESForm*, uint32_t)
        // function — the `fieldType` argument distinguishes them: 'CSED'
        // (DESC reversed) reads Book Text, 'MANC' (CNAM reversed) reads the
        // tooltip. We only want to substitute when the engine asks for the
        // body, so the hook gates substitution on fieldType == 'CSED'.
        //
        // Whether BookMenu's text path actually goes through
        // TESDescription::GetDescription is the empirical question this
        // smoke test answers — the per-hit log lines below surface either
        // outcome.
        //
        // Implementation: MinHook. SKSE's `trampoline.write_branch<5>` is a
        // call-site patcher (it reads a rel32 from src+N-4 and returns the
        // old destination), not a function-entry detour — using it on a
        // function prologue produces a garbage trampoline pointer that
        // crashes on call. MinHook handles the prologue disassembly +
        // jmp-back trampoline construction internally. Same pattern
        // SkyrimNet itself uses for its AI-package hook (see chattelsys's
        // PackageInjector.cpp).

        constexpr std::uint32_t kFieldType_DESC = 'CSED';  // "DESC" reversed
        constexpr std::uint32_t kFieldType_CNAM = 'MANC';  // "CNAM" reversed

        using GetDescription_t = void (*)(RE::TESDescription*,
                                          RE::BSString&,
                                          RE::TESForm*,
                                          std::uint32_t);

        GetDescription_t g_origGetDescription = nullptr;

        void HookedGetDescription(RE::TESDescription* self,
                                  RE::BSString&       a_out,
                                  RE::TESForm*        a_parent,
                                  std::uint32_t       a_fieldType)
        {
            g_hookCallCount.fetch_add(1, std::memory_order_relaxed);
            if (a_parent && a_parent->GetFormID() == g_testBookFormID) {
                const auto match = g_hookMatchCount.fetch_add(
                    1, std::memory_order_relaxed) + 1;
                // Confirmed empirically: DESC ('CSED') is the long body
                // (CK "Book Text"), CNAM ('MANC') is the short item-card
                // tooltip. SkyrimNet's `book.GetDescription()` Papyrus
                // grab uses DESC, so substituting on DESC successfully
                // feeds the LLM-substituted body into SkyrimNet's
                // event log + decorator/narration paths.
                //
                // BookMenu itself, however, does NOT read the rendered
                // body via TESDescription::GetDescription at all — see
                // the BookMenu::OpenBookMenu hook below for the path
                // that actually feeds the rendered page.
                if (a_fieldType != kFieldType_DESC) {
                    logger::info(
                        "LetterSmokeTest: hook matched test book but "
                        "fieldType=0x{:08X} (expected 'CSED'=0x{:08X}); "
                        "this is NOT the body read — falling through to "
                        "engine (likely a CNAM tooltip query)",
                        a_fieldType, kFieldType_DESC);
                    g_origGetDescription(self, a_out, a_parent, a_fieldType);
                    return;
                }
                std::scoped_lock lock(g_letterBodyMutex);
                if (!g_currentLetterBody.empty()) {
                    a_out = g_currentLetterBody.c_str();
                    logger::info(
                        "LetterSmokeTest: hook fired for test book "
                        "(match #{}, fieldType='CSED'/DESC, body={} chars) "
                        "— substituting cached letter",
                        match, g_currentLetterBody.size());
                    logger::debug(
                        "LetterSmokeTest: substituted body preview: {}",
                        BodyPreview(g_currentLetterBody));
                    return;
                }
                g_hookEmptyBodyCount.fetch_add(1, std::memory_order_relaxed);
                logger::warn(
                    "LetterSmokeTest: hook fired for test book "
                    "(match #{}, fieldType='CSED'/DESC) but cached body "
                    "is EMPTY — falling through to engine description "
                    "(player will see the placeholder text)",
                    match);
            }
            g_origGetDescription(self, a_out, a_parent, a_fieldType);
        }

        void InstallDescriptionHook()
        {
            REL::Relocation<std::uintptr_t> target{ RE::Offset::TESDescription::GetDescription };
            const auto targetAddr = target.address();
            const auto hookAddr   = reinterpret_cast<std::uintptr_t>(&HookedGetDescription);
            logger::info(
                "LetterSmokeTest: installing GetDescription hook via MinHook "
                "(target=0x{:016X}, hook=0x{:016X})",
                targetAddr, hookAddr);

            // MH_Initialize is idempotent under MH_ERROR_ALREADY_INITIALIZED,
            // which lets us coexist with other plugins (or our own future
            // hooks) that also use MinHook.
            const auto initStatus = MH_Initialize();
            if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED) {
                logger::error(
                    "LetterSmokeTest: MH_Initialize failed (status={}); "
                    "hook NOT installed",
                    static_cast<int>(initStatus));
                return;
            }

            const auto createStatus = MH_CreateHook(
                reinterpret_cast<LPVOID>(targetAddr),
                reinterpret_cast<LPVOID>(&HookedGetDescription),
                reinterpret_cast<LPVOID*>(&g_origGetDescription));
            if (createStatus != MH_OK) {
                logger::error(
                    "LetterSmokeTest: MH_CreateHook failed (status={}); "
                    "hook NOT installed",
                    static_cast<int>(createStatus));
                return;
            }

            const auto enableStatus = MH_EnableHook(reinterpret_cast<LPVOID>(targetAddr));
            if (enableStatus != MH_OK) {
                logger::error(
                    "LetterSmokeTest: MH_EnableHook failed (status={}); "
                    "hook created but DISABLED",
                    static_cast<int>(enableStatus));
                return;
            }

            logger::info(
                "LetterSmokeTest: GetDescription hook installed "
                "(orig trampoline=0x{:016X})",
                reinterpret_cast<std::uintptr_t>(g_origGetDescription));
        }

        // -- BookMenu::OpenBookMenu hook --------------------------------------
        //
        // The GetDescription hook covers SkyrimNet's body grab, but the
        // actual BookMenu rendered body comes through a separate engine
        // path: BookMenu::OpenBookMenu(BSString description, ..., Book*,
        // ...). The static function lives at RELOCATION_ID(50122, 51053).
        //
        // We MinHook it, and when the book is our test letter, we hand
        // the original a BSString containing our cached LLM body instead
        // of the placeholder text the engine would have passed.
        //
        // Lifetime note: BookMenu::OpenBookMenu copies the description
        // text into its own Scaleform-side storage before returning (the
        // const-ref param is borrowed only for the duration of the call),
        // so a stack-local substitute BSString is safe.

        using OpenBookMenu_t = void(*)(const RE::BSString&,
                                       const RE::ExtraDataList*,
                                       RE::TESObjectREFR*,
                                       RE::TESObjectBOOK*,
                                       const RE::NiPoint3&,
                                       const RE::NiMatrix3&,
                                       float,
                                       bool);

        OpenBookMenu_t                g_origOpenBookMenu     = nullptr;
        std::atomic<std::uint64_t>    g_openBookMenuCalls    = 0;
        std::atomic<std::uint64_t>    g_openBookMenuMatches  = 0;

        // Flag set when OpenBookMenu fires for our test book; consumed by
        // the MenuOpenCloseEvent sink on Book Menu close so we only fire
        // the post-read flow when the close was for *our* letter. Needed
        // because MenuOpenCloseEvent doesn't carry the book identity.
        std::atomic<bool>             g_testBookMenuOpen     = false;

        void HookedOpenBookMenu(const RE::BSString&      a_description,
                                const RE::ExtraDataList* a_extraList,
                                RE::TESObjectREFR*       a_ref,
                                RE::TESObjectBOOK*       a_book,
                                const RE::NiPoint3&      a_pos,
                                const RE::NiMatrix3&     a_rot,
                                float                    a_scale,
                                bool                     a_useDefaultPos)
        {
            g_openBookMenuCalls.fetch_add(1, std::memory_order_relaxed);

            const auto bookFormID = a_book ? a_book->GetFormID() : 0;
            if (a_book && bookFormID == g_testBookFormID) {
                const auto match = g_openBookMenuMatches.fetch_add(
                    1, std::memory_order_relaxed) + 1;
                // Mark that BookMenu was opened FOR our test book — the
                // MenuOpenCloseEvent sink reads this on the matching
                // close to know it should fire the post-read flow.
                g_testBookMenuOpen.store(true, std::memory_order_relaxed);
                std::scoped_lock lock(g_letterBodyMutex);
                if (!g_currentLetterBody.empty()) {
                    RE::BSString substitute = g_currentLetterBody.c_str();
                    logger::info(
                        "LetterSmokeTest: OpenBookMenu hook fired for test "
                        "book (match #{}, body={} chars) — substituting "
                        "cached letter into the renderer",
                        match, g_currentLetterBody.size());
                    logger::debug(
                        "LetterSmokeTest: OpenBookMenu substituted body "
                        "preview: {}",
                        BodyPreview(g_currentLetterBody));
                    g_origOpenBookMenu(substitute, a_extraList, a_ref, a_book,
                                       a_pos, a_rot, a_scale, a_useDefaultPos);
                    return;
                }
                logger::warn(
                    "LetterSmokeTest: OpenBookMenu matched test book "
                    "(match #{}) but cached body is EMPTY — falling "
                    "through (player will see the original description)",
                    match);
            }
            g_origOpenBookMenu(a_description, a_extraList, a_ref, a_book,
                               a_pos, a_rot, a_scale, a_useDefaultPos);
        }

        void InstallOpenBookMenuHook()
        {
            REL::Relocation<std::uintptr_t> target{ RELOCATION_ID(50122, 51053) };
            const auto targetAddr = target.address();
            const auto hookAddr   = reinterpret_cast<std::uintptr_t>(&HookedOpenBookMenu);
            logger::info(
                "LetterSmokeTest: installing OpenBookMenu hook via MinHook "
                "(target=0x{:016X}, hook=0x{:016X})",
                targetAddr, hookAddr);

            // MH_Initialize is idempotent — InstallDescriptionHook may
            // have already called it.
            const auto initStatus = MH_Initialize();
            if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED) {
                logger::error(
                    "LetterSmokeTest: MH_Initialize failed (status={}); "
                    "OpenBookMenu hook NOT installed",
                    static_cast<int>(initStatus));
                return;
            }

            const auto createStatus = MH_CreateHook(
                reinterpret_cast<LPVOID>(targetAddr),
                reinterpret_cast<LPVOID>(&HookedOpenBookMenu),
                reinterpret_cast<LPVOID*>(&g_origOpenBookMenu));
            if (createStatus != MH_OK) {
                logger::error(
                    "LetterSmokeTest: MH_CreateHook failed for OpenBookMenu "
                    "(status={}); hook NOT installed",
                    static_cast<int>(createStatus));
                return;
            }

            const auto enableStatus = MH_EnableHook(reinterpret_cast<LPVOID>(targetAddr));
            if (enableStatus != MH_OK) {
                logger::error(
                    "LetterSmokeTest: MH_EnableHook failed for OpenBookMenu "
                    "(status={}); hook created but DISABLED",
                    static_cast<int>(enableStatus));
                return;
            }

            logger::info(
                "LetterSmokeTest: OpenBookMenu hook installed "
                "(orig trampoline=0x{:016X})",
                reinterpret_cast<std::uintptr_t>(g_origOpenBookMenu));
        }

        // -- MenuOpenCloseEvent sink ------------------------------------------
        //
        // Replaces the original TESBookReadEvent sink, which silently
        // never fired for our test case. Empirical finding: the engine's
        // TESBookReadEvent is dispatched only for **world-ref** book
        // reads (player activates a book sitting in the world); reading
        // a book *from inventory* takes a different code path that
        // doesn't go through that event source. Our test delivers via
        // AddObjectToContainer and the player reads from inventory, so
        // TESBookReadEvent is the wrong signal entirely.
        //
        // Instead we watch UI::MenuOpenCloseEvent for the "Book Menu"
        // closing. To know whether the close was for *our* test letter
        // (vs. any other book), we set g_testBookMenuOpen inside the
        // OpenBookMenu hook when the book FormID matches, and clear it
        // on close.

        struct BookMenuCloseSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
        {
            RE::BSEventNotifyControl ProcessEvent(
                const RE::MenuOpenCloseEvent*                a_event,
                RE::BSTEventSource<RE::MenuOpenCloseEvent>* /*src*/) override
            {
                if (!a_event) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                if (a_event->menuName != RE::BookMenu::MENU_NAME) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                logger::debug(
                    "LetterSmokeTest: MenuOpenCloseEvent for '{}' "
                    "(opening={})",
                    a_event->menuName.c_str(), a_event->opening);
                if (a_event->opening) {
                    // We don't act on open — that's what the OpenBookMenu
                    // hook is for. We only care about close.
                    return RE::BSEventNotifyControl::kContinue;
                }

                // Book Menu closing. Was it opened for OUR test book?
                const bool wasForTestBook =
                    g_testBookMenuOpen.exchange(false, std::memory_order_relaxed);
                if (!wasForTestBook) {
                    logger::debug(
                        "LetterSmokeTest: BookMenu closed but most recent "
                        "open was not for our test book — ignoring");
                    return RE::BSEventNotifyControl::kContinue;
                }

                logger::info(
                    "LetterSmokeTest: BookMenu closed after a test-book "
                    "open — treating as completed read");

                {
                    std::scoped_lock lock(g_stateMutex);
                    if (g_state != State::kInPlayerInventory) {
                        logger::info(
                            "LetterSmokeTest: stale book-close ignored "
                            "(current state={})",
                            StateName(g_state));
                        return RE::BSEventNotifyControl::kContinue;
                    }
                    g_state = State::kPostReadCooldown;
                    logger::info(
                        "LetterSmokeTest: state -> {}",
                        StateName(g_state));
                }

                logger::info(
                    "LetterSmokeTest: queuing post-read sleep of {}s",
                    kPostReadDelaySeconds);

                AsyncDispatch::EnqueueWork([] {
                    std::this_thread::sleep_for(
                        std::chrono::seconds(kPostReadDelaySeconds));
                    logger::debug(
                        "LetterSmokeTest: post-read delay elapsed; "
                        "marshalling to main thread");
                    AsyncDispatch::MarshalToMainThread([] {
                        OnPostReadDelayElapsed();
                    });
                });
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        BookMenuCloseSink* GetBookMenuCloseSink()
        {
            static BookMenuCloseSink instance;
            return &instance;
        }

        // -- Game-state helpers (main thread) ---------------------------------

        // Returns how many copies of `obj` are currently in the player's
        // inventory. Used to verify Add/Remove actually took. Returns -1 if
        // the call can't be made.
        std::int32_t PlayerInventoryCount(RE::TESBoundObject* obj)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || !obj) {
                return -1;
            }
            return player->GetItemCount(obj);
        }

        RE::TESObjectBOOK* GetTestBook()
        {
            if (g_testBookFormID == 0) {
                logger::warn(
                    "LetterSmokeTest: GetTestBook called before FormID "
                    "was resolved");
                return nullptr;
            }
            auto* form = RE::TESForm::LookupByID(g_testBookFormID);
            if (!form) {
                logger::warn(
                    "LetterSmokeTest: LookupByID(0x{:08X}) returned null",
                    g_testBookFormID);
                return nullptr;
            }
            auto* book = form->As<RE::TESObjectBOOK>();
            if (!book) {
                logger::warn(
                    "LetterSmokeTest: form 0x{:08X} no longer casts to "
                    "TESObjectBOOK (type=0x{:02X})",
                    g_testBookFormID,
                    static_cast<std::uint8_t>(form->GetFormType()));
            }
            return book;
        }

        void DeliverLetterToPlayer()
        {
            logger::debug("LetterSmokeTest: DeliverLetterToPlayer enter");
            auto* book   = GetTestBook();
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!book || !player) {
                logger::warn(
                    "LetterSmokeTest: cannot deliver (book={}, player={})",
                    static_cast<void*>(book), static_cast<void*>(player));
                return;
            }
            const auto countBefore = PlayerInventoryCount(book);

            // Title is iteration-stamped so consecutive deliveries are
            // visually distinct in the inventory list. SetFullName
            // updates both inventory display AND BookMenu title bar.
            char titleBuf[64];
            std::snprintf(titleBuf, sizeof(titleBuf), "%s%d",
                          kTestLetterTitlePrefix, g_iteration.load());
            logger::debug(
                "LetterSmokeTest: SetFullName('{}') on book 0x{:08X}",
                titleBuf, book->GetFormID());
            book->SetFullName(titleBuf);

            std::size_t bodyChars = 0;
            {
                std::scoped_lock lock(g_letterBodyMutex);
                bodyChars = g_currentLetterBody.size();
            }
            logger::debug(
                "LetterSmokeTest: AddObjectToContainer book 0x{:08X} "
                "(cached body={} chars, inventory before={})",
                book->GetFormID(), bodyChars, countBefore);
            player->AddObjectToContainer(book, nullptr, 1, nullptr);

            const auto countAfter = PlayerInventoryCount(book);
            if (countAfter <= countBefore) {
                logger::warn(
                    "LetterSmokeTest: inventory count did NOT increase "
                    "after AddObjectToContainer (before={}, after={}) — "
                    "delivery may have failed",
                    countBefore, countAfter);
            } else {
                logger::info(
                    "LetterSmokeTest: delivered letter (iteration {}, "
                    "inventory {} -> {})",
                    g_iteration.load(), countBefore, countAfter);
            }
        }

        void RemoveLetterFromPlayer()
        {
            logger::debug("LetterSmokeTest: RemoveLetterFromPlayer enter");
            auto* book   = GetTestBook();
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!book || !player) {
                logger::warn(
                    "LetterSmokeTest: cannot remove (book={}, player={})",
                    static_cast<void*>(book), static_cast<void*>(player));
                return;
            }
            const auto countBefore = PlayerInventoryCount(book);
            if (countBefore <= 0) {
                logger::warn(
                    "LetterSmokeTest: inventory count is {} before remove "
                    "— player already discarded the letter?",
                    countBefore);
            }
            player->RemoveItem(
                book,
                1,
                RE::ITEM_REMOVE_REASON::kRemove,
                nullptr,
                nullptr);
            const auto countAfter = PlayerInventoryCount(book);
            if (countAfter >= countBefore && countBefore > 0) {
                logger::warn(
                    "LetterSmokeTest: inventory count did NOT decrease "
                    "after RemoveItem (before={}, after={}) — removal "
                    "may have failed",
                    countBefore, countAfter);
            } else {
                logger::info(
                    "LetterSmokeTest: removed letter from inventory "
                    "({} -> {})",
                    countBefore, countAfter);
            }
        }

        // -- State transitions ------------------------------------------------

        void RequestLetter()
        {
            logger::debug("LetterSmokeTest: RequestLetter enter");
            {
                std::scoped_lock lock(g_stateMutex);
                if (g_state != State::kIdle) {
                    logger::info(
                        "LetterSmokeTest: RequestLetter skipped — "
                        "state is {} (expected Idle)",
                        StateName(g_state));
                    return;
                }
                g_state = State::kRequestingLLM;
                logger::info(
                    "LetterSmokeTest: state -> {}",
                    StateName(g_state));
            }

            const int iteration = ++g_iteration;
            logger::info(
                "LetterSmokeTest: requesting letter from LLM "
                "(iteration {}, prompt='{}', variant='{}', "
                "SkyrimNet available={}, version={})",
                iteration,
                kPromptName,
                kPromptVariant,
                SkyrimNetAPI::IsAvailable(),
                SkyrimNetAPI::GetVersion());

            // Static prompt, no variable substitutions needed, but Inja
            // expects valid JSON. An empty object is fine.
            const std::string context = "{}";
            logger::debug(
                "LetterSmokeTest: SendCustomPromptToLLM context='{}'",
                context);

            const bool queued = SkyrimNetAPI::SendCustomPromptToLLM(
                kPromptName,
                kPromptVariant,
                context,
                [iteration](std::string response, bool success) {
                    logger::debug(
                        "LetterSmokeTest: LLM callback entered "
                        "(iteration {}, success={}, response.size()={})",
                        iteration, success, response.size());
                    if (!success || response.empty()) {
                        logger::warn(
                            "LetterSmokeTest: LLM call failed "
                            "(iteration {}, success={}, empty={}); "
                            "using fallback body",
                            iteration, success, response.empty());
                        char fallback[160];
                        std::snprintf(
                            fallback, sizeof(fallback),
                            "(LLM call failed -- fallback body, iteration %d.)",
                            iteration);
                        AsyncDispatch::MarshalToMainThread(
                            [text = std::string(fallback)] {
                                OnLetterReceived(text);
                            });
                        return;
                    }
                    logger::info(
                        "LetterSmokeTest: LLM response received "
                        "(iteration {}, {} chars)",
                        iteration, response.size());
                    logger::info(
                        "LetterSmokeTest: LLM response preview: {}",
                        BodyPreview(response));
                    AsyncDispatch::MarshalToMainThread(
                        [text = std::move(response)]() mutable {
                            OnLetterReceived(std::move(text));
                        });
                });

            if (!queued) {
                logger::warn(
                    "LetterSmokeTest: LLM call could not be queued "
                    "(SendCustomPromptToLLM returned false — SkyrimNet "
                    "function pointer null?); using fallback");
                AsyncDispatch::MarshalToMainThread([iteration] {
                    char fallback[160];
                    std::snprintf(
                        fallback, sizeof(fallback),
                        "(SkyrimNet unavailable -- fallback body, iteration %d.)",
                        iteration);
                    OnLetterReceived(std::string(fallback));
                });
            } else {
                logger::debug(
                    "LetterSmokeTest: LLM call queued successfully "
                    "(iteration {})",
                    iteration);
            }
        }

        void OnLetterReceived(std::string text)
        {
            logger::debug(
                "LetterSmokeTest: OnLetterReceived enter (raw {} chars)",
                text.size());
            std::string wrapped = WrapInFontTag(std::move(text));
            const auto wrappedSize = wrapped.size();
            {
                std::scoped_lock lock(g_letterBodyMutex);
                g_currentLetterBody = std::move(wrapped);
                logger::info(
                    "LetterSmokeTest: cached letter body "
                    "(wrapped {} chars)",
                    wrappedSize);
                logger::debug(
                    "LetterSmokeTest: cached body preview: {}",
                    BodyPreview(g_currentLetterBody));
            }
            // 5s pause so the player sees an empty inventory briefly
            // BEFORE the new letter pops in.
            logger::info(
                "LetterSmokeTest: sleeping {}s before delivering the new "
                "letter",
                kPreDeliveryDelaySeconds);
            AsyncDispatch::EnqueueWork([] {
                std::this_thread::sleep_for(
                    std::chrono::seconds(kPreDeliveryDelaySeconds));
                logger::debug(
                    "LetterSmokeTest: pre-delivery delay elapsed; "
                    "marshalling to main thread");
                AsyncDispatch::MarshalToMainThread([] {
                    DeliverLetterToPlayer();
                    std::scoped_lock lock(g_stateMutex);
                    g_state = State::kInPlayerInventory;
                    logger::info(
                        "LetterSmokeTest: state -> {} "
                        "(waiting for player to read; hook calls so far: "
                        "total={}, matched={}, emptyBody={})",
                        StateName(g_state),
                        g_hookCallCount.load(),
                        g_hookMatchCount.load(),
                        g_hookEmptyBodyCount.load());
                });
            });
        }

        void OnPostReadDelayElapsed()
        {
            // We've already slept `kPostReadDelaySeconds` after the book
            // menu closed. Now sleep an additional `kPreRemovalDelay` so
            // the still-in-inventory state is independently observable,
            // THEN remove the letter, THEN sleep `kPreRequestDelay` so
            // the empty-inventory state is independently observable
            // BEFORE the next LLM request kicks off.
            logger::debug("LetterSmokeTest: OnPostReadDelayElapsed enter");
            logger::info(
                "LetterSmokeTest: sleeping {}s before removing the letter "
                "from inventory",
                kPreRemovalDelaySeconds);
            AsyncDispatch::EnqueueWork([] {
                std::this_thread::sleep_for(
                    std::chrono::seconds(kPreRemovalDelaySeconds));
                logger::debug(
                    "LetterSmokeTest: pre-removal delay elapsed; "
                    "marshalling to main thread");
                AsyncDispatch::MarshalToMainThread([] {
                    RemoveLetterFromPlayer();
                    {
                        std::scoped_lock lock(g_stateMutex);
                        g_state = State::kIdle;
                        logger::info(
                            "LetterSmokeTest: state -> {} "
                            "(loop -> next iteration after pre-request delay)",
                            StateName(g_state));
                    }
                    // Now sleep before kicking off the next LLM request.
                    logger::info(
                        "LetterSmokeTest: sleeping {}s before next LLM "
                        "request",
                        kPreRequestDelaySeconds);
                    AsyncDispatch::EnqueueWork([] {
                        std::this_thread::sleep_for(
                            std::chrono::seconds(kPreRequestDelaySeconds));
                        logger::debug(
                            "LetterSmokeTest: pre-request delay elapsed; "
                            "marshalling to main thread");
                        AsyncDispatch::MarshalToMainThread([] {
                            RequestLetter();
                        });
                    });
                });
            });
        }

        void StartInitialDelay()
        {
            logger::debug(
                "LetterSmokeTest: StartInitialDelay enter (sleeping {}s on "
                "worker thread)",
                kInitialDelaySeconds);
            AsyncDispatch::EnqueueWork([] {
                std::this_thread::sleep_for(
                    std::chrono::seconds(kInitialDelaySeconds));
                logger::debug(
                    "LetterSmokeTest: initial delay elapsed; "
                    "marshalling to main thread");
                AsyncDispatch::MarshalToMainThread([] {
                    RequestLetter();
                });
            });
        }
    }  // namespace

    // -- Public API ----------------------------------------------------------

    void Initialize()
    {
        logger::info("LetterSmokeTest::Initialize enter");
        if (g_initialized) {
            logger::info(
                "LetterSmokeTest::Initialize already complete — no-op "
                "(testBookFormID=0x{:08X})",
                g_testBookFormID);
            return;
        }

        logger::debug(
            "LetterSmokeTest: looking up '{}' via TESForm::LookupByEditorID "
            "(requires powerofthree's Tweaks)",
            kBookEditorID);
        auto* form = RE::TESForm::LookupByEditorID(kBookEditorID);
        if (!form) {
            logger::error(
                "LetterSmokeTest: book '{}' not found by EditorID "
                "(ESP not loaded, or powerofthree's Tweaks missing?). "
                "Smoke test disabled.",
                kBookEditorID);
            return;
        }
        logger::debug(
            "LetterSmokeTest: '{}' resolved to form 0x{:08X} (type=0x{:02X})",
            kBookEditorID, form->GetFormID(),
            static_cast<std::uint8_t>(form->GetFormType()));
        auto* book = form->As<RE::TESObjectBOOK>();
        if (!book) {
            logger::error(
                "LetterSmokeTest: EditorID '{}' resolved to a non-book "
                "form (type=0x{:02X}). Smoke test disabled.",
                kBookEditorID,
                static_cast<std::uint8_t>(form->GetFormType()));
            return;
        }
        g_testBookFormID = book->GetFormID();
        const char* originalName = book->GetFullName();
        logger::info(
            "LetterSmokeTest: resolved '{}' to FormID 0x{:08X} "
            "(original name='{}')",
            kBookEditorID, g_testBookFormID,
            originalName ? originalName : "<null>");

        InstallDescriptionHook();
        InstallOpenBookMenuHook();

        if (auto* ui = RE::UI::GetSingleton()) {
            ui->AddEventSink<RE::MenuOpenCloseEvent>(GetBookMenuCloseSink());
            logger::info("LetterSmokeTest: MenuOpenCloseEvent sink registered "
                         "(watching for Book Menu close)");
        } else {
            logger::error(
                "LetterSmokeTest: RE::UI singleton unavailable; "
                "book-close detection disabled (test loop will stall "
                "after first delivery)");
        }

        g_initialized = true;
        logger::info("LetterSmokeTest::Initialize complete");
    }

    void OnPostLoadGame()
    {
        logger::info("LetterSmokeTest::OnPostLoadGame enter");
        if (!g_initialized) {
            logger::warn(
                "LetterSmokeTest: OnPostLoadGame called but Initialize() "
                "didn't complete; skipping (was the book form found?)");
            return;
        }
        {
            std::scoped_lock lock(g_stateMutex);
            if (g_state != State::kIdle) {
                logger::info(
                    "LetterSmokeTest: skipping start — already in state {} "
                    "(loop already running from earlier kPostLoadGame?)",
                    StateName(g_state));
                return;
            }
        }
        logger::info(
            "LetterSmokeTest: starting test loop in {}s "
            "(iteration counter reset will fire on first RequestLetter)",
            kInitialDelaySeconds);
        StartInitialDelay();
    }
}
