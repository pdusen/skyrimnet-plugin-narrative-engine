Scriptname _ne_BanditAmbushQuest extends Quest  

ReferenceAlias Property Bandit01Ref  Auto  

ReferenceAlias Property Bandit02Ref  Auto  

ReferenceAlias Property Bandit03Ref  Auto  

ReferenceAlias Property Bandit04Ref  Auto  

ReferenceAlias Property Bandit05Ref  Auto  

ReferenceAlias Property Bandit06Ref  Auto

Function CheckAllBanditsDead()
    if GetStage() >= 200
        return
    endif
    int aliveCount = 0
    if !IsAliasDeadOrEmpty(Bandit01Ref)
        aliveCount += 1
    endif
    if !IsAliasDeadOrEmpty(Bandit02Ref)
        aliveCount += 1
    endif
    if !IsAliasDeadOrEmpty(Bandit03Ref)
        aliveCount += 1
    endif
    if !IsAliasDeadOrEmpty(Bandit04Ref)
        aliveCount += 1
    endif
    if !IsAliasDeadOrEmpty(Bandit05Ref)
        aliveCount += 1
    endif
    if !IsAliasDeadOrEmpty(Bandit06Ref)
        aliveCount += 1
    endif
    Debug.Trace("[NE_Ambush] CheckAllBanditsDead aliveCount=" + aliveCount)
    if aliveCount == 0
        Debug.Trace("[NE_Ambush] Completing quest")
        SetStage(200)
        CompleteQuest()
        ; Notify the C++ ActionDispatcher that the ambush has resolved so it
        ; clears in-flight state and starts the global action cooldown.
        ; strArg must match the action's IAction::Name() ("ambush").
        int handle = ModEvent.Create("_ne_ActionCompleted")
        if handle
            ModEvent.PushString(handle, "ambush")
            ModEvent.PushFloat(handle, 0.0)
            ModEvent.Send(handle)
        endif
        Stop()
    endif
EndFunction

bool Function IsAliasDeadOrEmpty(ReferenceAlias akAlias)
    if akAlias == None
        return true
    endif
    Actor a = akAlias.GetActorReference()
    if a == None
        return true
    endif
    return a.IsDead()
EndFunction
