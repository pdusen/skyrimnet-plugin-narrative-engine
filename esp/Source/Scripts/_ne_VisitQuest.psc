Scriptname _ne_VisitQuest extends Quest

; Sender alias — Optional with no fill rule, force-filled from C++ once
; EnsureQuestStarted has returned. Carries both AI packages (Follow when
; GetStage < 50, Return Travel when GetStage >= 50); the engine's package
; selector swaps between them as C++ advances the stage.
ReferenceAlias Property Sender       Auto

; ReturnAnchor alias — Optional with no fill rule, force-filled from C++
; with an XMarker placed at the sender's own position before they are
; warped anywhere. The Return Travel package's Destination is
; `Alias:ReturnAnchor`, so the sender walks toward this marker during
; ReturnHome, and Shutdown() deletes it.
ReferenceAlias Property ReturnAnchor Auto

; -----------------------------------------------------------------
; Alias force-fill trampolines
; -----------------------------------------------------------------
;
; ReferenceAlias.ForceRefTo has no CommonLibSSE-NG binding —
; RE::BGSRefAlias exposes only GetReference() and GetActorReference() —
; so both fills come back through Papyrus.
;
; Each takes a FormID rather than an ObjectReference: a reference passed
; from C++ arrives non-None but unpacks to null inside ForceRefTo. See
; docs/engine-findings/passing-references-to-papyrus-from-cpp.md.
;
; Game.GetFormEx (SKSE) rather than Game.GetForm, because the return
; anchor is a dynamically-created reference at 0xFF...... and needs the
; full 32-bit range.

Function FillSenderSlot(int aiFormID)
    ObjectReference akRef = Game.GetFormEx(aiFormID) as ObjectReference
    if akRef == None
        Debug.Trace("[_ne_VisitQuest] FillSenderSlot: form " + aiFormID + " did not resolve to an ObjectReference")
        return
    endIf
    Sender.ForceRefTo(akRef)
    Debug.Trace("[_ne_VisitQuest] FillSenderSlot: Sender forced to " + akRef)
EndFunction

Function FillReturnAnchorSlot(int aiFormID)
    ObjectReference akRef = Game.GetFormEx(aiFormID) as ObjectReference
    if akRef == None
        Debug.Trace("[_ne_VisitQuest] FillReturnAnchorSlot: form " + aiFormID + " did not resolve to an ObjectReference")
        return
    endIf
    ReturnAnchor.ForceRefTo(akRef)
    Debug.Trace("[_ne_VisitQuest] FillReturnAnchorSlot: ReturnAnchor forced to " + akRef)
EndFunction

Function StartReturnTravel()
    Actor senderActor = Sender.GetActorReference()
    if senderActor == None
        Debug.Trace("[_ne_VisitQuest] StartReturnTravel: Sender empty")
        return
    endIf

    senderActor.EvaluatePackage()
EndFunction

; -----------------------------------------------------------------
; Sender-action trampoline
; -----------------------------------------------------------------
;
; C++ VM-dispatches into this each time the state machine wants the
; sender to run a SkyrimNet action. Handles both plugin-owned turns
; (Salutation / ReEngage / Valediction — called with our custom
; conversation action name and a briefing argsJson) and the built-in
; ContinueConversation nudge (called with actionName =
; "ContinueConversation" and empty argsJson).
;
; Isolating the ExecuteAction call in Papyrus keeps the SkyrimNet API
; surface fully typed by its author. If SkyrimNet's action design
; changes, this function is the only place that has to change.
Function RunSenderAction(String actionName, String argsJson)
    Debug.Trace("[_ne_VisitQuest] RunSenderAction: entered (action='" + actionName + "', argsJson.len=" + StringUtil.GetLength(argsJson) + ")")
    Actor senderActor = Sender.GetActorReference()
    if senderActor == None
        Debug.Trace("[_ne_VisitQuest] RunSenderAction: Sender empty")
        return
    endIf
    Debug.Trace("[_ne_VisitQuest] RunSenderAction: calling SkyrimNetApi.ExecuteAction on sender " + senderActor)
    int result = SkyrimNetApi.ExecuteAction(actionName, senderActor, argsJson)
    Debug.Trace("[_ne_VisitQuest] RunSenderAction: ExecuteAction returned " + result)
EndFunction

; -----------------------------------------------------------------
; Sender-narration trampoline
; -----------------------------------------------------------------
;
; Third-person scene narration used to trigger a sender turn. Fires
; SkyrimNet's DirectNarration API which feeds `content` to the
; downstream dialogue LLM as scene context; the LLM then produces a
; spoken line in the sender's voice, addressed to the player.
;
; Used for Salutation / ReEngage / Valediction turns — SkyrimNet's
; built-in ContinueConversation action stays on the RunSenderAction
; path (which uses ExecuteAction) for the ignore-irritation nudges.
Function RunSenderNarration(String content)
    Debug.Trace("[_ne_VisitQuest] RunSenderNarration: entered (content.len=" + StringUtil.GetLength(content) + ")")
    Actor senderActor = Sender.GetActorReference()
    if senderActor == None
        Debug.Trace("[_ne_VisitQuest] RunSenderNarration: Sender empty")
        return
    endIf
    int result = SkyrimNetApi.DirectNarration(content, senderActor, Game.GetPlayer())
    Debug.Trace("[_ne_VisitQuest] RunSenderNarration: DirectNarration returned " + result)
EndFunction

; -----------------------------------------------------------------
; Silent sender-scene trampoline
; -----------------------------------------------------------------
;
; Same third-person scene narration as RunSenderNarration, but fires
; SkyrimNet's RegisterPersistentEvent API instead of DirectNarration.
; The event still lands in SkyrimNet's memory / event history the
; same way — it just does NOT prompt the sender to speak a response.
;
; Used at Valediction time when the LLM observed that the sender
; already said their closing/goodbye during the natural exchange, so
; a fresh spoken closing line would double up. The scene beat is
; still recorded so the memory-formation and event-log downstream
; systems know the visit ended.
Function RunSenderSilentSceneEvent(String content)
    Debug.Trace("[_ne_VisitQuest] RunSenderSilentSceneEvent: entered (content.len=" + StringUtil.GetLength(content) + ")")
    Actor senderActor = Sender.GetActorReference()
    if senderActor == None
        Debug.Trace("[_ne_VisitQuest] RunSenderSilentSceneEvent: Sender empty")
        return
    endIf
    int result = SkyrimNetApi.RegisterPersistentEvent(content, senderActor, Game.GetPlayer())
    Debug.Trace("[_ne_VisitQuest] RunSenderSilentSceneEvent: RegisterPersistentEvent returned " + result)
EndFunction

; -----------------------------------------------------------------
; Shutdown — terminal teardown
; -----------------------------------------------------------------
;
; Called by the Stage 200 fragment. Stop() halts the quest; Reset()
; clears the alias fills so both packages on the Sender alias release
; and the actor's package stack falls back to their normal AI.
Function Shutdown()
    Stop()

    ObjectReference returnAnchorRef = ReturnAnchor.GetReference()
    if returnAnchorRef != None
        returnAnchorRef.DisableNoWait()
        returnAnchorRef.Delete()
    endIf

    Reset()
EndFunction
