;BEGIN FRAGMENT CODE - Do not edit anything between this and the end comment
;NEXT FRAGMENT INDEX 8
Scriptname _ne__QF__ne_PooledLetterQuest_0500081A Extends Quest Hidden

;BEGIN ALIAS PROPERTY LetterRef
;ALIAS PROPERTY TYPE ReferenceAlias
ReferenceAlias Property Alias_LetterRef Auto
;END ALIAS PROPERTY

;BEGIN ALIAS PROPERTY Sender
;ALIAS PROPERTY TYPE ReferenceAlias
ReferenceAlias Property Alias_Sender Auto
;END ALIAS PROPERTY

;BEGIN FRAGMENT Fragment_1
Function Fragment_1()
;BEGIN AUTOCAST TYPE _ne_PooledLetterQuest
Quest __temp = self as Quest
_ne_PooledLetterQuest kmyQuest = __temp as _ne_PooledLetterQuest
;END AUTOCAST
;BEGIN CODE
; 10
; Queue letter for courier
kmyQuest.DispatchLetterToCourier()
;END CODE
EndFunction
;END FRAGMENT

;BEGIN FRAGMENT Fragment_7
Function Fragment_7()
;BEGIN AUTOCAST TYPE _ne_PooledLetterQuest
Quest __temp = self as Quest
_ne_PooledLetterQuest kmyQuest = __temp as _ne_PooledLetterQuest
;END AUTOCAST
;BEGIN CODE
; 200
; Quest shutdown
kmyQuest.Shutdown()
;END CODE
EndFunction
;END FRAGMENT

;BEGIN FRAGMENT Fragment_4
Function Fragment_4()
;BEGIN CODE
; 40
; Letter read by player
;END CODE
EndFunction
;END FRAGMENT

;BEGIN FRAGMENT Fragment_3
Function Fragment_3()
;BEGIN CODE
; 30
; Letter delivered to player inventory
;END CODE
EndFunction
;END FRAGMENT

;BEGIN FRAGMENT Fragment_6
Function Fragment_6()
;BEGIN CODE
; 60
; Letter recycled by allocator

SetStage(200)
;END CODE
EndFunction
;END FRAGMENT

;BEGIN FRAGMENT Fragment_2
Function Fragment_2()
;BEGIN CODE
; 20
; Letter in courier container
;END CODE
EndFunction
;END FRAGMENT

;BEGIN FRAGMENT Fragment_0
Function Fragment_0()
;BEGIN CODE
; 0
; Quest is starting up
SetStage(10)
;END CODE
EndFunction
;END FRAGMENT

;BEGIN FRAGMENT Fragment_5
Function Fragment_5()
;BEGIN CODE
; 50
; Letter disposed by player
SetStage(200)
;END CODE
EndFunction
;END FRAGMENT

;END FRAGMENT CODE - Do not edit anything between this and the begin comment
