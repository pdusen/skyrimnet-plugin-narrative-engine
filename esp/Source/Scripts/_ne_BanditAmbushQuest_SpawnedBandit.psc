Scriptname _ne_BanditAmbushQuest_SpawnedBandit extends ReferenceAlias

ReferenceAlias Property PlayerRef Auto

Float Property TunnelVisionReleaseRangeUnits = 1500.0 Auto
Float Property TunnelVisionPollIntervalSec   = 0.5   Auto

bool initialized          = false
bool released             = false
int  postReleaseLockTicks = 0

Event OnAliasInit()
    Initialize()
EndEvent

Event OnLoad()
    Initialize()
EndEvent

Function Initialize()
    if initialized
        return
    endif
    Actor bandit = GetActorReference()
    Actor player = PlayerRef.GetActorReference()
    if !bandit || !player || !bandit.Is3DLoaded()
        return
    endif
    initialized = true
    bandit.SetActorValue("Aggression", 0.0)
    bandit.DrawWeapon()
    bandit.EvaluatePackage()
    RegisterForSingleUpdate(TunnelVisionPollIntervalSec)
EndFunction

Event OnUpdate()
    Actor bandit = GetActorReference()
    Actor player = PlayerRef.GetActorReference()
    if !bandit || !player
        return
    endif
    if !released
        if bandit.GetDistance(player) <= TunnelVisionReleaseRangeUnits
            bandit.SetActorValue("Aggression", 2.0)
            bandit.EvaluatePackage()
            bandit.StartCombat(player)
            released = true
            postReleaseLockTicks = 3
        endif
        RegisterForSingleUpdate(TunnelVisionPollIntervalSec)
        return
    endif
    if postReleaseLockTicks > 0
        if bandit.GetCombatTarget() != player
            bandit.StartCombat(player)
        endif
        postReleaseLockTicks -= 1
        RegisterForSingleUpdate(TunnelVisionPollIntervalSec)
    endif
EndEvent

Event OnDeath(Actor akKiller)
    _ne_BanditAmbushQuest q = GetOwningQuest() as _ne_BanditAmbushQuest
    if q
        q.CheckAllBanditsDead()
    endif
EndEvent

Event OnDying(Actor akKiller)
    _ne_BanditAmbushQuest q = GetOwningQuest() as _ne_BanditAmbushQuest
    if q
        q.CheckAllBanditsDead()
    endif
EndEvent
