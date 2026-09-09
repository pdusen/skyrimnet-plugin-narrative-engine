#include "EngineMock.h"

#include <filesystem>
#include <optional>

#include "FakeVTable.h"
#include "RelocationMocks.h"

#include <RE/Skyrim.h>
#include <REL/Module.h>
#include <SKSE/Interfaces.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <new>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

// Definitions for the CommonLibSSE functions production code calls.
//
// Every function below is declared in a CommonLibSSE header and normally
// defined in CommonLibSSE.lib, where it resolves an address inside a running
// SkyrimSE.exe. The mocked-engine test target does not link that library, so
// these definitions are what the linker binds instead. Adding a module to that
// target means adding whatever engine functions it calls here — the linker
// names each one it is missing.

namespace NarrativeEngine::Testing
{
    // Defined further down, next to the registry they operate on.
    std::vector<RE::Actor*>& LoadedActors();
    void SetEditorID(const void* form, const std::string& editorID);
    void ClearActorRegistry();
    void ClearLocationPool();
    void ClearEditorIDsByForm();
    void ClearQuestAndAliasPools();
    void ClearWorldPool();

    // What the harness knows about a fabricated cell that the engine's own
    // out-of-line accessors have to answer from.
    struct ExteriorCellFacts
    {
        RE::EXTERIOR_DATA coordinates{};
        RE::BGSLocation* location = nullptr;
        bool isInterior = false;
        // The streaming system has finished with it. Cells appear in the grid
        // before their contents arrive, and the default matches what a test
        // that never says otherwise means: a cell that is fully there.
        bool attached = true;
    };

    const ExteriorCellFacts* FactsFor(const void* cell);

    // What a fabricated cell holds, which reference a handle names, and whether
    // an extra-data list carries teleport data. Reached from the out-of-line
    // engine functions further down the file.
    const std::vector<RE::TESObjectREFR*>& ReferencesIn(const void* cell);
    RE::TESNPC* BaseFormOf(const void* actor);
    std::deque<FakeObject>& SimpleFormObjects();
    RE::TESRace* RaceOf(const void* npc);
    RE::BGSLocation* EditorLocationOf(const void* ref);
    const char* FormName(const void* form);
    void SetFormName(const void* form, std::string name);

    // Every form the harness has fabricated of one record type, which is what
    // the data handler hands back when asked for that type's array.
    RE::BSTArray<RE::TESForm*>& FormsOfType(RE::FormType type);
    bool ActorHasKeyword(const void* actor, const RE::BGSKeyword* keyword);
    RE::TESObjectREFR* ReferenceForHandle(std::uint32_t raw);
    RE::BSExtraData* TeleportDataOn(const void* extraList);

    // Cells are fabricated with headroom past sizeof(TESObjectCELL).
    // TESObjectCELL::GetRuntimeData() hands back a view whose members sit at
    // offsets beyond the type's own size -- the block is relocated per runtime,
    // and CommonLibSSE's static_assert covers the record's fixed part only.
    // Writing worldSpace through that view runs off the end of an
    // exactly-sized object and corrupts whatever follows it.
    inline constexpr std::size_t kCellStorageBytes = sizeof(RE::TESObjectCELL) + 0x200;

    // Actors get the same treatment, and for the same reason: several of their
    // accessors -- AsActorState among them -- are relocated views whose members
    // sit past the type's own size, so an exactly-sized object is written off
    // the end of.
    inline constexpr std::size_t kActorStorageBytes = sizeof(RE::Actor) + 0x400;
    void RegisterCellFacts(const void* cell, std::int16_t cellX, std::int16_t cellY, RE::BGSLocation* location);
    const EngineMock::QuestState* QuestStateFor(const void* quest);
    EngineMock::QuestState* MutableQuestStateFor(const void* quest);
    std::unordered_map<const void*, RE::TESObjectREFR*>& AliasReferences();
    RE::TESObjectCELL* FabricatedCell();

    namespace
    {
        // Where a resident's placed reference sits relative to their base
        // form. The engine has no such rule -- this is the fabrication's own
        // convention, so that a test holding an NPC's id can reach the actor
        // made for them without the world fixture handing back both.
        constexpr std::uint32_t kPlacedRefOffset = 0x01000000u;

        // LocationKeywords documents.
        std::unordered_map<const void*, std::string>& EditorIDsByForm()
        {
            static std::unordered_map<const void*, std::string> ids;
            return ids;
        }

        const char* FormEditorIDImpl(void* self)
        {
            const auto& ids = EditorIDsByForm();
            const auto it = ids.find(self);
            return it != ids.end() ? it->second.c_str() : "";
        }

        // TESForm::AsReference() forwards to the virtual AsReference1 at slot
        // 0x2B (AsReference2 at 0x2C is its const twin). A form that IS a
        // reference answers with itself; everything else answers null, which
        // is what the engine's own default does.
        RE::TESObjectREFR* FormAsReferenceSelf(void* self)
        {
            return static_cast<RE::TESObjectREFR*>(self);
        }

        RE::TESObjectREFR* FormAsReferenceNull(void*)
        {
            return nullptr;
        }

        // TESObjectREFR::GetCurrentScene is VIRTUAL, at slot 0x4A. An
        // out-of-line definition of it would never be called: the caller goes
        // through the vtable, so the stand-in has to live in a slot.
        RE::BGSScene* GetCurrentSceneImpl(void*);
        void Update3DPositionImpl(void*, bool);

        // TESForm::SetDelete, slot 0x23. The engine's own body is the flag bit
        // and nothing else, so the stand-in is too -- and setting it for real
        // is what makes IsDeleted() the thing a test asks.
        void SetDeleteImpl(void* self, bool set)
        {
            auto* form = static_cast<RE::TESForm*>(self);
            if (set)
                form->formFlags |= static_cast<std::uint32_t>(RE::TESForm::RecordFlags::kDeleted);
            else
                form->formFlags &= ~static_cast<std::uint32_t>(RE::TESForm::RecordFlags::kDeleted);
        }

        void WireFormDefaults(FakeObject& form, bool isReference)
        {
            void* asReference = isReference ? reinterpret_cast<void*>(&FormAsReferenceSelf)
                                            : reinterpret_cast<void*>(&FormAsReferenceNull);
            form.Slot(0x2B, asReference);
            form.Slot(0x2C, asReference);
            form.Slot(0x23, reinterpret_cast<void*>(&SetDeleteImpl));
            if (isReference) {
                form.Slot(0x4A, reinterpret_cast<void*>(&GetCurrentSceneImpl));
                // Update3DPosition, slot 0x3F. Placement code calls it right
                // after SetPosition to drag the physics body along; without it
                // the actor's capsule stays behind. Nothing here has a physics
                // body, so recording the call is the whole of the stand-in.
                form.Slot(0x3F, reinterpret_cast<void*>(&Update3DPositionImpl));
            }
        }
    } // namespace

    namespace
    {
        EngineMock* g_installed = nullptr;

        // Storage standing in for an engine singleton.
        //
        // The mocked accessors answer out of EngineMock rather than by reading
        // through these pointers, so the bytes only have to supply a stable,
        // correctly-aligned, non-null address. Deliberately oversized: some
        // inline CommonLibSSE code computes interior pointers at fixed offsets
        // from a singleton (ScriptEventSourceHolder's event sources sit at
        // +0x1238), and those have to land inside memory we own rather than
        // past the end of it.
        template <class T> T* OpaqueSingleton()
        {
            constexpr std::size_t kSize = sizeof(T) > 0x4000 ? sizeof(T) : 0x4000;
            alignas(16) static std::byte storage[kSize]{};
            return reinterpret_cast<T*>(storage);
        }

        REL::Version VersionFor(Runtime runtime)
        {
            switch (runtime) {
            case Runtime::SE:
                return REL::Version(1, 5, 97, 0);
            case Runtime::VR:
                return REL::Version(1, 4, 15, 0);
            case Runtime::AE:
            default:
                return REL::Version(1, 6, 1170, 0);
            }
        }

        REL::Module::Runtime ModuleRuntimeFor(Runtime runtime)
        {
            switch (runtime) {
            case Runtime::SE:
                return REL::Module::Runtime::SE;
            case Runtime::VR:
                return REL::Module::Runtime::VR;
            case Runtime::AE:
            default:
                return REL::Module::Runtime::AE;
            }
        }
    } // namespace

    EngineMock::EngineMock(Runtime runtime) : runtime_(runtime)
    {
        if (g_installed) {
            std::fprintf(stderr, "[EngineMock] FATAL: a second EngineMock was constructed while one was alive.\n");
            std::abort();
        }

        // Tell CommonLibSSE which runtime it is looking at, without a Skyrim
        // process to learn it from. `mock()` is CommonLibSSE-NG's own
        // unit-testing hook (guarded by ENABLE_COMMONLIBSSE_TESTING); it fills
        // in the module singleton so the inline REL::Module::IsAE()/IsVR()
        // branches scattered through the headers resolve deterministically
        // instead of trying to inspect a loaded executable.
        if (!REL::Module::mock(VersionFor(runtime), ModuleRuntimeFor(runtime))) {
            std::fprintf(stderr, "[EngineMock] FATAL: REL::Module::mock() failed.\n");
            std::abort();
        }

        // Start from empty form tables so anything an earlier test registered
        // cannot be found by a later one.
        FormTable().clear();
        EditorIDTable().clear();
        ClearActorRegistry();
        ClearLocationPool();
        // Editor IDs are keyed by form ADDRESS, and the location pool recycles
        // addresses between tests. Left uncleared, a fresh nameless location
        // can land where a named one was and inherit its editor ID.
        ClearEditorIDsByForm();
        ClearQuestAndAliasPools();
        // The world pool is deliberately NOT cleared. Worldspaces and their
        // exterior cells are long-lived record data that a save does not
        // reset, and the hold grid built over them latches once per process
        // and keeps pointers into them — exactly as it does in game. Clearing
        // between tests would leave that grid pointing at freed cells.

        g_installed = this;
    }

    EngineMock::~EngineMock()
    {
        FormTable().clear();
        EditorIDTable().clear();
        ClearActorRegistry();
        ClearLocationPool();
        g_installed = nullptr;
        // The REL module singleton is left mocked rather than reset. Resetting
        // would arm `REL::Module::get()` to re-initialise from a real process
        // on the next call, which aborts; leaving the last mocked runtime in
        // place is harmless because every EngineMock re-mocks on construction.
    }

    EngineMock* EngineMock::Current()
    {
        return g_installed;
    }
} // namespace NarrativeEngine::Testing

using NarrativeEngine::Testing::EngineMock;

// ---------------------------------------------------------------------------
// RE::Calendar
// ---------------------------------------------------------------------------

RE::Calendar* RE::Calendar::GetSingleton()
{
    auto* mock = EngineMock::Current();
    if (!mock || !mock->calendar.present)
        return nullptr;
    return NarrativeEngine::Testing::OpaqueSingleton<RE::Calendar>();
}

float RE::Calendar::GetHoursPassed() const
{
    auto* mock = EngineMock::Current();
    if (!mock)
        return 0.0f;
    ++mock->calendar.getHoursPassedCalls;
    return mock->calendar.hoursPassed;
}

// ---------------------------------------------------------------------------
// RE::UI
// ---------------------------------------------------------------------------

RE::UI* RE::UI::GetSingleton()
{
    auto* mock = EngineMock::Current();
    if (!mock || !mock->ui.present)
        return nullptr;
    return NarrativeEngine::Testing::OpaqueSingleton<RE::UI>();
}

bool RE::UI::GameIsPaused()
{
    auto* mock = EngineMock::Current();
    if (!mock)
        return false;
    ++mock->ui.pausedQueries;
    return mock->ui.gameIsPaused;
}

bool RE::UI::IsMenuOpen(const std::string_view& a_menuName)
{
    auto* mock = EngineMock::Current();
    if (!mock)
        return false;
    mock->ui.isMenuOpenQueries.emplace_back(a_menuName);
    const auto& open = mock->ui.openMenus;
    return std::find(open.begin(), open.end(), a_menuName) != open.end();
}

// ---------------------------------------------------------------------------
// RE::PlayerCharacter / RE::Actor
// ---------------------------------------------------------------------------

namespace NarrativeEngine::Testing
{
    RE::PlayerCharacter* BuildFakePlayer();
}

RE::PlayerCharacter* RE::PlayerCharacter::GetSingleton()
{
    auto* mock = EngineMock::Current();
    if (!mock || !mock->player.present)
        return nullptr;
    return NarrativeEngine::Testing::BuildFakePlayer();
}

bool RE::Actor::IsInCombat() const
{
    auto* mock = EngineMock::Current();
    return mock != nullptr && mock->player.inCombat;
}

// ---------------------------------------------------------------------------
// RE::ScriptEventSourceHolder
// ---------------------------------------------------------------------------

RE::ScriptEventSourceHolder* RE::ScriptEventSourceHolder::GetSingleton()
{
    auto* mock = EngineMock::Current();
    if (!mock || !mock->events.holderPresent)
        return nullptr;
    return NarrativeEngine::Testing::OpaqueSingleton<RE::ScriptEventSourceHolder>();
}

// ---------------------------------------------------------------------------
// RE::BSSpinLockGuard
// ---------------------------------------------------------------------------
//
// A no-op guard. The real one spins on a lock word inside an engine structure;
// the tests are single-threaded, and taking a real lock on fake storage would
// only be a way to hang.

RE::BSSpinLockGuard::BSSpinLockGuard(BSSpinLock& a_lock) : _lock(a_lock) {}

RE::BSSpinLockGuard::~BSSpinLockGuard() = default;

// The lock itself, for the engine structures the harness builds for real
// rather than as opaque storage -- BSTEventSource holds one by value. Zeroed
// like the engine's own constructor; Lock and Unlock are no-ops for the same
// reason the guard is.

RE::BSSpinLock::BSSpinLock() : _owningThread(0), _lockCount(0) {}

// ---------------------------------------------------------------------------
// RE::BSScript — the Papyrus virtual machine
// ---------------------------------------------------------------------------
//
// The three that matter (GetSingleton, GetObjectHandlePolicy,
// DispatchMethodCall) are non-virtual wrappers in CommonLibSSE, so standing in
// for them needs no vtable on the opaque singleton storage. The rest are the
// argument-packing machinery `RE::MakeFunctionArguments` drags in.

RE::BSScript::Internal::VirtualMachine* RE::BSScript::Internal::VirtualMachine::GetSingleton()
{
    auto* mock = EngineMock::Current();
    if (!mock || !mock->papyrus.vmPresent)
        return nullptr;
    return NarrativeEngine::Testing::OpaqueSingleton<RE::BSScript::Internal::VirtualMachine>();
}

RE::BSScript::IObjectHandlePolicy* RE::BSScript::IVirtualMachine::GetObjectHandlePolicy()
{
    auto* mock = EngineMock::Current();
    if (!mock || !mock->papyrus.handlePolicyPresent)
        return nullptr;
    return NarrativeEngine::Testing::OpaqueSingleton<RE::BSScript::IObjectHandlePolicy>();
}

