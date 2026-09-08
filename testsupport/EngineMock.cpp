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
#include <new>
#include <unordered_map>
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
    RE::TESObjectCELL* FabricatedCell();

    namespace
    {
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

        void WireFormDefaults(FakeObject& form, bool isReference)
        {
            void* asReference = isReference ? reinterpret_cast<void*>(&FormAsReferenceSelf)
                                            : reinterpret_cast<void*>(&FormAsReferenceNull);
            form.Slot(0x2B, asReference);
            form.Slot(0x2C, asReference);
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
    return mock != nullptr && mock->ui.gameIsPaused;
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
    return NarrativeEngine::Testing::OpaqueSingleton<RE::TES>();
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
        static RE::BSTEventSource<SKSE::ModCallbackEvent> source;
        return source;
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

        auto& object = registry.objects.emplace_back(sizeof(RE::Actor), 256);
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

    RE::TESFaction* EngineMock::AddFaction(std::uint32_t formID)
    {
        auto& object = FactionObjects().emplace_back(sizeof(RE::TESFaction), 256);
        WireFormDefaults(object, false);
        auto* form = object.As<RE::TESForm>();
        form->formType = RE::FormType::Faction;
        form->formID = formID;
        FormTable().insert({formID, form});
        return object.As<RE::TESFaction>();
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
    auto* mock = EngineMock::Current();
    if (!mock || !mock->courier.aliasHasReference)
        return nullptr;
    return NarrativeEngine::Testing::OpaqueSingleton<RE::TESObjectREFR>();
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
            static FakeObject cell{sizeof(RE::TESObjectCELL), 128};
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
    auto* mock = NarrativeEngine::Testing::EngineMock::Current();
    return mock != nullptr && mock->world.cellIsInterior;
}

// TESForm::GetName is out-of-line, and the ModEvent sink reads it off an
// event's sender to say who sent it. Answers out of the mock so a test can put
// a name on the sender without fabricating a whole form.

const char* RE::TESForm::GetName() const
{
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
        if (!editorID.empty()) {
            SetEditorID(location, editorID);
            EditorIDTable().insert({RE::BSFixedString(editorID.c_str()), object.As<RE::TESForm>()});
        }
        return location;
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
            return scene;
        }
    } // namespace
} // namespace NarrativeEngine::Testing
