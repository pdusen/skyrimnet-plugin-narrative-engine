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
    Stop()
    Reset()
EndFunction
