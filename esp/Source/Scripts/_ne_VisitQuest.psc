Scriptname _ne_VisitQuest extends Quest

; Sender alias — Optional with no fill rule, force-filled from C++ once
; EnsureQuestStarted has returned. Carries both AI packages (Follow at
; stages 10-49, Return Travel from 50), each conditioned on the quest
; running as well; the engine's package selector swaps between them as
; C++ advances the stage.
ReferenceAlias Property Sender       Auto

; ReturnAnchor alias — Optional with no fill rule, force-filled from C++
; with an XMarker placed at the sender's own position before they are
; warped anywhere. The Return Travel package's Destination is
; `Alias:ReturnAnchor`, so the sender walks toward this marker during
; ReturnHome. C++ deletes it on teardown, and Shutdown() too if the alias
; still holds it.
ReferenceAlias Property ReturnAnchor Auto

; -----------------------------------------------------------------
; Visit ids
; -----------------------------------------------------------------
;
; Every call that fills or empties the aliases carries the id of the
; visit it is for. C++ hands them out in increasing order and keeps the
; counter in its co-save, which is saved alongside this quest, so an id
; is never reused against this script's record of the ones it has seen.
;
; Separate calls into this script are separate stacks, and the VM runs
; them in no promised order. Without the ids, a fill queued just before
; a visit was torn down could land after the teardown, on a quest that
; was already stopped -- and stopping it again never undoes that, so
; the actor kept the alias and the Follow package for good.

; The highest visit id this script has been told is over. A fill for a
; visit at or below it is refused.
int LastClosedVisitId = 0

; The visit whose fills the aliases hold. 0 for a fill made before visits
; carried ids.
int HeldVisitId = 0

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
;
; ForceRefTo waits on the frame, and other calls into this script run
; while it does. An EndVisit that ran in that gap has already emptied
; the alias and will not come back for this fill, so each fill checks
; again once it lands and takes itself back out.

Function FillSenderSlot(int aiVisitId, int aiFormID)
    if aiVisitId <= LastClosedVisitId
        Debug.Trace("[_ne_VisitQuest] FillSenderSlot: visit " + aiVisitId + " has already ended; not filling")
        return
    endIf
    ObjectReference akRef = Game.GetFormEx(aiFormID) as ObjectReference
    if akRef == None
        Debug.Trace("[_ne_VisitQuest] FillSenderSlot: form " + aiFormID + " did not resolve to an ObjectReference")
        return
    endIf
    HeldVisitId = aiVisitId
    Sender.ForceRefTo(akRef)
    if aiVisitId <= LastClosedVisitId
        Sender.Clear()
        Debug.Trace("[_ne_VisitQuest] FillSenderSlot: visit " + aiVisitId + " ended while Sender was being filled; cleared")
        return
    endIf
    Debug.Trace("[_ne_VisitQuest] FillSenderSlot: Sender forced to " + akRef + " for visit " + aiVisitId)
EndFunction

Function FillReturnAnchorSlot(int aiVisitId, int aiFormID)
    if aiVisitId <= LastClosedVisitId
        Debug.Trace("[_ne_VisitQuest] FillReturnAnchorSlot: visit " + aiVisitId + " has already ended; not filling")
        return
    endIf
    ObjectReference akRef = Game.GetFormEx(aiFormID) as ObjectReference
    if akRef == None
        Debug.Trace("[_ne_VisitQuest] FillReturnAnchorSlot: form " + aiFormID + " did not resolve to an ObjectReference")
        return
    endIf
    HeldVisitId = aiVisitId
    ReturnAnchor.ForceRefTo(akRef)
    if aiVisitId <= LastClosedVisitId
        ReturnAnchor.Clear()
        Debug.Trace("[_ne_VisitQuest] FillReturnAnchorSlot: visit " + aiVisitId + " ended while ReturnAnchor was being filled; cleared")
        return
    endIf
    Debug.Trace("[_ne_VisitQuest] FillReturnAnchorSlot: ReturnAnchor forced to " + akRef + " for visit " + aiVisitId)
EndFunction

