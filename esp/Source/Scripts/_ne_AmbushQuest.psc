Scriptname _ne_AmbushQuest extends Quest

ReferenceAlias Property Attacker01  Auto

ReferenceAlias Property Attacker02  Auto

ReferenceAlias Property Attacker03  Auto

ReferenceAlias Property Attacker04  Auto

ReferenceAlias Property Attacker05  Auto

ReferenceAlias Property Attacker06  Auto

ReferenceAlias Property Attacker07  Auto

ReferenceAlias Property Attacker08  Auto

ReferenceAlias Property PlayerRef  Auto

; Both functions below exist only because the underlying engine call has no
; CommonLibSSE-NG binding. Everything else this beat needs is native C++.

; ReferenceAlias.ForceRefTo has no native binding.
Function FillAttackerSlot(int aiIndex, ObjectReference akRef)
    ReferenceAlias slot = GetAttackerSlot(aiIndex)
    if slot == None || akRef == None
        Debug.Trace("[_ne_AmbushQuest] FillAttackerSlot: bad index " + aiIndex)
        return
    endif
    slot.ForceRefTo(akRef)
EndFunction

; Actor.StartCombat has no native binding.
Function EngageAttacker(int aiIndex)
    ReferenceAlias slot = GetAttackerSlot(aiIndex)
    if slot == None
        return
    endif
    Actor attacker = slot.GetActorReference()
    Actor player = PlayerRef.GetActorReference()
    if attacker == None || player == None
        return
    endif
    attacker.StartCombat(player)
EndFunction

ReferenceAlias Function GetAttackerSlot(int aiIndex)
    if aiIndex == 0
        return Attacker01
    elseif aiIndex == 1
        return Attacker02
    elseif aiIndex == 2
        return Attacker03
    elseif aiIndex == 3
        return Attacker04
    elseif aiIndex == 4
        return Attacker05
    elseif aiIndex == 5
        return Attacker06
    elseif aiIndex == 6
        return Attacker07
    elseif aiIndex == 7
        return Attacker08
    endif
    return None
EndFunction
