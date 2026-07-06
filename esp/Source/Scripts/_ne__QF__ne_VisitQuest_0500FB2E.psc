;BEGIN FRAGMENT CODE - Do not edit anything between this and the end comment
;NEXT FRAGMENT INDEX 9
Scriptname _ne__QF__ne_VisitQuest_0500FB2E Extends Quest Hidden

;BEGIN ALIAS PROPERTY SpawnMarker
;ALIAS PROPERTY TYPE ReferenceAlias
ReferenceAlias Property Alias_SpawnMarker Auto
;END ALIAS PROPERTY

;BEGIN ALIAS PROPERTY PlayerRef
;ALIAS PROPERTY TYPE ReferenceAlias
ReferenceAlias Property Alias_PlayerRef Auto
;END ALIAS PROPERTY

;BEGIN ALIAS PROPERTY Sender
;ALIAS PROPERTY TYPE ReferenceAlias
ReferenceAlias Property Alias_Sender Auto
;END ALIAS PROPERTY

;BEGIN ALIAS PROPERTY ReturnAnchor
;ALIAS PROPERTY TYPE ReferenceAlias
ReferenceAlias Property Alias_ReturnAnchor Auto
;END ALIAS PROPERTY

;BEGIN FRAGMENT Fragment_0
Function Fragment_0()
;BEGIN CODE
; 0
; Startup

SetStage(10)
;END CODE
EndFunction
;END FRAGMENT

;BEGIN FRAGMENT Fragment_4
Function Fragment_4()
;BEGIN CODE
; 27
; ReEngage
;END CODE
EndFunction
;END FRAGMENT

;BEGIN FRAGMENT Fragment_2
Function Fragment_2()
;BEGIN CODE
; 20
; Discuss
;END CODE
EndFunction
;END FRAGMENT

;BEGIN FRAGMENT Fragment_3
Function Fragment_3()
;BEGIN CODE
; 25
; OnHold
;END CODE
EndFunction
;END FRAGMENT

;BEGIN FRAGMENT Fragment_1
Function Fragment_1()
;BEGIN AUTOCAST TYPE _ne_VisitQuest
Quest __temp = self as Quest
_ne_VisitQuest kmyQuest = __temp as _ne_VisitQuest
;END AUTOCAST
;BEGIN CODE
; 10
; Warp & Salutation
kmyQuest.MoveSenderToSpawnMarker()
;END CODE
EndFunction
;END FRAGMENT

;BEGIN FRAGMENT Fragment_6
Function Fragment_6()
;BEGIN AUTOCAST TYPE _ne_VisitQuest
Quest __temp = self as Quest
_ne_VisitQuest kmyQuest = __temp as _ne_VisitQuest
;END AUTOCAST
;BEGIN CODE
; 50
; ReturnHome entry
kmyQuest.StartReturnTravel()
;END CODE
EndFunction
;END FRAGMENT

;BEGIN FRAGMENT Fragment_7
Function Fragment_7()
;BEGIN CODE
; 60
; Rollback
SetStage(200)
;END CODE
EndFunction
;END FRAGMENT

;BEGIN FRAGMENT Fragment_8
Function Fragment_8()
;BEGIN AUTOCAST TYPE _ne_VisitQuest
Quest __temp = self as Quest
_ne_VisitQuest kmyQuest = __temp as _ne_VisitQuest
;END AUTOCAST
;BEGIN CODE
; 200
; Terminal
kmyQuest.Shutdown()
;END CODE
EndFunction
;END FRAGMENT

;BEGIN FRAGMENT Fragment_5
Function Fragment_5()
;BEGIN CODE
; 30
; Valediction
;END CODE
EndFunction
;END FRAGMENT

;END FRAGMENT CODE - Do not edit anything between this and the begin comment
