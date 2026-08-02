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

; The three functions below exist only because the underlying engine call has
; no CommonLibSSE-NG binding. Everything else this beat needs is native C++.

; ReferenceAlias.ForceRefTo has no native binding.
;
; Takes a FormID rather than an ObjectReference because a reference
; passed from C++ arrives non-None but unpacks to null inside ForceRefTo.
; Game.GetFormEx (SKSE) rather than Game.GetForm: dynamically-created
; references live at 0xFF...... and need the full 32-bit range.
Function FillAttackerSlot(int aiIndex, int aiFormID)
    ReferenceAlias slot = GetAttackerSlot(aiIndex)
    if slot == None
        Debug.Trace("[_ne_AmbushQuest] FillAttackerSlot: bad index " + aiIndex)
        return
    endif
    ObjectReference akRef = Game.GetFormEx(aiFormID) as ObjectReference
    if akRef == None
        Debug.Trace("[_ne_AmbushQuest] FillAttackerSlot: slot " + aiIndex + " form " + aiFormID + " did not resolve to an ObjectReference")
        return
    endif
    slot.ForceRefTo(akRef)
    Debug.Trace("[_ne_AmbushQuest] FillAttackerSlot: slot " + aiIndex + " forced to " + akRef)
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

; SkyrimNetApi.RegisterPersistentEvent has no native binding.
;
; Records the encounter in SkyrimNet's event log without prompting
; anyone to speak — same shape as _ne_VisitQuest's silent scene event.
; Attributed to the lead attacker so the memory system associates it
; with whoever actually jumped the player.
Function RunAmbushNarration(String content)
    Actor originator = Attacker01.GetActorReference()
    Actor player = PlayerRef.GetActorReference()
    int result = SkyrimNetApi.RegisterPersistentEvent(content, originator, player)
    Debug.Trace("[_ne_AmbushQuest] RunAmbushNarration: RegisterPersistentEvent returned " + result)
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
