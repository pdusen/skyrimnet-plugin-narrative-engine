#include "EngineMock.h"

#include "FakeVTable.h"

#include <RE/Skyrim.h>
#include <REL/Module.h>
#include <SKSE/Interfaces.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

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

        g_installed = this;
    }

    EngineMock::~EngineMock()
    {
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

RE::PlayerCharacter* RE::PlayerCharacter::GetSingleton()
{
    auto* mock = EngineMock::Current();
    if (!mock || !mock->player.present)
        return nullptr;
    return NarrativeEngine::Testing::OpaqueSingleton<RE::PlayerCharacter>();
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