RE::VMHandle RE::BSScript::IObjectHandlePolicy::GetHandleForObject(RE::FormType a_typeID, const RE::TESForm* a_srcData)
{
    auto* mock = EngineMock::Current();
    if (!mock)
        return 0;
    mock->papyrus.handleRequests.push_back({static_cast<std::uint32_t>(a_typeID), static_cast<const void*>(a_srcData)});
    return mock->papyrus.handle;
}

bool RE::BSScript::IVirtualMachine::DispatchMethodCall(
    RE::VMHandle a_handle,
    const RE::BSFixedString& a_className,
    const RE::BSFixedString& a_fnName,
    RE::BSScript::IFunctionArguments* a_args,
    RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>& a_result)
{
    (void)a_result;
    auto* mock = EngineMock::Current();
    if (!mock)
        return false;
    mock->papyrus.dispatches.push_back({a_handle,
                                        a_className.c_str() ? a_className.c_str() : "",
                                        a_fnName.c_str() ? a_fnName.c_str() : "",
                                        a_args != nullptr});

    // Actually run the argument functor, as the real VM does when it pulls a
    // queued call off the stack. MakeFunctionArguments only captures the
    // arguments in a tuple; nothing is converted into Papyrus Variables until
    // someone asks. Skipping this would leave the recorded argument list empty
    // and quietly make every assertion about what was passed meaningless.
    if (a_args) {
        RE::BSScrapArray<RE::BSScript::Variable> packed;
        (*a_args)(packed);
    }

    return mock->papyrus.dispatchSucceeds;
}

// Argument packing. `MakeFunctionArguments` builds a scrap-allocated array of
// Variables and sets each one; none of it is read back here, so the Variable
// bodies only have to be safe, and SetSInt only has to record what was packed.

void* RE::BSScrapArrayAllocator::allocate(std::size_t a_size)
{
    void* mem = std::malloc(a_size);
    if (mem)
        std::memset(mem, 0, a_size);
    return mem;
}

void RE::BSScrapArrayAllocator::deallocate(void* a_ptr)
{
    std::free(a_ptr);
}

RE::BSScrapArrayAllocator::~BSScrapArrayAllocator()
{
    // The real one returns the block to the thread's scrap heap. Ours came
    // from the CRT, so it goes back there.
    deallocate(_data);
}

// Variable's own members are out-of-line too, so standing in for its
// constructor means standing in for theirs. All three are trivial: the
// Variable body zeroes everything they set anyway.

RE::BSScript::TypeInfo::TypeInfo()
{
    std::memset(static_cast<void*>(this), 0, sizeof(*this));
}

RE::BSScript::Variable::Value::Value(void* a_val)
{
    std::memset(static_cast<void*>(this), 0, sizeof(*this));
    p = a_val;
}

RE::BSScript::Variable::Value::~Value() {}

RE::BSScript::Variable::Variable()
{
    // The real default constructor leaves a None-typed, zero-valued variable.
    // Zeroing reproduces that without depending on the layout of the union,
    // and matters because these live in scrap memory the caller allocated.
    std::memset(static_cast<void*>(this), 0, sizeof(*this));
}

RE::BSScript::Variable::~Variable() = default;

void RE::BSScript::Variable::SetString(std::string_view a_val)
{
    if (auto* mock = EngineMock::Current())
        mock->papyrus.packedStrings.emplace_back(a_val);
}

void RE::BSScript::Variable::SetBool(bool a_val)
{
    if (auto* mock = EngineMock::Current())
        mock->papyrus.packedBools.push_back(a_val);
}

// A form passed as a Papyrus object argument. The engine turns it into a
// handle through the same policy the receiver's own handle came from; the
// harness records the form, which is the thing a test can name.
void RE::BSScript::PackHandle(RE::BSScript::Variable*, const void* a_src, std::uint32_t)
{
    if (auto* mock = EngineMock::Current())
        mock->papyrus.packedForms.push_back(a_src);
}

void RE::BSScript::Variable::SetSInt(std::int32_t a_val)
{
    // Deliberately does not write into the Variable: nothing in a test reads
    // one back, and recording the value is what lets a test assert which stage
    // number actually got packed for the Papyrus call.
    if (auto* mock = EngineMock::Current())
        mock->papyrus.packedInts.push_back(a_val);
}

// ---------------------------------------------------------------------------
// SKSE::SerializationInterface
// ---------------------------------------------------------------------------
//
// The three co-save calls SKSECosaveIO forwards to. All out-of-line in
// CommonLibSSE.lib, so these definitions are what the linker binds.

bool SKSE::SerializationInterface::OpenRecord(std::uint32_t a_type, std::uint32_t a_version) const
{
    auto* mock = EngineMock::Current();
    if (!mock)
        return false;
    mock->cosave.opened.push_back({a_type, a_version});
    return mock->cosave.openRecordSucceeds;
}

bool SKSE::SerializationInterface::WriteRecordData(const void* a_buf, std::uint32_t a_length) const
{
    auto* mock = EngineMock::Current();
    if (!mock)
        return false;
    const auto* first = static_cast<const std::byte*>(a_buf);
    mock->cosave.written.insert(mock->cosave.written.end(), first, first + a_length);
    return mock->cosave.writeSucceeds;
}

std::uint32_t SKSE::SerializationInterface::ReadRecordData(void* a_buf, std::uint32_t a_length) const
{
    auto* mock = EngineMock::Current();
    if (!mock)
        return 0;
    auto& state = mock->cosave;
    const auto remaining = static_cast<std::uint32_t>(state.readable.size() - state.readCursor);
    const auto n = a_length < remaining ? a_length : remaining;
    if (n > 0) {
        std::memcpy(a_buf, state.readable.data() + state.readCursor, n);
        state.readCursor += n;
    }
    return n;
}

bool SKSE::SerializationInterface::ResolveFormID(RE::FormID a_oldFormID, RE::FormID& a_newFormID) const
{
    auto* mock = EngineMock::Current();
    if (!mock)
        return false;
    mock->cosave.resolveRequests.push_back(a_oldFormID);
    if (!mock->cosave.resolveSucceeds)
        return false;
    a_newFormID = mock->cosave.resolvedFormID;
    return true;
}

// ---------------------------------------------------------------------------
// RE::IFormFactory / RE::Script — the console-command path
// ---------------------------------------------------------------------------
//
// The factory is the harness's first object that has to survive a virtual
// call: `ConcreteFormFactory<T, F>::Create()` is inline and forwards to the
// virtual `CreateImpl()` at slot 01, so opaque storage is not enough. The
// Script form it returns needs a vtable too, because production `delete`s it
// and that runs the virtual destructor at slot 00.

namespace NarrativeEngine::Testing
{
    namespace
    {
        // The transient Script form. Slot 00 is the scalar deleting
        // destructor; ours deliberately does not free, because the storage
        // belongs to the FakeObject and outlives the `delete`.
        void ScriptDeletingDestructor(void*, unsigned int) {}

        FakeObject& FakeScript()
        {
            // sizeof(RE::Script) with TESForm's virtual interface; only slot 00
            // is ever reached from here, but the table is sized generously so a
            // later caller touching another slot aborts by name instead of
            // running off the end.
            static FakeObject script{sizeof(RE::Script), 64};
            static bool wired = false;
            if (!wired) {
                script.Slot(0, reinterpret_cast<void*>(&ScriptDeletingDestructor));
                wired = true;
            }
            return script;
        }

        RE::TESForm* FormFactoryCreateImpl(void*)
        {
            auto* mock = EngineMock::Current();
            if (!mock || !mock->console.scriptCreationSucceeds)
                return nullptr;
            return FakeScript().As<RE::TESForm>();
        }

        FakeObject& FakeFormFactory()
        {
            static FakeObject factory{sizeof(RE::ConcreteFormFactory<RE::Script, RE::FormType::Script>), 8};
            static bool wired = false;
            if (!wired) {
                // Slot 01 per IFormFactory's header comments.
                factory.Slot(1, reinterpret_cast<void*>(&FormFactoryCreateImpl));
                wired = true;
            }
            return factory;
        }
    } // namespace
} // namespace NarrativeEngine::Testing

RE::IFormFactory* RE::IFormFactory::GetFormFactoryByType(RE::FormType)
{
    auto* mock = EngineMock::Current();
    if (!mock || !mock->console.formFactoryPresent)
        return nullptr;
    return NarrativeEngine::Testing::FakeFormFactory().As<RE::IFormFactory>();
}

void RE::Script::SetCommand(std::string_view a_command)
{
    if (auto* mock = EngineMock::Current())
        mock->console.commandsSet.emplace_back(a_command);
}

void RE::Script::CompileAndRun(RE::TESObjectREFR* a_targetRef, RE::COMPILER_NAME)
{
    if (auto* mock = EngineMock::Current())
        mock->console.compileTargets.push_back(static_cast<const void*>(a_targetRef));
}

// ---------------------------------------------------------------------------
// SKSE::TaskInterface
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Worldspaces and their exterior cells
// ---------------------------------------------------------------------------
//
// Built for real: the grid builder walks a worldspace's cellMap the engine's
// own way, and BSTHashMap is a container the harness already knows how to keep
// alive. What is stood in for is only the two out-of-line cell accessors, whose
// answers come from a side table keyed by the cell's address.

namespace NarrativeEngine::Testing
{
    namespace
    {
        std::unordered_map<const void*, ExteriorCellFacts>& CellFacts()
        {
            static auto* facts = new std::unordered_map<const void*, ExteriorCellFacts>();
            return *facts;
        }

        struct WorldPool
        {
            std::deque<FakeObject> worldObjects;
            std::deque<FakeObject> cellObjects;
            RE::BSTArray<RE::TESForm*> worldForms;
        };

        // Leaked, like the form tables: it holds CommonLibSSE containers that
        // free through a relocation.
        WorldPool& Worlds()
        {
            static auto* pool = new WorldPool();
            return *pool;
        }
    } // namespace

    void ClearWorldPool()
    {
        CellFacts().clear();
        Worlds().worldObjects.clear();
        Worlds().cellObjects.clear();
        Worlds().worldForms.clear();
    }

    RE::BSTArray<RE::TESForm*>& WorldSpaceForms()
    {
        return Worlds().worldForms;
    }

    RE::BSTArray<RE::TESForm*>& FormsOfType(RE::FormType type)
    {
        // Leaked with the rest of the form tables: these are CommonLibSSE
        // containers that free through a relocation.
        static auto* byType = new std::map<RE::FormType, RE::BSTArray<RE::TESForm*>*>();
        auto*& array = (*byType)[type];
        if (!array)
            array = new RE::BSTArray<RE::TESForm*>();
        return *array;
    }

    void RegisterCellFacts(const void* cell, std::int16_t cellX, std::int16_t cellY, RE::BGSLocation* location)
    {
        ExteriorCellFacts facts;
        facts.coordinates.cellX = cellX;
        facts.coordinates.cellY = cellY;
        facts.location = location;
        CellFacts()[cell] = facts;
    }

    void EngineMock::StandPlayerInCell(RE::TESWorldSpace* worldSpace, std::int16_t cellX, std::int16_t cellY)
    {
        world.playerCellWorldSpace = worldSpace;
        world.playerCellX = cellX;
        world.playerCellY = cellY;
        world.cellIsInterior = false;
    }

    const ExteriorCellFacts* FactsFor(const void* cell)
    {
        const auto& facts = CellFacts();
        const auto it = facts.find(cell);
        return it == facts.end() ? nullptr : &it->second;
    }

    void EngineMock::SetActorBleedingOut(RE::Actor* actor, bool bleedingOut)
    {
        if (!actor)
            return;
        actor->AsActorState()->actorState1.lifeState =
            bleedingOut ? RE::ACTOR_LIFE_STATE::kBleedout : RE::ACTOR_LIFE_STATE::kAlive;
    }

    RE::TESWorldSpace* EngineMock::AddWorldSpace(std::uint32_t formID, std::string editorID)
    {
        auto& pool = Worlds();
        // Headroom past sizeof, for the same reason cells get it.
        auto& object = pool.worldObjects.emplace_back(sizeof(RE::TESWorldSpace) + 0x200, 128);
        WireFormDefaults(object, false);
        // Its editor ID, which is how anything naming a worldspace in a file
        // or a log refers to it. Left empty unless a test sets one, which is
        // the case a caller falls back from.
        object.Slot(0x32, reinterpret_cast<void*>(&FormEditorIDImpl));
        auto* worldSpace = object.As<RE::TESWorldSpace>();
        worldSpace->formType = RE::FormType::WorldSpace;
        worldSpace->formID = formID;
        // Map bounds wide enough for any test grid. A worldspace whose bounds
        // are all zero is a one-cell world, and a grid builder reads that as
        // degenerate and skips it.
        worldSpace->worldMapData.nwCellX = -64;
        worldSpace->worldMapData.nwCellY = 64;
        worldSpace->worldMapData.seCellX = 64;
        worldSpace->worldMapData.seCellY = -64;
        // The cell map is a real C++ container sitting in zeroed storage, so
        // its members never got their defaults -- and one of them, the
        // end-of-chain sentinel, is a non-zero magic value the iterator tests
        // against. Left at zero the map accepts inserts and reports the right
        // size while yielding nothing from a range-for, which is what a grid
        // builder walking it would see. Constructing it in place fixes that.
        new (&worldSpace->cellMap) RE::BSTHashMap<RE::CellID, RE::TESObjectCELL*>();
        if (!editorID.empty()) {
            SetEditorID(worldSpace, editorID);
            EditorIDTable().insert({RE::BSFixedString(editorID.c_str()), object.As<RE::TESForm>()});
        }
        pool.worldForms.push_back(object.As<RE::TESForm>());
        FormTable().insert({formID, object.As<RE::TESForm>()});
        return worldSpace;
    }

    RE::TESObjectCELL* EngineMock::AddExteriorCell(RE::TESWorldSpace* worldSpace,
                                                   std::int16_t cellX,
                                                   std::int16_t cellY,
                                                   RE::BGSLocation* location)
    {
        if (!worldSpace)
            return nullptr;
        auto& pool = Worlds();
        auto& object = pool.cellObjects.emplace_back(kCellStorageBytes, 128);
        WireFormDefaults(object, false);
        auto* cell = object.As<RE::TESObjectCELL>();
        cell->formType = RE::FormType::Cell;
        cell->formID = 0x00C00000u + static_cast<std::uint32_t>(pool.cellObjects.size());

        ExteriorCellFacts facts;
        facts.coordinates.cellX = cellX;
        facts.coordinates.cellY = cellY;
        facts.location = location;
        CellFacts()[static_cast<const void*>(cell)] = facts;

        // The cell knows its own worldspace. Anything sweeping the loaded grid
        // reads it from here rather than from whatever map it was found in.
        cell->GetRuntimeData().worldSpace = worldSpace;

        // CellID takes (y, x), in that order.
        worldSpace->cellMap.insert({RE::CellID{cellY, cellX}, cell});
        return cell;
    }

    void EngineMock::SetCellAttached(RE::TESObjectCELL* cell, bool attached)
    {
        if (!cell)
            return;
        CellFacts()[static_cast<const void*>(cell)].attached = attached;
    }

    void EngineMock::SetCellInterior(RE::TESObjectCELL* cell, bool interior)
    {
        if (!cell)
            return;
        CellFacts()[static_cast<const void*>(cell)].isInterior = interior;
    }

