# NarrativeEngine

> [!WARNING]
> **NarrativeEngine is in early alpha.** Features are still landing, and edge-case behavior hasn't been broadly
> tested. Do not install it into a stable or long-term playthrough you care about — try it on a dedicated save or
> a short-lived character while the mod is still in this state.

A Skyrim mod, in active development. The mod is a **plugin for SkyrimNet** — not a standalone mod.

**SkyrimNet** is itself a Skyrim mod (combining an `.esp` and an SKSE plugin) that puts LLMs in control of various
emergent gameplay systems — most notably ad-hoc dialogue generation for NPCs. SkyrimNet exposes a plugin surface so
other mods can extend its LLM-driven systems.

NarrativeEngine ships both an `.esp` and an SKSE plugin of its own (the same shape as SkyrimNet itself, and the same
shape as the IntelEngine prior-art reference described in [`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md)).

## Table of Contents

- [What This Plugin Does](#what-this-plugin-does)
- [Requirements](#requirements)
- [Getting Started](#getting-started)
- [Q&A](#qa)
- [Contributing](#contributing)

## What This Plugin Does

NarrativeEngine is a **Director** that watches your playthrough and, at a steady cadence, decides whether *right now*
is a good moment for something to happen — a letter arriving, someone showing up to talk to you, a fight breaking
out — and then makes it happen using characters, topics, and pretexts drawn from what you've actually been doing
in-game.

The goal is a playthrough with *shape*. Instead of random encounters that fire on distance or timer, events unfold in
a familiar dramatic arc: quiet setup, escalating pressure, a peak, a wind-down, and a return to quiet before the next
cycle begins. Which phase you're in influences both *when* the next event fires (peaks are brief; setups linger) and
*what kind* of event feels right for the moment (a climax wants a confrontation; a resolution wants something
reflective).

Under the hood, NarrativeEngine leans heavily on SkyrimNet's memory system — the same one that lets NPCs remember
your past conversations — to answer questions like "who around here has a reason to reach out?" and "what should this
letter be about?". Every so often the plugin sends a small evaluation request to the model to score the current
dramatic tension, then either advances the arc to its next phase or picks one of a small set of *narrative beats* to
dispatch:

- **NPC Letter** — a courier delivers a handwritten letter from someone whose path has recently crossed yours,
  written in their voice, about something that actually happened between you.
- **NPC Visit** — an NPC travels to your location, opens a spoken exchange in their own words, and departs when
  the conversation reaches a natural close.
- **Ambush** — bandits assemble along your path, sized and positioned by the model based on where the story
  currently sits and what tone fits.

Each beat's specifics — who, why, what topic, what mood, how far away — are authored by the LLM from the actual
state of your game, not drawn from a static list. NarrativeEngine also ships an in-game dashboard (bound to a
hotkey, F7 by default) that surfaces what the Director is thinking, shows why it picked what it picked, and lets
you tune the pacing and the individual beats to your taste.

## Requirements

**SkyrimNet.** NarrativeEngine is a plugin for SkyrimNet and cannot run without it. Install SkyrimNet first and
confirm it works on its own — if characters aren't speaking and NPCs aren't remembering conversations, NarrativeEngine
won't run either. SkyrimNet has its own set of prerequisites (SKSE, Address Library, PapyrusUtil, a configured LLM
provider, and so on); rather than duplicate that list here, defer to their maintained
[System Requirements](https://github.com/MinLL/SkyrimNet-GamePlugin#-system-requirements) section and make sure every
item there is satisfied before layering NarrativeEngine on top.

Beyond what SkyrimNet already requires, NarrativeEngine needs:

- **MCM Helper** — hosts NarrativeEngine's Mod Configuration Menu page (mod credits and a hotkey rebind for the
  in-game dashboard). Available on Nexus Mods.

That's the only *additional* install. NarrativeEngine also hard-requires **PrismaUI** (which hosts the in-game
dashboard) and **SkyUI** (which MCM Helper depends on), but both are already in SkyrimNet's requirements list
linked above — following that guide end-to-end covers them. If SkyrimNet is working, they're in place.

## Getting Started

1. **Install SkyrimNet and its prerequisites, and confirm SkyrimNet works standalone.** Follow the SkyrimNet install
   guide end to end — SKSE, Address Library, PapyrusUtil, an LLM provider (OpenRouter is the path of least
   resistance), and SkyrimNet itself. Load a save and verify NPCs are speaking generated dialogue and remembering
   past conversations. Do **not** install NarrativeEngine until this works — trying to debug both at once is
   miserable.
2. **Install MCM Helper.** Not one of SkyrimNet's prerequisites, but NarrativeEngine needs it — the mod's MCM
   page (credits + dashboard-hotkey rebind) won't register without it. Install through your mod manager the same
   way any other Skyrim mod goes in; no per-mod configuration needed.
3. **Install NarrativeEngine.** Add it to your load order *after* SkyrimNet (SkyrimNet must be loaded first so
   NarrativeEngine can register its plugin manifest with it). No FOMOD choices to make — it's a single-option
   install.
4. **Launch the game and open the dashboard.** Load a save (or start a new game). Press **F7** — the NarrativeEngine
   dashboard should appear as an overlay. If it doesn't, check `Data/SKSE/Plugins/NarrativeEngine.log` for the
   line reading `DashboardUIManager: initialized`; its absence usually means PrismaUI failed to load, which
   almost always traces back to a SKSE / Address Library mismatch. Close the dashboard the same way you opened
   it, or with Esc.
5. **Point the Director at a fast, cheap model.** Open the SkyrimNet dashboard (SkyrimNet's own PrismaUI
   overlay) and navigate to **AI → Models**. The full list of SkyrimNet's model profiles appears there,
   including two NarrativeEngine registers: `narrative_engine_director` and `narrative_engine_composer`. Only
   the director needs your attention: it runs on every tick to score dramatic tension and pick beats, so a
   top-tier model here gets expensive fast. Override it to something small and cheap;
   `deepseek/deepseek-v4-flash` is a reasonable pick on OpenRouter. The composer profile (letters and
   briefings written in a specific NPC's voice) fires only when a beat actually starts, so its default —
   inheriting SkyrimNet's dialogue model — is fine.
6. **Play.** The Director starts ticking as soon as you're in-world. First few ticks may not fire any beats — the
   arc has to build up tension and the phase has to overstay its ideal duration first. Open the dashboard any time
   to watch what it's thinking, tune the phase durations under the Settings tab if the pacing feels off, or
   temporarily disable the tick from the Dispatch tab when you want a quiet stretch.

## Q&A

### Doesn't IntelEngine already do a lot of this?

Yes, IntelEngine covers some of the same ground. But it isn't currently being maintained, and there are aspects
of its architecture I wanted to approach differently. The main one: IntelEngine's events fire on a fixed timer
cadence, with little regard for what the player is actually doing in the moment. The goal here is for the events
this plugin generates to weave seamlessly into your ongoing gameplay.

### Is this meant to replace IntelEngine?

No, not currently. IntelEngine provides a lot of features and event types that this plugin has no answer for.
That said, as development continues, the hope is for this plugin to grow feature-rich enough to fill many of the
same needs. As new features land that overlap with IntelEngine's, this README will be updated with guidance on
running the two plugins side by side.

### Was AI used to write this plugin?

Yes, heavily. I'm a full-time software engineer at my day job, and taking on a major solo programming project in
what little spare time I have left is unappealing. It's also been several years since I was regularly writing
C++, which I don't use at my day job, so getting fully up to speed on modern C++ programming isn't something I
have time for.

That said, I didn't just ask Claude to implement the plugin top-to-bottom on its own; I have high standards for
software and am very picky about the way things are done. Every step of development involved heavy direction
from me, and there's a detailed planning stage before every phase of development. If you want a glimpse of that
process, the [implementation phase-planning documents](docs/implementation/) are all still here in the repository
for anyone to read.

If you have a moral objection to AI, I don't blame you, but I'm also not sure why you'd be looking at this plugin
in the first place.

### What kind of monthly cost should I expect running this alongside SkyrimNet?

The straight answer is that I haven't done a thorough audit of this plugin's LLM costs, and given the mod's
alpha state, I don't feel comfortable making a firm guess right now. I don't think they're crazy, but if you're
on an extremely tight budget, I recommend against running this for now.

### Is it safe to add or remove NarrativeEngine mid-playthrough?

It's safe to add at any time. Once you've added it, I do NOT recommend removing it — multiple quests run in the
background, and pulling the plugin out once they're active would have unpredictable results.

### How often should I actually expect something to happen during play?

There's no fixed answer to this. The system moves through a cycle of narrative phases; some build tension,
others release it. It watches what's currently going on in your game, and if the current phase has been active
longer than its ideal duration, it tries to fire an event to help move things along to the next phase.

Theoretically, if your regular gameplay happens to perfectly follow the cadence of rising and falling narrative
action through each of the narrative phases, you might never see an event fire at all. In my experience,
however, that's highly unlikely.

You can adjust the ideal duration for each phase from the dashboard's Settings tab. If you don't care about the
narrative cycle and just want to see a particular type of event right now, the Dispatch tab has buttons for that
— though they're primarily meant for debugging.

### Can I disable specific beats I don't want to see?

Yes, the dashboard's Dispatch tab has checkboxes to disable specific beats you don't want to see. However, at
the moment, the list of available beats is small enough that I don't recommend turning any of them off unless
you feel very strongly about one.

### Will the Director interrupt me during vanilla quests, combat, or dialogue?

No. Those situations represent what the plugin considers "alpha canon" — facts about what's happening in the
game that can't be disputed or overwritten. The plugin will observe them and factor them in when judging the
current scene's narrative tension, but it won't generate new events while they're occurring.

There are also safeguards in place to prevent the plugin from taking any action at all while you're in a menu
or in vanilla dialogue.

### Does the plugin work in non-English Skyrim installations?

It depends. If your SkyrimNet installation is already generating dialogue and other content correctly in your
preferred language, that should carry over here. I haven't tested it myself, though, so no guarantees.

Static content — the PrismaUI dashboard, and certain events the plugin writes to SkyrimNet's event log — is
currently English-only.

## Contributing

Everything a contributor needs — the IntelEngine prior-art reference, working-directory and naming conventions,
C++ source layout, environment setup, build / statics / ESP / Papyrus workflows, linting and autoformatting, and
markdown conventions — lives in [`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md). Start there.
