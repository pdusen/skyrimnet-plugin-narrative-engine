Scriptname _ne_MCM extends SKI_ConfigBase
;
; NarrativeEngine MCM page. Single page, two columns:
;   Left  — About (mod identifying info, read-only).
;   Right — Controls: dashboard hotkey + three modifier toggles.
;
; The rebound hotkey persists via the two auto properties below (Papyrus
; save data). The C++ side is notified of every rebind — and of the current
; binding on save-load — through the "_ne_DashboardHotkeyChanged" ModEvent.
; C++'s MCMEventSink consumes it and updates the in-memory Settings.
;
; The bitmask convention for DashboardHotkeyModifiers matches SkyUI: bit 0 =
; Shift, bit 1 = Ctrl, bit 2 = Alt. Same convention the C++ HotkeySink
; assembles when it reads GetAsyncKeyState() at input time, so no remap.
;
; Why modifiers are separate toggles and not part of the keymap capture:
; SkyUI's AddKeyMapOptionST dismisses its "Press a button" modal on the
; very first keydown, whether or not that key is a modifier. There is no
; way through the public MCM API to make the modal wait for a combo (see
; SkyUI wiki + SKI_ConfigMenu.psc reference implementation, which itself
; doesn't try). The universally-adopted workaround is to bind a primary
; key with the keymap and expose Shift/Ctrl/Alt as independent toggles;
; the C++ side checks held-modifier state at activation time.


; -------------------------------------------------------------------------
;   Persisted properties
; -------------------------------------------------------------------------

int Property DashboardHotkeyDXSC = 65 Auto
{DirectX scan code for the dashboard toggle. Default 65 = DIK_F7.}

int Property DashboardHotkeyModifiers = 0 Auto
{SkyUI modifier bitmask: 1=Shift, 2=Ctrl, 4=Alt. Combinable.}


; -------------------------------------------------------------------------
;   SKI_ConfigBase overrides
; -------------------------------------------------------------------------

Event OnConfigInit()
    ModName = "NarrativeEngine"
    Pages = new string[1]
    Pages[0] = ""
EndEvent


Event OnPageReset(string a_page)
    ; Left column: About
    SetCursorFillMode(TOP_TO_BOTTOM)
    SetCursorPosition(0)
    AddHeaderOption("About")
    AddTextOption("NarrativeEngine", "v0.1.0 (dev)",                       OPTION_FLAG_DISABLED)
    AddTextOption("Author",          "pdusen",                             OPTION_FLAG_DISABLED)
    AddTextOption("Description",     "Narrative Director for SkyrimNet",   OPTION_FLAG_DISABLED)

    ; Right column: Controls
    SetCursorPosition(1)
    AddHeaderOption("Controls")
    AddKeyMapOptionST("DashboardHotkey", "Dashboard Hotkey", DashboardHotkeyDXSC)
    AddToggleOptionST("RequireShift", "Require Shift", HasModifier(1))
    AddToggleOptionST("RequireCtrl",  "Require Ctrl",  HasModifier(2))
    AddToggleOptionST("RequireAlt",   "Require Alt",   HasModifier(4))
EndEvent


; SKI_QuestBase.OnGameReload() fires when the player loads a save, driven
; by SKI_PlayerLoadGameAlias attached to the Player alias on this quest.
; Re-send the current binding so C++'s Settings reflects whatever the
; Papyrus save carried, overwriting the plugin-INI defaults that
; Settings::Load seeded at boot.
Event OnGameReload()
    Parent.OnGameReload()
    SendHotkeyChangedEvent()
EndEvent


; -------------------------------------------------------------------------
;   State option handlers — primary key
; -------------------------------------------------------------------------

State DashboardHotkey
    ; Reject modifier-only keycodes. Binding "primary = LCtrl" would leave
    ; no way to actually fire the hotkey (the C++ side treats modifiers as
    ; a gate, not as a primary key), so silently revert the display.
    Event OnKeyMapChangeST(int a_keyCode, string a_conflictControl, string a_conflictName)
        if IsModifierKeyCode(a_keyCode)
            SetKeyMapOptionValueST(DashboardHotkeyDXSC)
            return
        endif

        DashboardHotkeyDXSC = a_keyCode
        SendHotkeyChangedEvent()
        SetKeyMapOptionValueST(a_keyCode)
    EndEvent

    Event OnDefaultST()
        DashboardHotkeyDXSC = 65
        SendHotkeyChangedEvent()
        SetKeyMapOptionValueST(65)
    EndEvent
EndState


; -------------------------------------------------------------------------
;   State option handlers — modifier toggles
; -------------------------------------------------------------------------

State RequireShift
    Event OnSelectST()
        SetModifier(1, !HasModifier(1))
        SetToggleOptionValueST(HasModifier(1))
        SendHotkeyChangedEvent()
    EndEvent

    Event OnDefaultST()
        SetModifier(1, false)
        SetToggleOptionValueST(false)
        SendHotkeyChangedEvent()
    EndEvent
EndState

State RequireCtrl
    Event OnSelectST()
        SetModifier(2, !HasModifier(2))
        SetToggleOptionValueST(HasModifier(2))
        SendHotkeyChangedEvent()
    EndEvent

    Event OnDefaultST()
        SetModifier(2, false)
        SetToggleOptionValueST(false)
        SendHotkeyChangedEvent()
    EndEvent
EndState

State RequireAlt
    Event OnSelectST()
        SetModifier(4, !HasModifier(4))
        SetToggleOptionValueST(HasModifier(4))
        SendHotkeyChangedEvent()
    EndEvent

    Event OnDefaultST()
        SetModifier(4, false)
        SetToggleOptionValueST(false)
        SendHotkeyChangedEvent()
    EndEvent
EndState


; -------------------------------------------------------------------------
;   Helpers
; -------------------------------------------------------------------------

; True if the given DIK code is one of the six modifier variants
; (L/R Shift/Ctrl/Alt). Modifier-only primary bindings are rejected
; because the C++ HotkeySink checks modifiers as a gate around a
; non-modifier primary key.
bool Function IsModifierKeyCode(int a_keyCode)
    return a_keyCode == 42 || a_keyCode == 54 \
        || a_keyCode == 29 || a_keyCode == 157 \
        || a_keyCode == 56 || a_keyCode == 184
EndFunction


bool Function HasModifier(int a_bit)
    return Math.LogicalAnd(DashboardHotkeyModifiers, a_bit) != 0
EndFunction


Function SetModifier(int a_bit, bool a_on)
    if a_on
        DashboardHotkeyModifiers = Math.LogicalOr(DashboardHotkeyModifiers, a_bit)
    else
        ; No LogicalAndNot in Papyrus; XOR with the bit only when it's set.
        if HasModifier(a_bit)
            DashboardHotkeyModifiers = Math.LogicalXor(DashboardHotkeyModifiers, a_bit)
        endif
    endif
EndFunction


; Send the current (DXSC, modifiers) pair to C++'s MCMEventSink.
;   numArg = DXSC (float, exact for small ints)
;   strArg = modifiers bitmask formatted as decimal string
; The C++ sink parses strArg as int and casts numArg back to int.
Function SendHotkeyChangedEvent()
    string modsStr = "" + DashboardHotkeyModifiers
    SendModEvent("_ne_DashboardHotkeyChanged", modsStr, DashboardHotkeyDXSC as float)
EndFunction