    namespace
    {
        // Navmesh storage. Leaked in full, and deliberately: every piece of it
        // is a CommonLibSSE container that frees through a relocation, and the
        // meshes are held by intrusive smart pointers that would delete
        // through a fabricated vtable the moment the last one went away.
        struct NavMeshPool
        {
            std::deque<FakeObject> meshObjects;
            std::deque<RE::NavMeshArray*> lists;
        };

        NavMeshPool& NavMeshes()
        {
            static auto* pool = new NavMeshPool();
            return *pool;
        }

        // The engine holds its meshes by intrusive smart pointer, and that
        // pointer releases with `delete`, which does not compile for a type
        // inheriting operator delete from more than one base -- and would be
        // wrong here anyway, since these meshes are fabricated storage the
        // harness owns. A BSTSmartPointer is one raw pointer and nothing else,
        // so the array is filled through a reference of the layout-identical
        // raw type. Production code only ever calls get() on an element.
        RE::BSTArray<RE::NavMesh*>& MeshSlots(RE::NavMeshArray& list)
        {
            return reinterpret_cast<RE::BSTArray<RE::NavMesh*>&>(list.navMeshes);
        }

        // The navmesh list for a cell, made on first use. Every array inside is
        // constructed in place: a BSTArray in zeroed storage is not an empty
        // BSTArray, and the difference shows up as a container that accepts
        // pushes and then yields nothing.
        RE::NavMeshArray& ListFor(RE::TESObjectCELL* cell)
        {
            auto*& list = cell->GetRuntimeData().navMeshes;
            if (!list) {
                // Zeroed bytes rather than a constructed NavMeshArray: its
                // constructor would instantiate the smart-pointer array's
                // destructor, which is the very thing that does not compile.
                list = reinterpret_cast<RE::NavMeshArray*>(new std::byte[sizeof(RE::NavMeshArray)]{});
                new (&MeshSlots(*list)) RE::BSTArray<RE::NavMesh*>();
                NavMeshes().lists.push_back(list);
            }
            return *list;
        }

        RE::NavMesh* NewMesh(RE::TESObjectCELL* cell, std::uint32_t meshFormID)
        {
            auto& object = NavMeshes().meshObjects.emplace_back(sizeof(RE::NavMesh) + 0x100, 128);
            WireFormDefaults(object, false);
            auto* mesh = object.As<RE::NavMesh>();
            mesh->formType = RE::FormType::NavMesh;
            mesh->formID = meshFormID;
            new (&mesh->vertices) RE::BSTArray<RE::BSNavmeshVertex>();
            new (&mesh->triangles) RE::BSTArray<RE::BSNavmeshTriangle>();
            new (&mesh->extraEdgeInfo) RE::BSTArray<RE::BSNavmeshEdgeExtraInfo>();
            MeshSlots(ListFor(cell)).push_back(mesh);
            return mesh;
        }
    } // namespace

    RE::NavMesh* EngineMock::AddNavmeshPatch(RE::TESObjectCELL* cell,
                                             std::uint32_t meshFormID,
                                             float westX,
                                             float southY,
                                             float eastX,
                                             float northY,
                                             float surfaceZ)
    {
        if (!cell)
            return nullptr;
        auto* mesh = NewMesh(cell, meshFormID);
        const RE::NiPoint3 corners[4] = {
            RE::NiPoint3{westX, southY, surfaceZ},
            RE::NiPoint3{eastX, southY, surfaceZ},
            RE::NiPoint3{eastX, northY, surfaceZ},
            RE::NiPoint3{westX, northY, surfaceZ},
        };
        for (const auto& corner : corners) {
            RE::BSNavmeshVertex vertex{};
            vertex.location = corner;
            mesh->vertices.push_back(vertex);
        }
        // The rectangle split along one diagonal, which is what any mesh
        // generator does with a flat quad.
        const std::uint16_t indices[2][3] = {{0, 1, 2}, {0, 2, 3}};
        for (const auto& triangle : indices) {
            RE::BSNavmeshTriangle tri{};
            tri.triangleFlags.set(RE::BSNavmeshTriangle::TriangleFlag::kPreferred);
            for (int v = 0; v < 3; ++v) {
                tri.vertices[v] = triangle[v];
                tri.triangles[v] = 0xFFFFu;
            }
            mesh->triangles.push_back(tri);
        }
        return mesh;
    }

    // -----------------------------------------------------------------------
    // NPC base forms and the relationships between them
    // -----------------------------------------------------------------------
    //
    // BGSRelationship::GetRelationship is a free function the engine resolves
    // through the address library, so what is stood in for is the lookup
    // rather than any member: see the entry in RelocationMocks.cpp.

    namespace
    {
        struct RelationshipPool
        {
            std::deque<FakeObject> npcs;
            std::deque<FakeObject> relationships;
            std::deque<FakeObject> associations;
            // Keyed by the ordered pair, because the record itself is ordered
            // and the label depends on which side someone sits.
            std::map<std::pair<const void*, const void*>, RE::BGSRelationship*> byPair;
        };

        RelationshipPool& Relationships()
        {
            static auto* pool = new RelationshipPool();
            return *pool;
        }
    } // namespace

    RE::BGSRelationship* RelationshipBetween(RE::TESNPC* a, RE::TESNPC* b)
    {
        if (!a || !b)
            return nullptr;
        const auto& table = Relationships().byPair;
        if (const auto it = table.find({static_cast<const void*>(a), static_cast<const void*>(b)}); it != table.end())
            return it->second;
        if (const auto it = table.find({static_cast<const void*>(b), static_cast<const void*>(a)}); it != table.end())
            return it->second;
        return nullptr;
    }

    namespace
    {
        std::map<const void*, RE::TESRace*>& Races()
        {
            static auto* table = new std::map<const void*, RE::TESRace*>();
            return *table;
        }

        std::map<const void*, RE::BGSLocation*>& EditorLocations()
        {
            static auto* table = new std::map<const void*, RE::BGSLocation*>();
            return *table;
        }
    } // namespace

    RE::TESRace* RaceOf(const void* npc)
    {
        const auto& table = Races();
        const auto it = table.find(npc);
        return it == table.end() ? nullptr : it->second;
    }

    RE::BGSLocation* EditorLocationOf(const void* ref)
    {
        const auto& table = EditorLocations();
        const auto it = table.find(ref);
        return it == table.end() ? nullptr : it->second;
    }

    void EngineMock::SetEditorIDOf(RE::TESForm* form, std::string editorID)
    {
        if (!form || editorID.empty())
            return;
        SetEditorID(form, editorID);
        EditorIDTable().insert({RE::BSFixedString(editorID.c_str()), form});
    }

    void EngineMock::SetNPCRace(RE::TESNPC* npc, RE::TESRace* race)
    {
        if (npc)
            Races()[static_cast<const void*>(npc)] = race;
    }

    void EngineMock::SetEditorLocation(RE::TESObjectREFR* ref, RE::BGSLocation* location)
    {
        if (ref)
            EditorLocations()[static_cast<const void*>(ref)] = location;
    }

    RE::TESNPC* EngineMock::AddNPC(std::uint32_t formID, std::string name, bool female)
    {
        auto& pool = Relationships();
        auto& object = pool.npcs.emplace_back(sizeof(RE::TESNPC) + 0x100, 256);
        WireFormDefaults(object, false);
        // GetFormEditorID is slot 0x32 on TESForm. Anything filtering
        // records by name pattern reads it.
        object.Slot(0x32, reinterpret_cast<void*>(&FormEditorIDImpl));
        auto* npc = object.As<RE::TESNPC>();
        npc->formType = RE::FormType::NPC;
        npc->formID = formID;
        if (female)
            npc->actorData.actorBaseFlags.set(RE::ACTOR_BASE_DATA::Flag::kFemale);
        // The engine's own arrays hold every record of a type; a fabricated
        // one has to join its array or a sweep of the load order misses it.
        new (&npc->factions) RE::BSTArray<RE::FACTION_RANK>();
        FormTable().insert({formID, object.As<RE::TESForm>()});
        FormsOfType(RE::FormType::NPC).push_back(object.As<RE::TESForm>());
        // Registered even when empty, so an NPC a test deliberately left
        // nameless reads as nameless rather than falling through to the
        // mock-wide name the ModEvent sink's sender uses.
        SetFormName(object.As<RE::TESForm>(), std::move(name));
        // Everyone is a person unless a test says otherwise. The keyword on an
        // NPC's race is how anything tells a person from a wolf, and giving
        // every fabricated NPC a race that carries it keeps that out of the
        // way of tests that are not about it.
        SetNPCRace(npc, DefaultPeopleRace());
        return npc;
    }

    namespace
    {
        // Which base form each fabricated actor was made from. Kept beside the
        // actor rather than written into it, because the field the engine
        // keeps it in sits inside a relocated block.
        std::map<const void*, RE::TESNPC*>& ActorBases()
        {
            static auto* table = new std::map<const void*, RE::TESNPC*>();
            return *table;
        }
    } // namespace

    RE::TESNPC* BaseFormOf(const void* actor)
    {
        const auto& table = ActorBases();
        const auto it = table.find(actor);
        return it == table.end() ? nullptr : it->second;
    }

    void EngineMock::SetActorBase(RE::Actor* actor, RE::TESNPC* base)
    {
        if (actor)
            ActorBases()[static_cast<const void*>(actor)] = base;
    }

    namespace
    {
        std::map<const void*, std::vector<const RE::BGSKeyword*>>& ActorKeywords()
        {
            static auto* table = new std::map<const void*, std::vector<const RE::BGSKeyword*>>();
            return *table;
        }
    } // namespace

    bool ActorHasKeyword(const void* actor, const RE::BGSKeyword* keyword)
    {
        const auto& table = ActorKeywords();
        const auto it = table.find(actor);
        if (it == table.end())
            return false;
        return std::find(it->second.begin(), it->second.end(), keyword) != it->second.end();
    }

    void EngineMock::GiveActorKeyword(RE::Actor* actor, RE::BGSKeyword* keyword)
    {
        if (actor && keyword)
            ActorKeywords()[static_cast<const void*>(actor)].push_back(keyword);
    }

    void EngineMock::AddRelationship(RE::TESNPC* a, RE::TESNPC* b, const char* labelForMale, const char* labelForFemale)
    {
        if (!a || !b)
            return;
        auto& pool = Relationships();

        auto& assocObject = pool.associations.emplace_back(sizeof(RE::BGSAssociationType) + 0x40, 128);
        WireFormDefaults(assocObject, false);
        auto* assoc = assocObject.As<RE::BGSAssociationType>();
        assoc->formType = RE::FormType::AssociationType;
        // Both rows carry the same pair of labels. Which row is read depends on
        // which side of the record the person being named sits, and a test that
        // set only one would pass or fail on that detail rather than on the
        // framing under test.
        for (auto& row : assoc->associationLabels) {
            row[RE::BGSAssociationType::Sexes::kMale] = labelForMale;
            row[RE::BGSAssociationType::Sexes::kFemale] = labelForFemale;
        }

        auto& object = pool.relationships.emplace_back(sizeof(RE::BGSRelationship) + 0x40, 128);
        WireFormDefaults(object, false);
        auto* rel = object.As<RE::BGSRelationship>();
        rel->formType = RE::FormType::Relationship;
        rel->npc1 = a;
        rel->npc2 = b;
        rel->assocType = assoc;
        pool.byPair[{static_cast<const void*>(a), static_cast<const void*>(b)}] = rel;
    }

    RE::TESObjectCELL* EngineMock::GroundCell()
    {
        return FabricatedCell();
    }

    void EngineMock::AddEmptyNavMeshList(RE::TESObjectCELL* cell)
    {
        if (cell)
            (void)ListFor(cell);
    }

    RE::NavMesh* EngineMock::AddNavMesh(RE::TESObjectCELL* cell,
                                        std::uint32_t meshFormID,
                                        const std::vector<FakeTriangle>& triangles)
    {
        if (!cell)
            return nullptr;
        auto* mesh = NewMesh(cell, meshFormID);

        using TriangleFlag = RE::BSNavmeshTriangle::TriangleFlag;
        constexpr std::array<TriangleFlag, 3> kEdgeLinkFlags = {
            TriangleFlag::kEdge0_Link,
            TriangleFlag::kEdge1_Link,
            TriangleFlag::kEdge2_Link,
        };
        constexpr std::uint16_t kNoTriangle = 0xFFFF;

        for (const auto& spec : triangles) {
            RE::BSNavmeshTriangle tri{};
            // Three vertices of its own, all on the spot asked for, so the
            // centroid the extractor computes lands exactly there.
            const auto base = static_cast<std::uint16_t>(mesh->vertices.size());
            for (int v = 0; v < 3; ++v) {
                RE::BSNavmeshVertex vertex{};
                vertex.location = RE::NiPoint3{spec.x, spec.y, spec.z};
                mesh->vertices.push_back(vertex);
                tri.vertices[v] = static_cast<std::uint16_t>(base + v);
            }
            if (spec.preferred)
                tri.triangleFlags.set(TriangleFlag::kPreferred);
            if (spec.deleted)
                tri.triangleFlags.set(TriangleFlag::kDeleted);

            for (int e = 0; e < 3; ++e) {
                tri.triangles[e] = kNoTriangle;
                if (spec.danglingLink[e]) {
                    // The link flag with an index past the end of the extra
                    // edge array: nothing to read, and nothing to follow.
                    tri.triangleFlags.set(kEdgeLinkFlags[e]);
                    tri.triangles[e] = 0xFFFEu;
                    continue;
                }
                if (spec.ledge[e] || spec.portalMesh[e] != 0) {
                    RE::BSNavmeshEdgeExtraInfo info{};
                    if (spec.ledge[e]) {
                        info.type = RE::EDGE_EXTRA_INFO_TYPE::kLedgeUp;
                    } else {
                        info.type = RE::EDGE_EXTRA_INFO_TYPE::kPortal;
                        info.portal.otherMeshID = spec.portalMesh[e];
                        info.portal.triangle = static_cast<std::uint16_t>(spec.portalTriangle[e]);
                    }
                    tri.triangleFlags.set(kEdgeLinkFlags[e]);
                    tri.triangles[e] = static_cast<std::uint16_t>(mesh->extraEdgeInfo.size());
                    mesh->extraEdgeInfo.push_back(info);
                    continue;
                }
                if (spec.neighbor[e] >= 0)
                    tri.triangles[e] = static_cast<std::uint16_t>(spec.neighbor[e]);
            }
            mesh->triangles.push_back(tri);
        }
        return mesh;
    }

    RE::NavMesh* EngineMock::AddNavMeshWithBadVertexIndex(RE::TESObjectCELL* cell, std::uint32_t meshFormID)
    {
        if (!cell)
            return nullptr;
        auto* mesh = NewMesh(cell, meshFormID);
        RE::BSNavmeshTriangle tri{};
        tri.triangleFlags.set(RE::BSNavmeshTriangle::TriangleFlag::kPreferred);
        tri.vertices[0] = 0;
        tri.vertices[1] = 1;
        tri.vertices[2] = 900; // the mesh has no such vertex
        tri.triangles[0] = 0xFFFFu;
        tri.triangles[1] = 0xFFFFu;
        tri.triangles[2] = 0xFFFFu;
        for (int v = 0; v < 2; ++v) {
            RE::BSNavmeshVertex vertex{};
            mesh->vertices.push_back(vertex);
        }
        mesh->triangles.push_back(tri);
        return mesh;
    }

