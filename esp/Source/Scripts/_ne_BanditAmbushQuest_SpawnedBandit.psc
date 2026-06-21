Scriptname _ne_BanditAmbushQuest_SpawnedBandit extends ReferenceAlias

ReferenceAlias Property PlayerRef Auto

Float Property TunnelVisionReleaseRangeUnits = 2800.0 Auto
Float Property TunnelVisionPollIntervalSec   = 0.5   Auto

bool combatStarted = false
bool released      = false

Event OnAliasInit()
    TryStartCombat()
EndEvent

Event OnLoad()
    TryStartCombat()
EndEvent

Function TryStartCombat()
    if combatStarted
        return
    endif
    Actor bandit = GetActorReference()
    Actor player = PlayerRef.GetActorReference()
    if bandit && player && bandit.Is3DLoaded()
        bandit.SetGhost(true)
        bandit.SetActorValue("Aggression", 0.0)
        bandit.StartCombat(player)
        bandit.EvaluatePackage()
        combatStarted = true
        RegisterForSingleUpdate(TunnelVisionPollIntervalSec)
    endif
EndFunction

Event OnUpdate()
    if released
        return
    endif
    Actor bandit = GetActorReference()
    Actor player = PlayerRef.GetActorReference()
    if !bandit || !player
        return
    endif
    if bandit.GetDistance(player) <= TunnelVisionReleaseRangeUnits
        bandit.SetGhost(false)
        bandit.SetActorValue("Aggression", 2.0)
        bandit.StartCombat(player)
        bandit.EvaluatePackage()
        released = true
        UnregisterForUpdate()
    else
        if bandit.GetCombatTarget() != player
            bandit.StartCombat(player)
            bandit.EvaluatePackage()
        endif
        RegisterForSingleUpdate(TunnelVisionPollIntervalSec)
    endif
EndEvent
