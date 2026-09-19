#pragma once

#include <RE/Skyrim.h>

#include <string>

// DebugNotify — corner-of-the-screen notices for the handful of moments
// worth knowing about while testing.
//
// The beats are mostly invisible until they land: a visitor is warped in
// out of sight and walks for a minute before speaking, a letter sits in
// the courier's inventory until he finds you, an ambush spawns behind a
// rise. When one of those silently does not happen, the only way to tell
// "it never fired" from "it fired and I missed it" is to read the log
// afterwards. These notifications collapse that to a glance.
//
// Off by default and gated on `bDebugNotifications`. They are a testing
// aid and say things a character could not know — who is walking toward
// you and from which faction — so they have no place in a normal game.
//
// == Threading ==
//
// Callable from ANY thread. `RE::DebugNotification` touches the UI and
// must run on the main thread, so every post is marshalled: onto the
// plugin thread via AsyncDispatch (the one API that needs no token from
// its caller, which is what lets the gossip and SkyrimNet threads use
// this), and from there onto the main thread.
//
// That means a post is asynchronous and lands a frame or two later. For
// a debug notice that is invisible, and it buys one rule that holds
// everywhere rather than a token-typed variant per calling context.
namespace NarrativeEngine::DebugNotify
{
    // Show `text` if debug notifications are enabled, otherwise do
    // nothing. The setting is read when the post reaches the main
    // thread, so toggling it off in the dashboard silences posts already
    // in flight.
    void Post(std::string text);

    // Post a notice that names an actor, with `pattern` a format string
    // carrying exactly one `{}` for the name.
    //
    // Takes a FormID rather than a name because the callers that need
    // this are not on the main thread -- the gossip simulation seeds
    // rumours on its own -- and reading a form off the main thread is
    // not allowed. The lookup happens where the notice is shown.
    //
    // `actor` may be a placed reference or a base NPC record; the
    // gossip graph is keyed by the latter.
    void PostActorNamed(RE::FormID actor, std::string pattern);
} // namespace NarrativeEngine::DebugNotify
