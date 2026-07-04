Scriptname _ne_PooledLetterQuest extends Quest

ReferenceAlias Property Sender  Auto

ReferenceAlias Property LetterRef  Auto

WICourierScript Property WICourier  Auto

Function DispatchLetterToCourier()
    ObjectReference letterObjRef = LetterRef.GetReference()
    if letterObjRef == None
        Debug.Trace("[_ne_PooledLetterQuest] LetterRef empty at Stage 10")
        SetStage(60)
        return
    endIf
    WICourier.AddItemToContainer(letterObjRef, 1)
EndFunction

Function Shutdown()
    ; Ask vanilla WICourier to release its tracking of this letter
    ; BEFORE we reset the quest (Reset() unfills the LetterRef alias,
    ; after which we can no longer produce the REFR argument
    ; removeRefFromContainer needs). removeRefFromContainer removes
    ; the letter from the courier's staging container if it's still
    ; there AND decrements the WICourierItemCount global that gates
    ; the courier's change-location event quest — skipping it leaves
    ; the courier system holding a stale reference and item count.
    ObjectReference letterObjRef = LetterRef.GetReference()
    if letterObjRef != None && WICourier != None
        WICourier.removeRefFromContainer(letterObjRef, False)
    endIf
    Stop()
    Reset()
EndFunction
