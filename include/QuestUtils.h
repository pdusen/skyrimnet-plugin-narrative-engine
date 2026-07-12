#pragma once

#include <cstdint>
#include <string_view>
#include <utility>

#include <RE/B/BSFixedString.h>
#include <RE/B/BSTSmartPointer.h>
#include <RE/F/FunctionArguments.h>
#include <RE/I/IObjectHandlePolicy.h>
#include <RE/I/IStackCallbackFunctor.h>
#include <RE/T/TESQuest.h>
#include <RE/V/VirtualMachine.h>

// QuestUtils — thin wrappers around the Papyrus VM dispatch surface for
// quest-scoped calls. Fire-and-forget: neither helper waits for a return
// value or exposes the callback slot. Any beat that VM-dispatches a
// Papyrus member function against a TESQuest reuses this module rather
// than reimplementing the handle-policy + FunctionArguments boilerplate.
namespace NarrativeEngine::QuestUtils
{
    // Queue a Papyrus member-function call on `quest` for later VM
    // execution. FIFO ordering: sequential dispatches against the same
    // quest handle run in the order they were queued.
    //
    // Returns false when the quest / VM / handle-policy isn't available
    // or DispatchMethodCall itself refused. True means the call was
    // successfully queued (NOT that it succeeded — the VM may still fail
    // it later; the observable effect is via engine state, not a return
    // value here).
    //
    // Args are taken by value so the deduced Args... are decayed types.
    // CommonLibSSE-NG's MakeFunctionArguments -> FunctionArguments goes
    // through an `is_parameter_convertible` SFINAE check that requires
    // `is_not_reference<T>`; passing lvalue-reference-deduced pointer
    // args through forwarding references would fail that check.
    template <typename... Args>
    inline bool VMDispatchOnQuest(RE::TESQuest* quest,
                                  std::string_view scriptName,
                                  std::string_view methodName,
                                  Args... args)
    {
        if (!quest)
            return false;
        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!vm)
            return false;
        auto* policy = vm->GetObjectHandlePolicy();
        if (!policy)
            return false;
        const auto handle = policy->GetHandleForObject(RE::TESQuest::FORMTYPE, quest);
        auto* fnArgs = RE::MakeFunctionArguments(std::move(args)...);
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
        return vm->DispatchMethodCall(
            handle, RE::BSFixedString(scriptName.data()), RE::BSFixedString(methodName.data()), fnArgs, callback);
    }

    // Convenience: VM-dispatch Quest.SetStage(int) on `quest`. Returns
    // whatever VMDispatchOnQuest returns (queued vs. not queued).
    bool VMDispatchQuestSetStage(RE::TESQuest* quest, std::uint32_t stage);
} // namespace NarrativeEngine::QuestUtils