    // -----------------------------------------------------------------------
    // The NAVI record
    // -----------------------------------------------------------------------
    //
    // BSNavmeshInfo is a forward declaration in CommonLibSSE and nothing more,
    // so there is no type to construct: the harness hands out blocks of bytes
    // with the fields written where `navInfo` says, and the code under test is
    // free to go looking for them. The blocks are oversized because a search
    // for a field reads a window past the front of one.
    namespace
    {
        constexpr std::size_t kNavInfoBytes = 0x100;

        struct NavPool
        {
            std::deque<FakeObject> mapObjects;
            std::deque<FakeObject> meshForms;
            std::deque<std::vector<std::byte>> infoBlocks;
            std::deque<RE::BSTArray<const RE::BSNavmeshInfo*>*> chains;
            std::deque<RE::BSTArray<RE::BSNavmeshInfo*>*> cellBuckets;
            // Which info block each navmesh FormID resolves to, which is the
            // whole of what the record is asked for by FormID.
            std::unordered_map<std::uint32_t, RE::BSNavmeshInfo*> byFormID;
            // Which cell each navmesh was filed under, so a second navmesh in
            // the same cell joins the bucket already there.
            std::unordered_map<std::uint64_t, RE::BSTArray<RE::BSNavmeshInfo*>*> bucketByCell;
            // What each fabricated NavMesh form calls its parent cell.
            std::unordered_map<const void*, RE::TESObjectCELL*> meshParent;
        };

        NavPool& Nav()
        {
            static auto* pool = new NavPool();
            return *pool;
        }

        RE::BSNavmeshInfo* NavmeshInfoImpl(void*, std::uint32_t id)
        {
            const auto& table = Nav().byFormID;
            const auto it = table.find(id);
            return it == table.end() ? nullptr : it->second;
        }

        RE::TESObjectCELL* SaveParentCellImpl(void* self)
        {
            // `self` is the TESChildCell subobject, which is what the harness
            // keyed the table on when it wired the slot.
            const auto& table = Nav().meshParent;
            const auto it = table.find(self);
            return it == table.end() ? nullptr : it->second;
        }
    } // namespace

    RE::NavMeshInfoMap* EngineMock::AddNavMeshInfoMap(std::uint32_t formID)
    {
        auto& pool = Nav();
        auto& object = pool.mapObjects.emplace_back(sizeof(RE::NavMeshInfoMap) + 0x100, 128);
        WireFormDefaults(object, false);
        auto* map = object.As<RE::NavMeshInfoMap>();
        map->formType = RE::FormType::Navigation;
        map->formID = formID;

        // GetNavmeshInfo is slot 02 of the BSNavmeshInfoMap vtable, which is a
        // base of its own with its own pointer, not of the primary one.
        auto* infoBase = static_cast<RE::BSNavmeshInfoMap*>(map);
        const auto infoOffset =
            static_cast<std::size_t>(reinterpret_cast<std::byte*>(infoBase) - reinterpret_cast<std::byte*>(map));
        object.BaseSlot(infoOffset, 8, 2, reinterpret_cast<void*>(&NavmeshInfoImpl));

        // Both containers are real ones sitting in zeroed storage, which is not
        // the same thing as an empty container: the hash map keeps a non-zero
        // end-of-chain sentinel it would never have got, and iterating without
        // it yields nothing at all.
        new (&map->ckNavMeshInfoMap) RE::BSTHashMap<std::uint64_t, RE::BSTArray<RE::BSNavmeshInfo*>*>();
        new (&map->allPaths) RE::BSTArray<RE::BSTArray<const RE::BSNavmeshInfo*>*>();

        FormTable().insert({formID, object.As<RE::TESForm>()});
        return map;
    }

    const RE::BSNavmeshInfo* EngineMock::AddNavmeshInfo(RE::NavMeshInfoMap* map,
                                                        std::uint32_t navMeshFormID,
                                                        std::uint32_t worldSpaceFormID,
                                                        std::int16_t cellX,
                                                        std::int16_t cellY,
                                                        float x,
                                                        float y,
                                                        float z)
    {
        if (!map)
            return nullptr;
        auto& pool = Nav();
        auto& block = pool.infoBlocks.emplace_back(kNavInfoBytes, std::byte{});
        auto* bytes = block.data();
        if (navInfo.writeFormID && navInfo.formIDOffset + sizeof(std::uint32_t) <= kNavInfoBytes)
            std::memcpy(bytes + navInfo.formIDOffset, &navMeshFormID, sizeof(navMeshFormID));
        if (navInfo.writePosition && navInfo.positionOffset + sizeof(float) * 3 <= kNavInfoBytes) {
            const float point[3] = {x, y, z};
            std::memcpy(bytes + navInfo.positionOffset, point, sizeof(point));
        }

        auto* info = reinterpret_cast<RE::BSNavmeshInfo*>(bytes);
        pool.byFormID[navMeshFormID] = info;

        // The engine files navmeshes by a packed key: worldspace in the high
        // half, then cellX, then cellY.
        const auto key = (static_cast<std::uint64_t>(worldSpaceFormID) << 32)
                         | (static_cast<std::uint64_t>(static_cast<std::uint16_t>(cellX)) << 16)
                         | static_cast<std::uint64_t>(static_cast<std::uint16_t>(cellY));
        auto& bucket = pool.bucketByCell[key];
        if (!bucket) {
            bucket = new RE::BSTArray<RE::BSNavmeshInfo*>();
            pool.cellBuckets.push_back(bucket);
            map->ckNavMeshInfoMap.insert({key, bucket});
        }
        bucket->push_back(info);
        return info;
    }

    RE::NavMesh* EngineMock::AddNavMeshForm(std::uint32_t formID,
                                            RE::TESObjectCELL* parentCell,
                                            float minX,
                                            float minY,
                                            float maxX,
                                            float maxY)
    {
        auto& pool = Nav();
        auto& object = pool.meshForms.emplace_back(sizeof(RE::NavMesh) + 0x100, 128);
        WireFormDefaults(object, false);
        auto* mesh = object.As<RE::NavMesh>();
        mesh->formType = RE::FormType::NavMesh;
        mesh->formID = formID;
        mesh->meshGrid.gridBoundsMin = RE::NiPoint3{minX, minY, 0.0f};
        mesh->meshGrid.gridBoundsMax = RE::NiPoint3{maxX, maxY, 0.0f};

        // GetSaveParentCell is slot 01 of TESChildCell, a base with its own
        // vtable pointer partway into the object.
        auto* childCell = static_cast<RE::TESChildCell*>(mesh);
        const auto childOffset =
            static_cast<std::size_t>(reinterpret_cast<std::byte*>(childCell) - reinterpret_cast<std::byte*>(mesh));
        object.BaseSlot(childOffset, 4, 1, reinterpret_cast<void*>(&SaveParentCellImpl));
        pool.meshParent[static_cast<const void*>(childCell)] = parentCell;

        FormTable().insert({formID, object.As<RE::TESForm>()});
        return mesh;
    }

    void EngineMock::AddPreferredPath(RE::NavMeshInfoMap* map, const std::vector<const RE::BSNavmeshInfo*>& chain)
    {
        if (!map)
            return;
        auto* stored = new RE::BSTArray<const RE::BSNavmeshInfo*>();
        Nav().chains.push_back(stored);
        for (const auto* info : chain)
            stored->push_back(info);
        map->allPaths.push_back(stored);
    }

    void EngineMock::LoadGrid(const std::vector<RE::TESObjectCELL*>& cells)
    {
        // Leaked with everything else here, and for the same reason: the grid
        // the engine hands out is never taken down inside a session.
        static auto* pool = new std::deque<std::vector<RE::TESObjectCELL*>>();

        std::uint32_t length = 0;
        while (static_cast<std::size_t>(length) * length < cells.size())
            ++length;

        auto& slots = pool->emplace_back(static_cast<std::size_t>(length) * length, nullptr);
        for (std::size_t i = 0; i < cells.size(); ++i)
            slots[i] = cells[i];

        auto* array = OpaqueSingleton<RE::GridCellArray>();
        array->length = length;
        array->cells = slots.empty() ? nullptr : slots.data();
        grid.present = true;
    }
} // namespace NarrativeEngine::Testing

RE::TESDataHandler* RE::TESDataHandler::GetSingleton(bool)
{
    auto* mock = EngineMock::Current();
    if (!mock || !mock->world.dataHandlerPresent)
        return nullptr;
    return NarrativeEngine::Testing::OpaqueSingleton<RE::TESDataHandler>();
}

RE::BSTArray<RE::TESForm*>& RE::TESDataHandler::GetFormArray(RE::FormType a_formType)
{
    // One array per record type, holding whatever the harness has fabricated
    // of that type. A type nothing has made answers empty, which is what an
    // install carrying none of that record looks like.
    if (a_formType == RE::FormType::WorldSpace)
        return NarrativeEngine::Testing::WorldSpaceForms();
    return NarrativeEngine::Testing::FormsOfType(a_formType);
}

RE::EXTERIOR_DATA* RE::TESObjectCELL::GetCoordinates()
{
    const auto* facts = NarrativeEngine::Testing::FactsFor(this);
    if (!facts)
        return nullptr;
    return const_cast<RE::EXTERIOR_DATA*>(&facts->coordinates);
}

RE::BGSLocation* RE::TESObjectCELL::GetLocation() const
{
    const auto* facts = NarrativeEngine::Testing::FactsFor(this);
    return facts ? facts->location : nullptr;
}

// ---------------------------------------------------------------------------
// Quests, aliases, and the extra data that records a fill
// ---------------------------------------------------------------------------
//
// Built for real rather than as opaque storage. ExtraAliasInstanceArray is a
// plain struct of arrays, and the walk under test reads it field by field, so a
// genuine one is both simpler than a stand-in and exactly faithful. What is
// mocked is only the handful of out-of-line predicates around it.

namespace NarrativeEngine::Testing
{
    namespace
    {
        // Fabricated quests, by address, so the mocked predicates below can
        // answer per-quest rather than globally. A walk over several aliases
        // sees several quests and has to tell them apart.
        std::unordered_map<const void*, EngineMock::QuestState>& QuestStates()
        {
            static std::unordered_map<const void*, EngineMock::QuestState> states;
            return states;
        }

        struct QuestPool
        {
            std::deque<FakeObject> objects;
            std::deque<RE::TESFile> files;
            // TESFileArray's members are private, so the array is built as
            // raw storage and its pointer/size written at their documented
            // offsets -- the same technique the string-pool stand-in uses.
            std::deque<RE::TESFileArray> fileArrays;
            std::deque<RE::TESFile*> filePointers;
        };

        // Leaked for the same reason as the event source above: these pools
        // hold CommonLibSSE containers that free through a relocation.
        QuestPool& Quests()
        {
            static auto* pool = new QuestPool();
            return *pool;
        }

        struct AliasPool
        {
            std::deque<FakeObject> aliasObjects;
            std::deque<RE::BGSRefAliasInstanceData> instances;
            std::deque<RE::BSTArray<RE::TESPackage*>> packageArrays;
            std::deque<RE::ExtraAliasInstanceArray> arrays;
        };

        AliasPool& Aliases()
        {
            static auto* pool = new AliasPool();
            return *pool;
        }

    } // namespace

    // The one array the mocked ExtraDataList::GetByType hands back.
    RE::ExtraAliasInstanceArray*& CurrentAliasArray()
    {
        static RE::ExtraAliasInstanceArray* array = nullptr;
        return array;
    }

    void ClearQuestAndAliasPools()
    {
        QuestStates().clear();
        Quests().objects.clear();
        Quests().files.clear();
        Quests().fileArrays.clear();
        Aliases().aliasObjects.clear();
        Aliases().instances.clear();
        Aliases().packageArrays.clear();
        Aliases().arrays.clear();
        AliasReferences().clear();
        CurrentAliasArray() = nullptr;
    }

    const EngineMock::QuestState* QuestStateFor(const void* quest)
    {
        return MutableQuestStateFor(quest);
    }

    // Starting, stopping and resetting a quest all change what the predicates
    // answer afterwards, which is the whole point of calling them.
    EngineMock::QuestState* MutableQuestStateFor(const void* quest)
    {
        auto& states = QuestStates();
        const auto it = states.find(quest);
        return it == states.end() ? nullptr : &it->second;
    }

    // Per-alias fills. An alias with an entry here answers with what it was
    // given -- null included, which reads as "the quest has not filled it yet"
    // and is a state the courier's single global flag cannot express.
    std::unordered_map<const void*, RE::TESObjectREFR*>& AliasReferences()
    {
        static auto* references = new std::unordered_map<const void*, RE::TESObjectREFR*>();
        return *references;
    }

    RE::TESQuest* EngineMock::AddQuest(const QuestState& state)
    {
        auto& pool = Quests();
        auto& object = pool.objects.emplace_back(sizeof(RE::TESQuest), 128);
        WireFormDefaults(object, false);
        object.Slot(0x32, reinterpret_cast<void*>(&FormEditorIDImpl));
        auto* quest = object.As<RE::TESQuest>();
        quest->formType = RE::FormType::Quest;
        quest->formID = state.formID;
        SetEditorID(quest, state.editorID);

        // GetFile is inline over sourceFiles.array, so the file list is built
        // for real; the ESP name is what self-exclusion compares against.
        auto& file = pool.files.emplace_back();
        std::memset(static_cast<void*>(&file), 0, sizeof(file));
        const auto n = state.sourceFile.copy(file.fileName, sizeof(file.fileName) - 1);
        file.fileName[n] = '\0';
        auto& filePointer = pool.filePointers.emplace_back(&file);
        auto& files = pool.fileArrays.emplace_back();
        // { TESFile** _data; uint32 _size; } at offsets 0 and 8.
        auto* raw = reinterpret_cast<std::byte*>(&files);
        RE::TESFile** dataPointer = &filePointer;
        std::memcpy(raw, &dataPointer, sizeof(dataPointer));
        const std::uint32_t count = 1;
        std::memcpy(raw + sizeof(dataPointer), &count, sizeof(count));
        quest->sourceFiles.array = &files;

        // Registered by editor ID as well, because modules that hold a quest
        // resolve it with LookupByEditorID exactly once per process.
        if (!state.editorID.empty())
            EditorIDTable().insert({RE::BSFixedString(state.editorID.c_str()), object.As<RE::TESForm>()});
        FormTable().insert({state.formID, object.As<RE::TESForm>()});

        QuestStates()[static_cast<const void*>(quest)] = state;
        return quest;
    }

