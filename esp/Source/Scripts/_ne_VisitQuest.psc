Scriptname _ne_VisitQuest extends Quest

; Sender alias — filled at EnsureQuestStarted via Find Matching Reference
; on `GetFactionRank _ne_VisitSenderFaction >= 4`. Carries both AI packages
; (Follow when GetStage < 50, Return Travel when GetStage >= 50); the
; engine's package selector swaps between them as C++ advances the stage.
ReferenceAlias Property Sender       Auto

; SpawnMarker alias — filled at EnsureQuestStarted via Find Matching
; Reference on nearest XMarkerHeading within the distance / line-of-sight
; bounds relative to PlayerRef. Used by the Stage 10 fragment to warp the
; sender to an out-of-sight starting point near the player.
ReferenceAlias Property SpawnMarker  Auto

; ReturnAnchor alias — Fill Type Specific Reference with the reference
; left blank; runtime-filled by SetReturnAnchor() at Start time pointing
; at the temp XMarker C++ placed at the sender's pre-dispatch position.
; The Return Travel package's Destination is `Alias:ReturnAnchor`, so
; the sender walks toward this marker during ReturnHome.
ReferenceAlias Property ReturnAnchor Auto

Function MoveSenderToSpawnMarker()
    Actor senderActor = Sender.GetActorReference()
    if senderActor == None
        Debug.Trace("[_ne_VisitQuest] MoveSenderToSpawnMarker: Sender empty")
        return
    endIf

    ObjectReference spawnMarkerRef = SpawnMarker.GetReference()
    if spawnMarkerRef == None
        Debug.Trace("[_ne_VisitQuest] MoveSenderToSpawnMarker: SpawnMarker empty")
        return
    endIf

    senderActor.MoveTo(spawnMarkerRef)
    senderActor.EvaluatePackage()
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
; Function RunSenderAction(String actionName, String argsJson)
;     Actor senderActor = Sender.GetActorReference()
;     if senderActor == None
;         Debug.Trace("[_ne_VisitQuest] RunSenderAction: Sender empty")
;         return
;     endIf
;     SkyrimNetApi.ExecuteAction(actionName, senderActor, argsJson)
; EndFunction

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
