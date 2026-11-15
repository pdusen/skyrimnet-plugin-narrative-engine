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
// quest-scoped calls. Fire-and-forget: none of them waits for a return
// value; VMDispatchOnQuestWithCallback hands the VM a functor it calls
// once the call has run, on a VM thread. Any beat that VM-dispatches a
// Papyrus member function against a TESQuest reuses this module rather
// than reimplementing the handle-policy + FunctionArguments boilerplate.
namespace NarrativeEngine::QuestUtils
{
    // Queue a Papyrus member-function call on `quest` for later VM
    // execution. Each call is a stack of its own: calls are queued in
    // order, but one that waits on the frame -- ForceRefTo, Stop, SetStage
    // -- lets calls queued after it run before it finishes, so nothing
    // promises they complete in the order they were sent.
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
    inline bool VMDispatchOnQuestWithCallback(RE::TESQuest* quest,
                                              std::string_view scriptName,
                                              std::string_view methodName,
                                              RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback,
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
        return vm->DispatchMethodCall(
            handle, RE::BSFixedString(scriptName.data()), RE::BSFixedString(methodName.data()), fnArgs, callback);
    }

    template <typename... Args>
    inline bool VMDispatchOnQuest(RE::TESQuest* quest,
                                  std::string_view scriptName,
                                  std::string_view methodName,
                                  Args... args)
    {
        return VMDispatchOnQuestWithCallback(quest, scriptName, methodName, {}, std::move(args)...);
    }

    // Convenience: VM-dispatch Quest.SetStage(int) on `quest`. Returns
    // whatever VMDispatchOnQuest returns (queued vs. not queued).
    bool VMDispatchQuestSetStage(RE::TESQuest* quest, std::uint32_t stage);
} // namespace NarrativeEngine::QuestUtils
