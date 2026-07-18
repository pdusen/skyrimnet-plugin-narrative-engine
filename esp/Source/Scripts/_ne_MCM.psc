Scriptname _ne_MCM extends MCM_ConfigBase
;
; NarrativeEngine MCM page — driven declaratively by MCM Helper.
;
; The page layout, controls, and INI bindings live in
; Data/MCM/Config/NarrativeEngine/config.json. MCM Helper reads it at
; runtime and drives the SkyUI MCM page from the JSON. Persistent
; values land in Data/MCM/Settings/NarrativeEngine.ini (cross-save
; global). C++'s Settings::Load reads that file at kDataLoaded so the
; INI is the source of truth for the dashboard hotkey.
;
; This script has one job: when the player changes a value in the MCM,
; fire the "_ne_DashboardHotkeyChanged" ModEvent so C++'s MCMEventSink
; re-reads the INI live (rather than waiting until the next boot).
; MCM Helper always writes the INI before firing OnSettingChange, so
; the sink can rely on the file already being current when the event
; arrives.

Event OnSettingChange(string a_ID)
    SendModEvent("_ne_DashboardHotkeyChanged")
EndEvent