    EngineMock::AliasedQuest EngineMock::AddQuestWithAliases(const QuestState& state,
                                                             const std::vector<std::string>& aliasNames)
    {
        // Storage that outlives the mock, keyed by editor ID, and never
        // cleared. Everything else the harness fabricates is rebuilt per test,
        // but a module that resolves aliases at data load caches the POINTERS
        // exactly once per process -- so the next test rebuilding the same
        // quest has to rebuild it at the same address or that cache dangles.
        struct Storage
        {
            std::deque<FakeObject> objects;
            AliasedQuest built;
            RE::TESForm* form = nullptr;
        };
        static auto* byEditorID = new std::map<std::string, Storage>();

        const auto [it, fresh] = byEditorID->try_emplace(state.editorID);
        auto& storage = it->second;
        if (fresh) {
            auto& questObject = storage.objects.emplace_back(sizeof(RE::TESQuest), 128);
            WireFormDefaults(questObject, false);
            questObject.Slot(0x32, reinterpret_cast<void*>(&FormEditorIDImpl));
            storage.built.quest = questObject.As<RE::TESQuest>();
            storage.form = questObject.As<RE::TESForm>();
            // Built in place. A zeroed BSTArray takes a push_back and reports
            // the right size afterwards, then iterates as empty -- so the
            // alias walk would find nothing and every slot would report both
            // its aliases missing.
            new (&storage.built.quest->aliases) RE::BSTArray<RE::BGSBaseAlias*>();
            for (std::size_t i = 0; i < aliasNames.size(); ++i) {
                auto& aliasObject = storage.objects.emplace_back(sizeof(RE::BGSRefAlias), 64);
                storage.built.aliases.push_back(aliasObject.As<RE::BGSRefAlias>());
            }
        }

        auto* quest = storage.built.quest;
        quest->formType = RE::FormType::Quest;
        quest->formID = state.formID;
        SetEditorID(quest, state.editorID);
        quest->aliases.clear();
        for (std::size_t i = 0; i < aliasNames.size() && i < storage.built.aliases.size(); ++i) {
            auto* alias = storage.built.aliases[i];
            auto* base = reinterpret_cast<RE::BGSBaseAlias*>(alias);
            base->aliasName = aliasNames[i].c_str();
            quest->aliases.push_back(base);
            // Explicitly empty rather than absent: an alias the quest has not
            // filled yet is the state a beat spends its whole COMPOSE arm
            // waiting on, and the courier's global answer cannot express it.
            AliasReferences()[static_cast<const void*>(alias)] = nullptr;
        }

        // The ESP a quest was authored in is not reproduced here: nothing that
        // drives a quest through its own aliases asks which file it came from.
        if (!state.editorID.empty())
            EditorIDTable().insert({RE::BSFixedString(state.editorID.c_str()), storage.form});
        FormTable().insert({state.formID, storage.form});
        QuestStates()[static_cast<const void*>(quest)] = state;
        return storage.built;
    }

    void EngineMock::FillRefAlias(RE::BGSRefAlias* alias, RE::TESObjectREFR* reference)
    {
        if (!alias)
            return;
        AliasReferences()[static_cast<const void*>(alias)] = reference;
    }

    void EngineMock::FillAliasInstances(RE::Actor*)
    {
        auto& pool = Aliases();
        auto& array = pool.arrays.emplace_back();
        for (const auto& described : aliases.instances) {
            auto& aliasObject = pool.aliasObjects.emplace_back(sizeof(RE::BGSBaseAlias), 32);
            auto* alias = aliasObject.As<RE::BGSBaseAlias>();
            using F = RE::BGSBaseAlias::FLAGS;
            alias->flags = F::kNone;
            if (described.reserves)
                alias->flags.set(F::kReserves);
            if (described.questObject)
                alias->flags.set(F::kQuestObject);

            auto& instance = pool.instances.emplace_back();
            instance.quest = AddQuest(described.quest);
            instance.alias = alias;
            if (described.dispensesPackages) {
                auto& packages = pool.packageArrays.emplace_back();
                // One non-null entry is enough: the walk asks only whether the
                // alias dispenses anything, never what.
                packages.push_back(reinterpret_cast<RE::TESPackage*>(&packages));
                instance.instancedPackages = &packages;
            } else {
                instance.instancedPackages = nullptr;
            }
            array.aliases.push_back(&instance);
        }
        CurrentAliasArray() = &array;
    }
} // namespace NarrativeEngine::Testing

RE::BSExtraData* RE::ExtraDataList::GetByType(RE::ExtraDataType a_type)
{
    // Two kinds of extra data are asked for on fabricated references, and each
    // is answered from a side table rather than from a real list: the alias
    // instances an actor was filled into, and the teleport data on a door.
    if (a_type == RE::ExtraDataType::kTeleport)
        return NarrativeEngine::Testing::TeleportDataOn(static_cast<const void*>(this));

    auto* mock = EngineMock::Current();
    if (!mock || !mock->aliases.arrayPresent)
        return nullptr;
    if (a_type != RE::ExtraDataType::kAliasInstanceArray)
        return nullptr;
    return NarrativeEngine::Testing::CurrentAliasArray();
}

const RE::BSExtraData* RE::ExtraDataList::GetByType(RE::ExtraDataType a_type) const
{
    return const_cast<RE::ExtraDataList*>(this)->GetByType(a_type);
}

bool RE::TESQuest::IsStopped() const
{
    const auto* state = NarrativeEngine::Testing::QuestStateFor(this);
    return state != nullptr && state->stopped;
}

bool RE::TESQuest::IsCompleted() const
{
    const auto* state = NarrativeEngine::Testing::QuestStateFor(this);
    return state != nullptr && state->completed;
}

// A no-op read guard. The tests are single-threaded, and taking a real lock on
// fabricated storage would only be a way to hang.
RE::BSReadLockGuard::BSReadLockGuard(BSReadWriteLock& a_lock) : _lock(a_lock) {}

RE::BSReadLockGuard::~BSReadLockGuard() = default;

// The extra-data node itself. Its constructor is out-of-line in the engine, and
// what it does there is set the base's vtable and next pointer; a zeroed object
// with an empty array is the same state, and the walk never asks it for its
// type through the vtable because GetByType is stood in for above.
RE::BSExtraData::BSExtraData() = default;

bool RE::BSExtraData::IsNotEqual(const BSExtraData*) const
{
    return true;
}

RE::ExtraAliasInstanceArray::ExtraAliasInstanceArray() = default;

RE::ExtraAliasInstanceArray::~ExtraAliasInstanceArray() = default;

RE::ExtraDataType RE::ExtraAliasInstanceArray::GetType() const
{
    return ExtraDataType::kAliasInstanceArray;
}

// ---------------------------------------------------------------------------
// The high-process actor list
// ---------------------------------------------------------------------------
//
// The engine's set of loaded, actively-simulated actors. Anything that sweeps
// the world for actors near the player walks this rather than the form table,
// so the harness answers it from the registry AddLoadedActor fills.

void RE::ProcessLists::ForEachHighActor(std::function<RE::BSContainer::ForEachResult(RE::Actor*)> a_callback)
{
    if (!a_callback)
        return;
    for (auto* actor : NarrativeEngine::Testing::LoadedActors()) {
        if (a_callback(actor) == RE::BSContainer::ForEachResult::kStop)
            return;
    }
}

// Whether a spell or shout is hostile. Answered globally: what the callers here
// need is the distinction between an attack and a heal, not a spell registry.
bool RE::MagicItem::IsHostile() const
{
    auto* mock = EngineMock::Current();
    return mock != nullptr && mock->magic.spellIsHostile;
}

// ---------------------------------------------------------------------------
// What the player can see
// ---------------------------------------------------------------------------
//
// The visibility fan asks one question of the engine, many times: did a ray
// from the camera reach this point. Standing in at that level rather than
// building geometry is what makes the answer exact — a hit fraction says
// everything an occluder would, and the fan's own logic is what is under test.

namespace NarrativeEngine::Testing
{
    namespace
    {
        // The target's 3D. A real NiNode is not needed: the fan reads the
        // node's world bound and asks for children by name, and both are
        // reachable through zeroed storage plus one stand-in.
        FakeObject& FakeTarget3D()
        {
            static FakeObject node{sizeof(RE::NiNode), 128};
            return node;
        }

        FakeObject& FakeCameraRoot()
        {
            static FakeObject node{sizeof(RE::NiNode), 128};
            return node;
        }
    } // namespace

    RE::NiNode* FabricatedCameraRoot()
    {
        return FakeCameraRoot().As<RE::NiNode>();
    }

    RE::NiAVObject* Fabricated3D()
    {
        auto* mock = EngineMock::Current();
        auto& object = FakeTarget3D();
        auto* node = object.As<RE::NiAVObject>();
        if (mock) {
            node->worldBound.radius = mock->visibility.targetBoundRadius;
            node->worldBound.center = RE::NiPoint3{};
        }
        return node;
    }
} // namespace NarrativeEngine::Testing

RE::NiAVObject* RE::TESObjectREFR::Get3D() const
{
    auto* mock = EngineMock::Current();
    if (!mock || !mock->visibility.target3DPresent)
        return nullptr;
    return NarrativeEngine::Testing::Fabricated3D();
}

RE::NiAVObject* RE::NiAVObject::GetObjectByName(const RE::BSFixedString& a_name)
{
    auto* mock = EngineMock::Current();
    if (!mock)
        return nullptr;
    const char* raw = a_name.c_str();
    const auto it = mock->visibility.namedNodes.find(raw ? raw : "");
    if (it == mock->visibility.namedNodes.end() || !it->second)
        return nullptr;
    // Answering with the root itself is enough: the fan only reads the node's
    // world translate, and a distinct object would add nothing to check.
    return NarrativeEngine::Testing::Fabricated3D();
}

RE::PlayerCamera* RE::PlayerCamera::GetSingleton()
{
    auto* mock = EngineMock::Current();
    if (!mock || !mock->visibility.cameraPresent)
        return nullptr;
    auto* camera = NarrativeEngine::Testing::OpaqueSingleton<RE::PlayerCamera>();
    if (!mock->visibility.cameraRootPresent) {
        // Written through the raw pointer: NiPointer refcounts, and these
        // fabricated nodes are process-lifetime statics with no refcount to
        // manage.
        std::memset(static_cast<void*>(&camera->cameraRoot), 0, sizeof(camera->cameraRoot));
        return camera;
    }
    auto* root = NarrativeEngine::Testing::FabricatedCameraRoot();
    root->world.translate = RE::NiPoint3{mock->visibility.cameraX, mock->visibility.cameraY, mock->visibility.cameraZ};
    std::memcpy(static_cast<void*>(&camera->cameraRoot), &root, sizeof(root));
    return camera;
}

bool RE::Actor::HasLineOfSight(RE::TESObjectREFR*, bool&)
{
    auto* mock = EngineMock::Current();
    return mock != nullptr && mock->visibility.engineLineOfSight;
}

RE::NiAVObject* RE::TES::Pick(RE::bhkPickData& a_pickData)
{
    auto* mock = EngineMock::Current();
    if (!mock)
        return nullptr;
    ++mock->visibility.pickCalls;
    a_pickData.rayOutput.hitFraction = mock->visibility.pickHitFraction;
    return nullptr;
}

RE::hkVector4& RE::hkVector4::operator=(const RE::hkVector4& a_rhs)
{
    quad = a_rhs.quad;
    return *this;
}

// ---------------------------------------------------------------------------
// The world: terrain, water, and moving an actor through it
// ---------------------------------------------------------------------------
//
// A flat plane with an optional rectangular hole, rather than any real
// geometry. What placement code has to get right is which points it asks about
// and what it does with a refusal, and a synthetic world makes both of those
// answerable exactly.

