#include "EngineMock.h"

#include "FakeVTable.h"
#include "RelocationMocks.h"

#include <RE/Skyrim.h>
#include <REL/Module.h>
#include <SKSE/Interfaces.h>

#include <algorithm>
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
    void ClearActorRegistry();

    namespace
    {
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

        void WireFormDefaults(FakeObject& form, bool isReference)
        {
            void* asReference = isReference ? reinterpret_cast<void*>(&FormAsReferenceSelf)
                                            : reinterpret_cast<void*>(&FormAsReferenceNull);
            form.Slot(0x2B, asReference);
            form.Slot(0x2C, asReference);
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

        g_installed = this;
    }

    EngineMock::~EngineMock()
    {
        FormTable().clear();
        EditorIDTable().clear();
        ClearActorRegistry();
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
    } // namespace
} // namespace NarrativeEngine::Testing

RE::BGSLocation* RE::TESObjectREFR::GetCurrentLocation() const
{
    using namespace NarrativeEngine::Testing;
    auto* mock = EngineMock::Current();
    if (!mock || !mock->world.playerHasLocation)
        return nullptr;

    auto& object = FakeLocation();
    WireFormDefaults(object, false);
    WireFullName<RE::BGSLocation>(object);
    auto* location = object.As<RE::BGSLocation>();
    location->formID = mock->world.locationFormID;
    location->formType = RE::FormType::Location;
    location->fullName = mock->world.locationName.c_str();
    return location;
}

bool RE::TESObjectCELL::IsInteriorCell() const
{
    auto* mock = NarrativeEngine::Testing::EngineMock::Current();
    return mock != nullptr && mock->world.cellIsInterior;
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
            auto* cell = cellObject.As<RE::TESObjectCELL>();
            cell->formID = mock->world.cellFormID;
            cell->formType = RE::FormType::Cell;
            cell->fullName = mock->world.cellName.c_str();
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