; -----------------------------------------------------------------
; EndVisit -- the visit is over
; -----------------------------------------------------------------
;
; Dispatched by C++ on every teardown, ahead of the terminal stage.
; Records the visit as closed, so no fill for it can land afterwards,
; and empties the aliases if what they hold is this visit's or older.
; C++ deletes the anchor itself, by FormID, because the alias may never
; have been filled with it.
Function EndVisit(int aiVisitId)
    if aiVisitId > LastClosedVisitId
        LastClosedVisitId = aiVisitId
    endIf
    if HeldVisitId <= aiVisitId
        Actor senderActor = Sender.GetActorReference()
        Sender.Clear()
        ReturnAnchor.Clear()
        if senderActor != None
            senderActor.EvaluatePackage()
        endIf
    endIf
    Debug.Trace("[_ne_VisitQuest] EndVisit: visit " + aiVisitId + " closed (held=" + HeldVisitId + ")")
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
; Walk-out trampoline
; -----------------------------------------------------------------
;
; Records, silently, that the player left in the middle of the
; conversation: the player is the originator and the sender the
; target, so the walk-out lands in the sender's history. Used when the
; player fast-travels away mid-visit.
;
; Takes the sender's FormID rather than reading the Sender alias,
; because C++ dispatches the Stage 200 shutdown straight after this
; call and Reset() empties the alias.
Function RecordPlayerLeftAbruptly(int aiSenderFormID, String content)
    Actor senderActor = Game.GetFormEx(aiSenderFormID) as Actor
    if senderActor == None
        Debug.Trace("[_ne_VisitQuest] RecordPlayerLeftAbruptly: form " + aiSenderFormID + " is not an Actor")
        return
    endIf
    int result = SkyrimNetApi.RegisterPersistentEvent(content, Game.GetPlayer(), senderActor)
    Debug.Trace("[_ne_VisitQuest] RecordPlayerLeftAbruptly: RegisterPersistentEvent returned " + result)
EndFunction

; -----------------------------------------------------------------
; Shutdown — terminal teardown
; -----------------------------------------------------------------
;
; Called by the Stage 200 fragment. Both aliases are cleared explicitly
; before Stop() rather than left to it: Stop() empties them only on a
; quest that is running, and a fill can land on one that is not. Reset()
; then puts the stage back to 0.
Function Shutdown()
    if HeldVisitId > LastClosedVisitId
        LastClosedVisitId = HeldVisitId
    endIf

    ObjectReference returnAnchorRef = ReturnAnchor.GetReference()
    Sender.Clear()
    ReturnAnchor.Clear()

    Stop()

    if returnAnchorRef != None
        returnAnchorRef.DisableNoWait()
        returnAnchorRef.Delete()
    endIf

    Reset()
EndFunction

; -----------------------------------------------------------------
; Orphan repair
; -----------------------------------------------------------------
;
; Called from C++ when the aliases hold somebody no visit is running
; for. A fill that lands after Shutdown has stopped the quest is not
; undone by stopping it again -- the quest is already stopped -- so
; the actor keeps the alias and both its packages. Clear() is what
; removes them.
;
; aiSparedVisitId is the visit that is about to start, when there is
; one, and fills held for it are left alone: the repair is queued ahead
; of them, but nothing promises it runs first. With no visit to spare,
; the quest is stopped and reset as well when it reads as still going,
; which is what a visit nobody is driving any more looks like.
Function RepairOrphanedFills(int aiSparedVisitId)
    Actor senderActor = Sender.GetActorReference()
    ObjectReference returnAnchorRef = ReturnAnchor.GetReference()
    Debug.Trace("[_ne_VisitQuest] RepairOrphanedFills: Sender=" + senderActor + " ReturnAnchor=" + returnAnchorRef + " held=" + HeldVisitId + " spared=" + aiSparedVisitId + " stage=" + GetStage())

    if aiSparedVisitId != 0 && HeldVisitId == aiSparedVisitId
        return
    endIf

    if HeldVisitId > LastClosedVisitId
        LastClosedVisitId = HeldVisitId
    endIf

    Sender.Clear()
    ReturnAnchor.Clear()

    if returnAnchorRef != None
        returnAnchorRef.DisableNoWait()
        returnAnchorRef.Delete()
    endIf

    if aiSparedVisitId == 0 && (IsRunning() || GetStage() != 0)
        Stop()
        Reset()
    endIf

    if senderActor != None
        senderActor.EvaluatePackage()
    endIf
EndFunction