float RE::NiPoint3::GetDistance(const NiPoint3& a_rhs) const noexcept
{
    const float dx = x - a_rhs.x;
    const float dy = y - a_rhs.y;
    const float dz = z - a_rhs.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

RE::TES* RE::TES::GetSingleton()
{
    auto* mock = EngineMock::Current();
    if (!mock || !mock->terrain.tesPresent)
        return nullptr;
    // Two of TES's members are read directly rather than through an accessor,
    // so they are written into the storage here on every fetch instead of
    // being answered from a stand-in function.
    auto* tes = NarrativeEngine::Testing::OpaqueSingleton<RE::TES>();
    tes->interiorCell = mock->grid.playerIndoors ? NarrativeEngine::Testing::FabricatedCell() : nullptr;
    tes->gridCells = mock->grid.present ? NarrativeEngine::Testing::OpaqueSingleton<RE::GridCellArray>() : nullptr;
    return tes;
}

bool RE::TESObjectCELL::IsAttached() const
{
    const auto* facts = NarrativeEngine::Testing::FactsFor(this);
    // A cell the harness never fabricated is the player's own, which is by
    // definition loaded.
    return facts == nullptr || facts->attached;
}

RE::TESObjectCELL* RE::TES::GetCell(const NiPoint3&) const
{
    auto* mock = EngineMock::Current();
    if (!mock || !mock->terrain.cellPresent)
        return nullptr;
    return NarrativeEngine::Testing::FabricatedCell();
}

bool RE::TES::GetLandHeight(const NiPoint3&, float& a_height)
{
    auto* mock = EngineMock::Current();
    if (!mock || !mock->terrain.landHeightResolves)
        return false;
    a_height = mock->terrain.landHeight;
    return true;
}

bool RE::TESObjectCELL::GetWaterHeight(const NiPoint3&, float& a_height)
{
    auto* mock = EngineMock::Current();
    if (!mock || !mock->terrain.hasWater)
        return false;
    a_height = mock->terrain.waterHeight;
    return true;
}

const char* RE::TESObjectREFR::GetName() const
{
    auto* mock = EngineMock::Current();
    return mock ? mock->placement.actorName.c_str() : "";
}

void RE::Actor::EvaluatePackage(bool, bool)
{
    if (auto* mock = EngineMock::Current())
        ++mock->placement.packageEvaluations;
}

void RE::Actor::SetPosition(const NiPoint3& a_pos, bool)
{
    // Writes through to the object as well as recording, so a later
    // GetPosition -- which is inline and reads data.location directly --
    // agrees with where the code just put the actor.
    data.location = a_pos;
    auto* mock = EngineMock::Current();
    if (!mock)
        return;
    const EngineMock::PlacementState::Move move{GetFormID(), a_pos.x, a_pos.y, a_pos.z};
    mock->placement.moves.push_back(move);
    mock->placement.positions[GetFormID()] = move;
}

// ---------------------------------------------------------------------------
// SKSE's ModEvent source
// ---------------------------------------------------------------------------
//
// A real BSTEventSource, not opaque storage. AddEventSink and SendEvent are
// both inline header code that walk the source's own arrays, so a genuine one
// lets a test register a production sink and then dispatch to it the way the
// Papyrus VM does. That is the only route to a sink defined in an anonymous
// namespace.
//
// Function-local static so it outlives every EngineMock: a registered sink is
// never unregistered, and the source would otherwise be left holding a pointer
// into a destroyed object.

namespace NarrativeEngine::Testing
{
    RE::BSTEventSource<SKSE::ModCallbackEvent>& EngineMock::ModEventSource()
    {
        // Deliberately never destroyed. See the note on the form tables in
        // RelocationMocks.cpp: a CommonLibSSE container frees its storage
        // through the mocked memory manager, which it reaches through a
        // relocation, and at static-destruction time there is no guaranteed
        // order between the two. Getting it wrong is a crash after the last
        // assertion has already passed, which reads as a flaky test.
        static auto* source = new RE::BSTEventSource<SKSE::ModCallbackEvent>();
        return *source;
    }
} // namespace NarrativeEngine::Testing

// SKSE's log directory. Out-of-line, and pointed at a directory under the test
// working tree so a module that keeps its own trace file writes a real one a
// test can read back.

std::optional<std::filesystem::path> SKSE::log::log_directory()
{
    auto* mock = EngineMock::Current();
    if (!mock || !mock->logging.directoryPresent)
        return std::nullopt;
    std::filesystem::path dir{mock->logging.directory};
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

RE::BSTEventSource<SKSE::ModCallbackEvent>* SKSE::GetModCallbackEventSource() noexcept
{
    auto* mock = EngineMock::Current();
    if (!mock || !mock->modEvents.sourcePresent)
        return nullptr;
    return &EngineMock::ModEventSource();
}

const SKSE::TaskInterface* SKSE::GetTaskInterface() noexcept
{
    auto* mock = EngineMock::Current();
    if (!mock || !mock->tasks.interfacePresent)
        return nullptr;
    return NarrativeEngine::Testing::OpaqueSingleton<SKSE::TaskInterface>();
}

void SKSE::TaskInterface::AddTask(TaskFn a_task) const
{
    auto* mock = EngineMock::Current();
    if (!mock)
        return;
    ++mock->tasks.queued;
    if (mock->tasks.runImmediately) {
        a_task();
    } else {
        mock->tasks.held.push_back(std::move(a_task));
    }
}

// ---------------------------------------------------------------------------
// RE::BSReadWriteLock / RE::TESObjectREFR liveness
// ---------------------------------------------------------------------------

RE::BSReadWriteLock::BSReadWriteLock()
{
    // The real one zeroes its wait-count word. Nothing in a single-threaded
    // test contends for it, so default-constructing is the whole job.
    std::memset(static_cast<void*>(this), 0, sizeof(*this));
}

bool RE::TESObjectREFR::IsDead(bool) const
{
    auto* mock = EngineMock::Current();
    return mock != nullptr && mock->forms.actorIsDead;
}

bool RE::TESObjectREFR::IsDisabled() const
{
    auto* mock = EngineMock::Current();
    return mock != nullptr && mock->forms.actorIsDisabled;
}

namespace NarrativeEngine::Testing
{
    namespace
    {
        // Fabricated actors, kept in a deque so their addresses stay stable as
        // more are added, plus the subset ProcessLists reports as loaded.
        struct ActorRegistry
        {
            std::deque<FakeObject> objects;
            std::unordered_map<std::uint32_t, RE::Actor*> byFormID;
            std::vector<RE::Actor*> loaded;

            void Clear()
            {
                objects.clear();
                byFormID.clear();
                loaded.clear();
            }
        };

        ActorRegistry& Actors()
        {
            static ActorRegistry registry;
            return registry;
        }

        std::deque<FakeObject>& FactionObjects()
        {
            static std::deque<FakeObject> objects;
            return objects;
        }
    } // namespace

    RE::Actor* EngineMock::AddActor(std::uint32_t formID)
    {
        auto& registry = Actors();
        if (const auto it = registry.byFormID.find(formID); it != registry.byFormID.end())
            return it->second;

        auto& object = registry.objects.emplace_back(kActorStorageBytes, 256);
        WireFormDefaults(object, true);
        auto* form = object.As<RE::TESForm>();
        // As<Actor>() switches on this, so it is what makes the cast succeed.
        form->formType = RE::FormType::ActorCharacter;
        form->formID = formID;
        FormTable().insert({formID, form});

        auto* actor = object.As<RE::Actor>();
        registry.byFormID.emplace(formID, actor);
        return actor;
    }

    RE::Actor* EngineMock::AddLoadedActor(std::uint32_t formID)
    {
        auto* actor = AddActor(formID);
        Actors().loaded.push_back(actor);
        return actor;
    }

    // -----------------------------------------------------------------------
    // References in a cell, and the doors between cells
    // -----------------------------------------------------------------------
    //
    // Extra data is not built for real. ExtraDataList is a linked structure the
    // engine allocates and walks, and the only question asked of one here is
    // whether it holds teleport data — so the harness keeps that answer in a
    // side table keyed by the list's own address, and the teleport block is
    // bytes rather than a constructed ExtraTeleport, whose vtable is declared
    // and never defined.

    namespace
    {
        struct RefPool
        {
            std::deque<FakeObject> objects;
            std::deque<std::vector<std::byte>> teleportBlocks;
            std::deque<RE::DoorTeleportData*> doorData;
            // What lives in each cell, in the order it was added, which is the
            // order a sweep of the cell reports them in.
            std::unordered_map<const void*, std::vector<RE::TESObjectREFR*>> byCell;
            // The teleport block hanging off each extra-data list.
            std::unordered_map<const void*, RE::BSExtraData*> teleportByList;
            // Which reference each handle resolves to. Handles start at one:
            // zero is the engine's "no reference".
            std::vector<RE::TESObjectREFR*> byHandle;
        };

        RefPool& Refs()
        {
            static auto* pool = new RefPool();
            return *pool;
        }

        RE::ObjectRefHandle HandleFor(RE::TESObjectREFR* ref)
        {
            auto& pool = Refs();
            pool.byHandle.push_back(ref);
            RE::ObjectRefHandle handle;
            const auto raw = static_cast<std::uint32_t>(pool.byHandle.size());
            std::memcpy(static_cast<void*>(&handle), &raw, sizeof(raw));
            return handle;
        }
    } // namespace

    const std::vector<RE::TESObjectREFR*>& ReferencesIn(const void* cell)
    {
        static auto* empty = new std::vector<RE::TESObjectREFR*>();
        const auto& table = Refs().byCell;
        const auto it = table.find(cell);
        return it == table.end() ? *empty : it->second;
    }

    RE::TESObjectREFR* ReferenceForHandle(std::uint32_t raw)
    {
        const auto& table = Refs().byHandle;
        return (raw == 0 || raw > table.size()) ? nullptr : table[raw - 1];
    }

    RE::BSExtraData* TeleportDataOn(const void* extraList)
    {
        const auto& table = Refs().teleportByList;
        const auto it = table.find(extraList);
        return it == table.end() ? nullptr : it->second;
    }

    RE::TESObjectREFR* EngineMock::AddReference(RE::TESObjectCELL* cell, std::uint32_t formID, RE::NiPoint3 position)
    {
        auto& pool = Refs();
        auto& object = pool.objects.emplace_back(sizeof(RE::TESObjectREFR) + 0x200, 256);
        WireFormDefaults(object, true);
        auto* form = object.As<RE::TESForm>();
        form->formType = RE::FormType::Reference;
        form->formID = formID;
        FormTable().insert({formID, form});

        auto* ref = object.As<RE::TESObjectREFR>();
        ref->data.location = position;
        ref->parentCell = cell;
        if (cell)
            pool.byCell[static_cast<const void*>(cell)].push_back(ref);
        return ref;
    }

    namespace
    {
        // A teleport block: zeroed bytes with the data pointer written where
        // ExtraTeleport keeps it.
        RE::BSExtraData* MakeTeleport(RE::DoorTeleportData* data)
        {
            auto& block = Refs().teleportBlocks.emplace_back(sizeof(RE::ExtraTeleport), std::byte{});
            std::memcpy(block.data() + offsetof(RE::ExtraTeleport, teleportData), &data, sizeof(data));
            return reinterpret_cast<RE::BSExtraData*>(block.data());
        }
    } // namespace

    RE::TESObjectREFR* EngineMock::AddLoadDoor(RE::TESObjectCELL* cell,
                                               std::uint32_t formID,
                                               RE::TESObjectCELL* destination,
                                               RE::NiPoint3 arrival)
    {
        auto* door = AddReference(cell, formID, RE::NiPoint3{});
        // The door on the far side. Nothing reads its own position — what
        // matters is the cell it stands in, which is how a caller tells a way
        // outdoors from a door into another room.
        auto* farSide = AddReference(destination, formID + 1u, arrival);

        auto* data = new RE::DoorTeleportData();
        Refs().doorData.push_back(data);
        data->linkedDoor = HandleFor(farSide);
        data->position = arrival;

        Refs().teleportByList[static_cast<const void*>(&door->extraList)] = MakeTeleport(data);
        return door;
    }

    RE::TESObjectREFR* EngineMock::AddUnlinkedDoor(RE::TESObjectCELL* cell, std::uint32_t formID)
    {
        auto* door = AddReference(cell, formID, RE::NiPoint3{});
        Refs().teleportByList[static_cast<const void*>(&door->extraList)] = MakeTeleport(nullptr);
        return door;
    }

    void EngineMock::SetLocationMarker(RE::BGSLocation* location, RE::TESObjectCELL* cell, RE::NiPoint3 position)
    {
        if (!location)
            return;
        static std::uint32_t nextMarker = 0x00B00001u;
        auto* marker = AddReference(cell, nextMarker++, position);
        location->worldLocMarker = HandleFor(marker);
    }

    RE::TESFaction* EngineMock::AddFaction(std::uint32_t formID, std::string editorID)
    {
        auto& object = FactionObjects().emplace_back(sizeof(RE::TESFaction), 256);
        WireFormDefaults(object, false);
        object.Slot(0x32, reinterpret_cast<void*>(&FormEditorIDImpl));
        auto* form = object.As<RE::TESForm>();
        form->formType = RE::FormType::Faction;
        form->formID = formID;
        FormTable().insert({formID, form});
        FormsOfType(RE::FormType::Faction).push_back(form);
        if (!editorID.empty()) {
            SetEditorID(form, editorID);
            EditorIDTable().insert({RE::BSFixedString(editorID.c_str()), form});
        }
        return object.As<RE::TESFaction>();
    }

    namespace
    {
        // Forms a content file names but nothing here reads through. Levelled
        // lists are compared by identity and globals are read for one float,
        // so neither needs more than storage with the right type on it.
        std::deque<FakeObject>& SimpleForms()
        {
            static auto* pool = new std::deque<FakeObject>();
            return *pool;
        }
    } // namespace

    std::deque<FakeObject>& SimpleFormObjects()
    {
        return SimpleForms();
    }

    RE::TESLevCharacter* EngineMock::AddLeveledCharacter(std::uint32_t formID, std::string editorID)
    {
        auto& object = SimpleForms().emplace_back(sizeof(RE::TESLevCharacter) + 0x40, 128);
        WireFormDefaults(object, false);
        object.Slot(0x32, reinterpret_cast<void*>(&FormEditorIDImpl));
        auto* form = object.As<RE::TESForm>();
        form->formType = RE::FormType::LeveledNPC;
        form->formID = formID;
        FormTable().insert({formID, form});
        if (!editorID.empty()) {
            SetEditorID(form, editorID);
            EditorIDTable().insert({RE::BSFixedString(editorID.c_str()), form});
        }
        return object.As<RE::TESLevCharacter>();
    }

    RE::TESGlobal* EngineMock::AddGlobal(std::uint32_t formID, std::string editorID, float value)
    {
        auto& object = SimpleForms().emplace_back(sizeof(RE::TESGlobal) + 0x40, 128);
        WireFormDefaults(object, false);
        object.Slot(0x32, reinterpret_cast<void*>(&FormEditorIDImpl));
        auto* form = object.As<RE::TESForm>();
        form->formType = RE::FormType::Global;
        form->formID = formID;
        FormTable().insert({formID, form});
        if (!editorID.empty()) {
            SetEditorID(form, editorID);
            EditorIDTable().insert({RE::BSFixedString(editorID.c_str()), form});
        }
        auto* global = object.As<RE::TESGlobal>();
        global->value = value;
        return global;
    }

    void EngineMock::SetFactionRank(RE::Actor* actor, RE::TESFaction* faction, int rank)
    {
        if (!actor || !faction)
            return;
        factions.ranks[{actor->GetFormID(), faction->GetFormID()}] = rank;
    }

    int EngineMock::FactionRank(RE::Actor* actor, RE::TESFaction* faction) const
    {
        if (!actor || !faction)
            return -1;
        const auto it = factions.ranks.find({actor->GetFormID(), faction->GetFormID()});
        return it != factions.ranks.end() ? it->second : -1;
    }

    std::vector<RE::Actor*>& LoadedActors()
    {
        return Actors().loaded;
    }

    void ClearActorRegistry()
    {
        Actors().Clear();
        FactionObjects().clear();
    }
} // namespace NarrativeEngine::Testing

// ---------------------------------------------------------------------------
// RE::TESQuest / RE::BGSRefAlias / RE::TESObjectREFR — the courier path
// ---------------------------------------------------------------------------

bool RE::TESQuest::IsRunning() const
{
    // Per-quest when the harness fabricated this one, because a single alias
    // walk sees several quests and has to tell them apart. The global answer
    // remains for the courier's own quest, which is fabricated elsewhere.
    if (const auto* state = NarrativeEngine::Testing::QuestStateFor(this))
        return state->running;
    auto* mock = EngineMock::Current();
    return mock != nullptr && mock->courier.questIsRunning;
}

std::uint16_t RE::TESQuest::GetCurrentStageID() const
{
    auto* mock = EngineMock::Current();
    return mock ? mock->courier.questStage : std::uint16_t{0};
}

RE::TESObjectREFR* RE::BGSRefAlias::GetReference() const
{
    // An alias the harness built answers for itself, because a quest with two
    // of them fills them at different moments and the code under test is
    // waiting on exactly that difference. The courier's own Container alias is
    // fabricated elsewhere and has no entry, so it keeps the global answer.
    const auto& references = NarrativeEngine::Testing::AliasReferences();
    if (const auto it = references.find(static_cast<const void*>(this)); it != references.end())
        return it->second;

    auto* mock = EngineMock::Current();
    if (!mock || !mock->courier.aliasHasReference)
        return nullptr;
    return NarrativeEngine::Testing::OpaqueSingleton<RE::TESObjectREFR>();
}

bool RE::TESQuest::EnsureQuestStarted(bool& a_result, bool)
{
    auto* mock = EngineMock::Current();
    if (!mock)
        return false;
    mock->questControl.started.push_back(static_cast<const void*>(this));
    a_result = mock->questControl.startResult;
    if (a_result) {
        if (auto* state = NarrativeEngine::Testing::MutableQuestStateFor(this)) {
            state->running = true;
            state->stopped = false;
        }
    }
    return mock->questControl.startCallSucceeds;
}

void RE::TESQuest::Stop()
{
    if (auto* mock = EngineMock::Current())
        mock->questControl.stopped.push_back(static_cast<const void*>(this));
    if (auto* state = NarrativeEngine::Testing::MutableQuestStateFor(this)) {
        state->running = false;
        state->stopped = true;
    }
}

void RE::TESQuest::Reset()
{
    if (auto* mock = EngineMock::Current())
        mock->questControl.reset.push_back(static_cast<const void*>(this));
    // Reset clears alias fills, which is why production has to delete the
    // letter reference BEFORE resetting the quest that points at it.
    for (auto* alias : aliases) {
        if (alias)
            NarrativeEngine::Testing::AliasReferences()[static_cast<const void*>(alias)] = nullptr;
    }
}

void RE::TESObjectREFR::MoveTo(RE::TESObjectREFR* a_target)
{
    // Recorded rather than reproduced. The engine's own body unloads the
    // mover's 3D, reparents them to the target's cell and reloads them there;
    // what the code under test is responsible for is choosing the target.
    if (auto* mock = EngineMock::Current())
        mock->questControl.teleports.push_back({GetFormID(), a_target ? a_target->GetFormID() : 0u});
}

void RE::TESObjectREFR::Disable()
{
    if (auto* mock = EngineMock::Current())
        mock->questControl.disabled.push_back(GetFormID());
}

std::map<RE::TESBoundObject*, int> RE::TESObjectREFR::GetInventoryCounts(
    std::function<bool(RE::TESBoundObject&)> a_filter,
    bool)
{
    std::map<RE::TESBoundObject*, int> counts;
    auto* mock = EngineMock::Current();
    if (!mock)
        return counts;
    ++mock->courier.getInventoryCountsCalls;
    if (mock->courier.inventoryCount < 0)
        return counts; // the book simply is not in the container

    // The filter is what production uses to ask about one specific book, so
    // run it against the registered books rather than inventing an entry: a
    // filter that matched the wrong object would otherwise go unnoticed.
    for (auto& entry : NarrativeEngine::Testing::FormTable()) {
        auto* form = entry.second;
        auto* bound = form ? form->As<RE::TESBoundObject>() : nullptr;
        if (bound && a_filter(*bound))
            counts.emplace(bound, mock->courier.inventoryCount);
    }
    return counts;
}

namespace NarrativeEngine::Testing
{

    RE::TESQuest* EngineMock::AddCourierQuest(bool withContainerAlias, bool withContainerRef)
    {
        static FakeObject quest{sizeof(RE::TESQuest), 256};
        static FakeObject alias{sizeof(RE::BGSRefAlias), 64};
        static FakeObject containerRef{sizeof(RE::TESObjectREFR), 256};

        WireFormDefaults(quest, false);
        auto* questForm = quest.As<RE::TESForm>();
        questForm->formType = RE::FormType::Quest;
        questForm->formID = 0x0002C6BFu;

        auto* q = quest.As<RE::TESQuest>();
        q->aliases.clear();
        if (withContainerAlias) {
            auto* a = alias.As<RE::BGSBaseAlias>();
            a->aliasName = "Container";
            q->aliases.push_back(a);
        }

        EditorIDTable().insert({RE::BSFixedString("WICourier"), questForm});

        if (withContainerRef) {
            WireFormDefaults(containerRef, true);
            auto* refForm = containerRef.As<RE::TESForm>();
            refForm->formType = RE::FormType::Reference;
            refForm->formID = 0x0004D8F1u;
            EditorIDTable().insert({RE::BSFixedString("WICourierContainerRef"), refForm});
        }
        return q;
    }

    RE::TESForm* EngineMock::AddBook(std::uint32_t formID)
    {
        static FakeObject book{sizeof(RE::TESObjectBOOK), 256};
        WireFormDefaults(book, false);
        auto* form = book.As<RE::TESForm>();
        // Book is a bound object, which is what As<TESBoundObject>() checks.
        form->formType = RE::FormType::Book;
        form->formID = formID;
        FormTable().insert({formID, form});
        return form;
    }
} // namespace NarrativeEngine::Testing

// ---------------------------------------------------------------------------
// RE::ProcessLists / actor handles / faction ranks
// ---------------------------------------------------------------------------

namespace NarrativeEngine::Testing
{
    namespace
    {
        FakeObject& FakeProcessLists()
        {
            static FakeObject lists{sizeof(RE::ProcessLists), 64};
            return lists;
        }
    } // namespace
} // namespace NarrativeEngine::Testing

RE::ProcessLists* RE::ProcessLists::GetSingleton()
{
    using namespace NarrativeEngine::Testing;
    auto* mock = EngineMock::Current();
    if (!mock)
        return nullptr;

    auto* lists = FakeProcessLists().As<RE::ProcessLists>();
    lists->highActorHandles.clear();
    lists->middleHighActorHandles.clear();
    lists->middleLowActorHandles.clear();
    lists->lowActorHandles.clear();

    // Handles are index+1 into LoadedActors(), so a zero handle reads as
    // "nothing" -- which is what an unset ActorHandle means in the engine too.
    //
    // Everything goes in the high list. The sweep walks all four in turn and
    // treats them identically, so spreading actors across them would be testing
    // the engine's bucketing rather than ours.
    for (std::size_t i = 0; i < LoadedActors().size(); ++i) {
        RE::ActorHandle handle;
        const auto raw = static_cast<std::uint32_t>(i + 1);
        std::memcpy(static_cast<void*>(&handle), &raw, sizeof(raw));
        lists->highActorHandles.push_back(handle);
    }
    return lists;
}

bool RE::BSPointerHandle<RE::Actor, RE::BSUntypedPointerHandle<21, 5>>::get_smartptr(
    RE::NiPointer<RE::Actor>& a_smartPointerOut) const
{
    using namespace NarrativeEngine::Testing;
    // The handle is a single uint32; reading it directly avoids depending on
    // accessors of a base class we do not own.
    std::uint32_t raw = 0;
    std::memcpy(&raw, static_cast<const void*>(this), sizeof(raw));
    if (raw == 0 || raw > LoadedActors().size()) {
        a_smartPointerOut.reset();
        return false;
    }
    a_smartPointerOut.reset(LoadedActors()[raw - 1]);
    return true;
}

bool RE::BSPointerHandle<RE::TESObjectREFR, RE::BSUntypedPointerHandle<21, 5>>::get_smartptr(
    RE::NiPointer<RE::TESObjectREFR>& a_smartPointerOut) const
{
    // A handle is one uint32; reading it directly avoids depending on the
    // accessors of a base class the harness does not own.
    std::uint32_t raw = 0;
    std::memcpy(&raw, static_cast<const void*>(this), sizeof(raw));
    auto* ref = NarrativeEngine::Testing::ReferenceForHandle(raw);
    a_smartPointerOut.reset(ref);
    return ref != nullptr;
}

RE::TESNPC* RE::Actor::GetActorBase()
{
    return NarrativeEngine::Testing::BaseFormOf(this);
}

RE::SEXES::SEX RE::TESNPC::GetSex() const
{
    // Read off the base form rather than from a mock-wide flag: a rumor names
    // several people at once, and each of them needs a pronoun of their own.
    return actorData.actorBaseFlags.any(RE::ACTOR_BASE_DATA::Flag::kFemale) ? RE::SEXES::kFemale : RE::SEXES::kMale;
}

bool RE::Actor::IsInFaction(const RE::TESFaction* a_faction) const
{
    // Answered from the same rank table SetFactionRank fills: membership is
    // rank zero or better, which is what the engine means by it too.
    auto* mock = NarrativeEngine::Testing::EngineMock::Current();
    if (!mock || !a_faction)
        return false;
    return mock->FactionRank(const_cast<RE::Actor*>(this), const_cast<RE::TESFaction*>(a_faction)) >= 0;
}

std::uint16_t RE::Actor::GetLevel() const
{
    auto* mock = NarrativeEngine::Testing::EngineMock::Current();
    return mock ? mock->world.actorLevel : static_cast<std::uint16_t>(1);
}

bool RE::Actor::HasKeyword(const RE::BGSKeyword* a_keyword) const
{
    // Actors carry the keywords of their base form, and the harness keeps the
    // answer beside the actor for the same reason it keeps the base form
    // there: the field the engine reads sits inside a relocated block.
    return NarrativeEngine::Testing::ActorHasKeyword(this, a_keyword);
}

RE::TESRace* RE::TESNPC::GetRace()
{
    return NarrativeEngine::Testing::RaceOf(this);
}

RE::BGSLocation* RE::TESObjectREFR::GetEditorLocation() const
{
    // Where a reference was placed in the editor, which is not where it is
    // now. Nothing here moves references, so a fabricated one has an editor
    // location only if a test gave it one.
    return NarrativeEngine::Testing::EditorLocationOf(this);
}

bool RE::TESObjectREFR::Is3DLoaded() const
{
    // Whether the reference's model is in memory. Distinct from whether it
    // exists: a persistent NPC on the other side of the province is a live
    // object with nothing rendered.
    auto* mock = NarrativeEngine::Testing::EngineMock::Current();
    return mock != nullptr && mock->visibility.target3DPresent;
}

float RE::TESObjectREFR::GetAngleZ() const
{
    // Which way a reference is facing. Read off the object rather than from a
    // mock-wide field, because a search that reasons about what is behind
    // someone has to be able to turn two of them different ways.
    return data.angle.z;
}

void RE::TESObjectCELL::ForEachReference(std::function<RE::BSContainer::ForEachResult(RE::TESObjectREFR*)> a_fn) const
{
    for (auto* ref : NarrativeEngine::Testing::ReferencesIn(static_cast<const void*>(this))) {
        if (a_fn(ref) == RE::BSContainer::ForEachResult::kStop)
            return;
    }
}

void RE::BSHandleRefObject::IncRefCount()
{
    // See DecRefCount: the registry owns fabricated actors outright.
}

void RE::BSHandleRefObject::DecRefCount()
{
    // Fabricated actors are owned by the registry, not by the smart pointers
    // handed out over them, so releasing one must not touch the object.
}

int RE::Actor::GetFactionRank(RE::TESFaction* a_faction, bool)
{
    // The `a_isPlayer` argument only steers the engine's player-crime path,
    // which nothing here models.
    auto* mock = NarrativeEngine::Testing::EngineMock::Current();
    return mock ? mock->FactionRank(this, a_faction) : -1;
}

void RE::Actor::AddToFaction(RE::TESFaction* a_faction, std::int8_t a_rank)
{
    auto* mock = NarrativeEngine::Testing::EngineMock::Current();
    if (!mock || !a_faction)
        return;
    mock->factions.addToFactionCalls.push_back({GetFormID(), a_faction->GetFormID(), a_rank});
    mock->SetFactionRank(this, a_faction, a_rank);
}

// ---------------------------------------------------------------------------
// The main-thread engine reads: player, cell, location, sky
// ---------------------------------------------------------------------------
//
// These are the harness's first fabricated objects with a SECONDARY vtable.
// `TESFullName::GetFullName` is slot 05 of the TESFullName vtable, and
// BGSLocation and TESObjectCELL each inherit TESFullName as a base past the
// first, so its vptr sits at its own offset rather than at zero. FakeObject's
// BaseSlot writes it there; see testsupport/FakeVTable.h.

namespace NarrativeEngine::Testing
{
    namespace
    {
        // Returns `fullName`, exactly as the engine's own override does. The
        // `self` a virtual call passes here is already adjusted to the
        // TESFullName subobject, so no further fixing up is needed.
        const char* FullNameImpl(void* self)
        {
            return static_cast<RE::TESFullName*>(self)->fullName.c_str();
        }

        // BGSKeywordForm::HasKeyword is slot 04 of ITS vtable, and
        // BGSKeywordForm is another base past the first. Walks the same span
        // the engine's own override walks.
        bool HasKeywordImpl(void* self, const RE::BGSKeyword* keyword)
        {
            auto* form = static_cast<RE::BGSKeywordForm*>(self);
            for (auto* candidate : form->GetKeywords()) {
                if (candidate == keyword)
                    return true;
            }
            return false;
        }

        template <class T> void WireKeywordForm(FakeObject& object)
        {
            auto* whole = object.As<T>();
            const auto offset =
                static_cast<std::size_t>(reinterpret_cast<std::byte*>(static_cast<RE::BGSKeywordForm*>(whole))
                                         - reinterpret_cast<std::byte*>(whole));
            object.BaseSlot(offset, 8, 4, reinterpret_cast<void*>(&HasKeywordImpl));
        }

        template <class T> void WireFullName(FakeObject& object)
        {
            auto* whole = object.As<T>();
            const auto offset =
                static_cast<std::size_t>(reinterpret_cast<std::byte*>(static_cast<RE::TESFullName*>(whole))
                                         - reinterpret_cast<std::byte*>(whole));
            object.BaseSlot(offset, 8, 5, reinterpret_cast<void*>(&FullNameImpl));
        }

        FakeObject& FakePlayer()
        {
            static FakeObject player{sizeof(RE::PlayerCharacter), 256};
            return player;
        }

        FakeObject& FakeCell()
        {
            static FakeObject cell{kCellStorageBytes, 128};
            return cell;
        }

        // The cell TES::GetCell hands back for any position. The same object
        // the player stands in, wired up whether or not a test gave the player
        // a cell of its own: placement code asks the world for the cell at a
        // coordinate, not for the player's.
        RE::TESObjectCELL* FabricatedCellImpl()
        {
            auto* mock = EngineMock::Current();
            auto& cellObject = FakeCell();
            WireFormDefaults(cellObject, false);
            WireFullName<RE::TESObjectCELL>(cellObject);
            cellObject.Slot(0x32, reinterpret_cast<void*>(&FormEditorIDImpl));
            auto* cell = cellObject.As<RE::TESObjectCELL>();
            cell->formType = RE::FormType::Cell;
            if (mock) {
                cell->formID = mock->world.cellFormID;
                cell->fullName = mock->world.cellName.c_str();
                SetEditorID(cell, mock->world.cellEditorID);
            }
            return cell;
        }

        FakeObject& FakeLocation()
        {
            static FakeObject location{sizeof(RE::BGSLocation), 128};
            return location;
        }

        FakeObject& FakeSky()
        {
            static FakeObject sky{sizeof(RE::Sky), 64};
            return sky;
        }

        FakeObject& FakeWeather()
        {
            static FakeObject weather{sizeof(RE::TESWeather), 128};
            return weather;
        }

        FakeObject& FakeParentLocation()
        {
            static FakeObject location{sizeof(RE::BGSLocation), 128};
            return location;
        }
    } // namespace

    RE::TESObjectCELL* FabricatedCell()
    {
        return FabricatedCellImpl();
    }
} // namespace NarrativeEngine::Testing

RE::BGSLocation* RE::TESObjectREFR::GetCurrentLocation() const
{
    using namespace NarrativeEngine::Testing;
    auto* mock = EngineMock::Current();
    if (!mock || !mock->world.playerHasLocation)
        return nullptr;
    if (mock->world.playerLocationOverride)
        return mock->world.playerLocationOverride;

    auto& object = FakeLocation();
    WireFormDefaults(object, false);
    WireFullName<RE::BGSLocation>(object);
    WireKeywordForm<RE::BGSLocation>(object);
    object.Slot(0x32, reinterpret_cast<void*>(&FormEditorIDImpl));
    auto* location = object.As<RE::BGSLocation>();
    location->formID = mock->world.locationFormID;
    location->formType = RE::FormType::Location;
    location->fullName = mock->world.locationName.c_str();
    location->keywords = nullptr;
    location->numKeywords = 0;
    SetEditorID(location, mock->world.locationEditorID);

    if (mock->world.locationParentEditorID.empty()) {
        location->parentLoc = nullptr;
    } else {
        auto& parentObject = FakeParentLocation();
        WireFormDefaults(parentObject, false);
        WireFullName<RE::BGSLocation>(parentObject);
        WireKeywordForm<RE::BGSLocation>(parentObject);
        parentObject.Slot(0x32, reinterpret_cast<void*>(&FormEditorIDImpl));
        auto* parent = parentObject.As<RE::BGSLocation>();
        parent->formID = mock->world.locationFormID + 1u;
        parent->formType = RE::FormType::Location;
        parent->fullName = mock->world.locationName.c_str();
        parent->parentLoc = nullptr;
        parent->keywords = nullptr;
        parent->numKeywords = 0;
        SetEditorID(parent, mock->world.locationParentEditorID);
        location->parentLoc = parent;
    }
    return location;
}

bool RE::TESObjectCELL::IsInteriorCell() const
{
    // Per-cell when the harness fabricated this one as an exterior: a grid
    // builder walks many cells at once and has to tell them apart. The global
    // answer remains for the player's own cell, which most tests steer through
    // world.cellIsInterior without caring about any others.
    if (const auto* facts = NarrativeEngine::Testing::FactsFor(this))
        return facts->isInterior;
    auto* mock = NarrativeEngine::Testing::EngineMock::Current();
    return mock != nullptr && mock->world.cellIsInterior;
}

// TESForm::GetName is out-of-line, and the ModEvent sink reads it off an
// event's sender to say who sent it. Answers out of the mock so a test can put
// a name on the sender without fabricating a whole form.

const char* RE::TESForm::GetName() const
{
    // Per-form first, because anything naming several people at once needs
    // them told apart. The mock-wide name remains the answer for a form
    // nobody gave one to, which is how the ModEvent sink's sender is set.
    if (const char* named = NarrativeEngine::Testing::FormName(this))
        return named;
    auto* mock = NarrativeEngine::Testing::EngineMock::Current();
    return mock ? mock->modEvents.senderName.c_str() : "";
}

const char* RE::TESObjectREFR::GetDisplayFullName()
{
    auto* mock = NarrativeEngine::Testing::EngineMock::Current();
    return mock ? mock->world.actorDisplayName.c_str() : "";
}

bool RE::Actor::IsPlayerTeammate() const
{
    auto* mock = NarrativeEngine::Testing::EngineMock::Current();
    return mock != nullptr && mock->world.actorIsPlayerTeammate;
}

RE::Sky* RE::Sky::GetSingleton()
{
    using namespace NarrativeEngine::Testing;
    auto* mock = EngineMock::Current();
    if (!mock || !mock->sky.present)
        return nullptr;

    auto* sky = FakeSky().As<RE::Sky>();
    sky->mode = static_cast<RE::Sky::Mode>(mock->sky.mode);
    if (mock->sky.hasWeather) {
        auto& object = FakeWeather();
        WireFormDefaults(object, false);
        auto* weather = object.As<RE::TESWeather>();
        weather->formID = mock->sky.weatherFormID;
        weather->formType = RE::FormType::Weather;
        weather->data.flags = static_cast<RE::TESWeather::WeatherDataFlag>(mock->sky.weatherFlags);
        weather->data.windSpeed = mock->sky.windSpeed;
        weather->data.thunderLightningFrequency = mock->sky.thunderLightningFrequency;
        sky->currentWeather = weather;
    } else {
        sky->currentWeather = nullptr;
    }
    return sky;
}

namespace NarrativeEngine::Testing
{
    RE::PlayerCharacter* BuildFakePlayer()
    {
        auto* mock = EngineMock::Current();
        auto& object = FakePlayer();
        WireFormDefaults(object, true);
        auto* pc = object.As<RE::PlayerCharacter>();
        pc->formType = RE::FormType::ActorCharacter;
        pc->formID = mock->world.playerFormID;
        pc->data.location = RE::NiPoint3{mock->world.playerX, mock->world.playerY, mock->world.playerZ};

        if (mock->world.playerHasCell) {
            auto& cellObject = FakeCell();
            WireFormDefaults(cellObject, false);
            WireFullName<RE::TESObjectCELL>(cellObject);
            cellObject.Slot(0x32, reinterpret_cast<void*>(&FormEditorIDImpl));
            auto* cell = cellObject.As<RE::TESObjectCELL>();
            cell->formID = mock->world.cellFormID;
            cell->formType = RE::FormType::Cell;
            cell->fullName = mock->world.cellName.c_str();
            SetEditorID(cell, mock->world.cellEditorID);
            // Where the cell sits in its exterior, and only when a test has
            // actually placed the player there. A grid lookup needs the
            // worldspace and the coordinates together; without either the
            // player is simply not in the grid, which is the state every test
            // that does not care about the grid wants.
            if (mock->world.playerCellWorldSpace) {
                cell->GetRuntimeData().worldSpace = mock->world.playerCellWorldSpace;
                RegisterCellFacts(cell, mock->world.playerCellX, mock->world.playerCellY, nullptr);
                // Registering facts is what makes IsInteriorCell answer per
                // cell rather than from the mock-wide flag, so the flag has to
                // be carried onto them or a test that puts the player indoors
                // after placing them in the grid would be ignored.
                mock->SetCellInterior(cell, mock->world.cellIsInterior);
            }
            pc->parentCell = cell;
        } else {
            pc->parentCell = nullptr;
        }
        return pc;
    }
} // namespace NarrativeEngine::Testing

// ---------------------------------------------------------------------------
// RE::Calendar — the rest of the reading
// ---------------------------------------------------------------------------

float RE::Calendar::GetDaysPassed() const
{
    auto* mock = EngineMock::Current();
    return mock ? mock->calendar.daysPassed : 0.0f;
}

std::uint32_t RE::Calendar::GetYear() const
{
    auto* mock = EngineMock::Current();
    return mock ? mock->calendar.year : 0u;
}

std::uint32_t RE::Calendar::GetMonth() const
{
    auto* mock = EngineMock::Current();
    return mock ? mock->calendar.month : 0u;
}

float RE::Calendar::GetDay() const
{
    auto* mock = EngineMock::Current();
    return mock ? mock->calendar.day : 0.0f;
}

float RE::Calendar::GetHour() const
{
    auto* mock = EngineMock::Current();
    return mock ? mock->calendar.hour : 0.0f;
}

// ---------------------------------------------------------------------------
// Keywords and locations
// ---------------------------------------------------------------------------

namespace NarrativeEngine::Testing
{
    namespace
    {
        // Editor IDs by form, since a TESForm does not carry one at runtime
        // without powerofthree's Tweaks -- which is exactly the dependency

        // Deliberately never cleared: see EngineMock::AddKeyword.
        struct KeywordPool
        {
            std::deque<FakeObject> objects;
            std::unordered_map<std::string, RE::BGSKeyword*> byEditorID;
        };

        KeywordPool& Keywords()
        {
            static KeywordPool pool;
            return pool;
        }

        struct LocationPool
        {
            std::deque<FakeObject> objects;
            std::deque<std::vector<RE::BGSKeyword*>> keywordArrays;

            void Clear()
            {
                objects.clear();
                keywordArrays.clear();
            }
        };

        LocationPool& Locations()
        {
            static LocationPool pool;
            return pool;
        }
    } // namespace

    void ClearLocationPool()
    {
        Locations().Clear();
    }

    void ClearEditorIDsByForm()
    {
        EditorIDsByForm().clear();
    }

    RE::BGSKeyword* EngineMock::AddKeyword(std::string_view editorID)
    {
        auto& pool = Keywords();
        const std::string key{editorID};
        auto it = pool.byEditorID.find(key);
        if (it == pool.byEditorID.end()) {
            auto& object = pool.objects.emplace_back(sizeof(RE::BGSKeyword), 128);
            WireFormDefaults(object, false);
            // GetFormEditorID is slot 0x32 on TESForm.
            object.Slot(0x32, reinterpret_cast<void*>(&FormEditorIDImpl));
            auto* form = object.As<RE::TESForm>();
            form->formType = RE::FormType::Keyword;
            form->formID = 0x0F000000u + static_cast<std::uint32_t>(pool.objects.size());
            EditorIDsByForm()[static_cast<const void*>(form)] = key;
            it = pool.byEditorID.emplace(key, object.As<RE::BGSKeyword>()).first;
        }

        // Both re-registered on every call, because the editor-ID tables are
        // emptied per EngineMock while the keyword itself is not.
        EditorIDsByForm()[static_cast<const void*>(it->second)] = key;
        EditorIDTable().insert({RE::BSFixedString(key.c_str()), reinterpret_cast<RE::TESForm*>(it->second)});
        return it->second;
    }

    // Everyone's race unless a test names another. Made once and kept, so
    // every NPC in a process shares one — which is also true in the game.
    RE::TESRace* EngineMock::DefaultPeopleRace()
    {
        static RE::TESRace* race = nullptr;
        if (!race)
            race = AddRace(0x00013746u, "NordRace", {"ActorTypeNPC"});
        return race;
    }

    RE::TESRace* EngineMock::AddRace(std::uint32_t formID,
                                     std::string editorID,
                                     std::vector<std::string> keywordEditorIDs)
    {
        auto& pool = SimpleFormObjects();
        auto& object = pool.emplace_back(sizeof(RE::TESRace) + 0x100, 256);
        WireFormDefaults(object, false);
        WireKeywordForm<RE::TESRace>(object);
        object.Slot(0x32, reinterpret_cast<void*>(&FormEditorIDImpl));
        auto* race = object.As<RE::TESRace>();
        race->formType = RE::FormType::Race;
        race->formID = formID;

        static auto* keywordArrays = new std::deque<std::vector<RE::BGSKeyword*>>();
        auto& keywords = keywordArrays->emplace_back();
        for (const auto& edid : keywordEditorIDs)
            keywords.push_back(AddKeyword(edid));
        race->keywords = keywords.empty() ? nullptr : keywords.data();
        race->numKeywords = static_cast<std::uint32_t>(keywords.size());

        FormTable().insert({formID, object.As<RE::TESForm>()});
        FormsOfType(RE::FormType::Race).push_back(object.As<RE::TESForm>());
        if (!editorID.empty()) {
            SetEditorID(race, editorID);
            EditorIDTable().insert({RE::BSFixedString(editorID.c_str()), object.As<RE::TESForm>()});
        }
        return race;
    }

    RE::BGSLocation* EngineMock::AddLocation(std::uint32_t formID,
                                             std::string name,
                                             std::vector<std::string> keywordEditorIDs,
                                             std::string editorID)
    {
        auto& pool = Locations();
        auto& object = pool.objects.emplace_back(sizeof(RE::BGSLocation), 256);
        WireFormDefaults(object, false);
        WireFullName<RE::BGSLocation>(object);
        WireKeywordForm<RE::BGSLocation>(object);
        // GetFormEditorID is slot 0x32 on TESForm.
        object.Slot(0x32, reinterpret_cast<void*>(&FormEditorIDImpl));

        auto* location = object.As<RE::BGSLocation>();
        location->formType = RE::FormType::Location;
        location->formID = formID;
        // A non-empty display name keeps the module's own label helper off the
        // editor-ID path, which needs a dependency the harness does not model.
        location->fullName = name.c_str();
        location->parentLoc = nullptr;

        auto& keywords = pool.keywordArrays.emplace_back();
        for (const auto& edid : keywordEditorIDs)
            keywords.push_back(AddKeyword(edid));
        location->keywords = keywords.empty() ? nullptr : keywords.data();
        location->numKeywords = static_cast<std::uint32_t>(keywords.size());

        FormTable().insert({formID, object.As<RE::TESForm>()});
        FormsOfType(RE::FormType::Location).push_back(object.As<RE::TESForm>());
        if (!editorID.empty()) {
            SetEditorID(location, editorID);
            EditorIDTable().insert({RE::BSFixedString(editorID.c_str()), object.As<RE::TESForm>()});
        }
        return location;
    }

    void EngineMock::AddResident(RE::BGSLocation* location, RE::TESNPC* npc, RE::BGSLocation* editorLocation)
    {
        if (!location || !npc)
            return;
        // The array is a real container in zeroed storage, so it is built in
        // place the first time a location is given anybody.
        static auto* built = new std::set<const void*>();
        if (built->insert(static_cast<const void*>(location)).second)
            new (&location->uniqueNPCs) RE::BSTArray<RE::UniqueNPCData>();

        RE::UniqueNPCData row{};
        // The row's `actor` field holds the BASE form despite its type — which
        // is how the engine's own records are laid out, and what everything
        // reading LCUN expects.
        row.actor = reinterpret_cast<RE::Actor*>(npc);
        // And its `refID` names the placed reference, which is the id anything
        // outside the engine — SkyrimNet especially — speaks. The reference is
        // fabricated here rather than left as a bare id, because code that
        // asks whether somebody is alive resolves it and reads their state: a
        // row naming a reference nothing can find reads as a person who has
        // died, and a world of those is a world where nobody talks.
        row.refID = AddActor(npc->GetFormID() + kPlacedRefOffset)->GetFormID();
        row.editorLoc = editorLocation;
        location->uniqueNPCs.push_back(row);
    }

    RE::Actor* EngineMock::PlacedActorFor(std::uint32_t npcFormID)
    {
        return RE::TESForm::LookupByID<RE::Actor>(npcFormID + kPlacedRefOffset);
    }

    void EngineMock::JoinFaction(RE::TESNPC* npc, RE::TESFaction* faction, std::int8_t rank)
    {
        if (!npc || !faction)
            return;
        RE::FACTION_RANK row{};
        row.faction = faction;
        row.rank = rank;
        npc->factions.push_back(row);
    }

    void EngineMock::SetLocationParent(RE::BGSLocation* child, RE::BGSLocation* parent)
    {
        if (child)
            child->parentLoc = parent;
    }
} // namespace NarrativeEngine::Testing

// ---------------------------------------------------------------------------
// Editor IDs and the scripted scene
// ---------------------------------------------------------------------------

namespace NarrativeEngine::Testing
{
    void SetEditorID(const void* form, const std::string& editorID)
    {
        EditorIDsByForm()[form] = editorID;
    }

    namespace
    {
        std::map<const void*, std::string>& FormNames()
        {
            static auto* table = new std::map<const void*, std::string>();
            return *table;
        }
    } // namespace

    const char* FormName(const void* form)
    {
        const auto& table = FormNames();
        const auto it = table.find(form);
        return it == table.end() ? nullptr : it->second.c_str();
    }

    void SetFormName(const void* form, std::string name)
    {
        FormNames()[form] = std::move(name);
    }

    namespace
    {
        FakeObject& FakeScene()
        {
            static FakeObject scene{sizeof(RE::BGSScene), 128};
            return scene;
        }
    } // namespace
} // namespace NarrativeEngine::Testing

namespace NarrativeEngine::Testing
{
    namespace
    {
        void Update3DPositionImpl(void*, bool)
        {
            if (auto* mock = EngineMock::Current())
                ++mock->placement.warpUpdates;
        }

        RE::BGSScene* GetCurrentSceneImpl(void*)
        {
            auto* mock = EngineMock::Current();
            if (!mock || !mock->world.playerInScene)
                return nullptr;

            auto& object = FakeScene();
            WireFormDefaults(object, false);
            auto* scene = object.As<RE::BGSScene>();
            scene->formType = RE::FormType::Scene;
            scene->formID = 0x0004E5C1u;
            scene->isPlaying = mock->world.sceneIsPlaying;
            // The quest that authored the scene. Whether it is one of ours is
            // what decides self-exclusion for anything reading the scene.
            scene->parentQuest = mock->AddQuest(mock->aliases.sceneQuest);
            return scene;
        }
    } // namespace
} // namespace NarrativeEngine::Testing
