import { useState } from 'react';

import type { DirectorState } from '../../types';

interface Props {
    state: DirectorState;
}

declare global {
    interface Window {
        ne_setDebugMode?: (arg: string) => void;
        ne_setTickInterval?: (arg: string) => void;
        ne_setMinPhaseDuration?: (arg: string) => void;
        ne_setPhaseIdealDuration?: (arg: string) => void;
        ne_beginHotkeyRebind?: (arg: string) => void;
        ne_cancelHotkeyRebind?: (arg: string) => void;
        ne_setLetterComposeMemoryCap?: (arg: string) => void;
        ne_setLetterComposeDialogueCap?: (arg: string) => void;
        ne_setActionSelectEventCap?: (arg: string) => void;
        ne_setActionSelectLetterMemoryCap?: (arg: string) => void;
        ne_setActionSelectVisitMemoryCap?: (arg: string) => void;
    }
}

// Per-phase slider config. Ranges match the clamps in
// DashboardUIManager::OnSetPhaseIdealDuration — if either side changes,
// both must stay in sync.
const PHASE_SLIDERS: Array<{
    key: keyof DirectorState['settings']['ideal_duration_seconds'];
    label: string;
    min: number;
    max: number;
}> = [
    { key: 'exposition', label: 'Exposition', min: 60, max: 1200 },
    { key: 'rising_action', label: 'Rising Action', min: 60, max: 1200 },
    { key: 'climax', label: 'Climax', min: 30, max: 600 },
    { key: 'falling_action', label: 'Falling Action', min: 60, max: 1200 },
    { key: 'resolution', label: 'Resolution', min: 60, max: 1200 },
];

// Live-update slider. Displays the current-drag value locally (via
// oninput) so the label tracks the thumb, but only fires the C++
// listener on release (via onchange) — one INI write per drag, not one
// per pixel of drag distance.
function LiveSlider({
    value,
    min,
    max,
    step,
    unit,
    onCommit,
}: {
    value: number;
    min: number;
    max: number;
    step: number;
    unit: string;
    onCommit: (v: number) => void;
}) {
    const [drag, setDrag] = useState<number | null>(null);
    const display = drag ?? value;
    return (
        <div className="settings-slider">
            <input
                type="range"
                min={min}
                max={max}
                step={step}
                value={display}
                onInput={e => setDrag(Number((e.target as HTMLInputElement).value))}
                onChange={e => {
                    const v = Number(e.target.value);
                    setDrag(null);
                    onCommit(v);
                }}
            />
            <span className="settings-slider-value">
                {display}
                {unit}
            </span>
        </div>
    );
}

function HotkeyRebindModal() {
    const onCancel = () => window.ne_cancelHotkeyRebind?.('');
    return (
        <div className="settings-rebind-overlay" role="dialog" aria-modal="true">
            <div className="settings-rebind-panel">
                <h3>Press any key to bind.</h3>
                <p>
                    Hold modifiers (Ctrl, Shift, Alt) while pressing to include them. Esc
                    cancels. A modifier alone is not a valid binding.
                </p>
                <button type="button" className="bulk-button" onClick={onCancel}>
                    Cancel
                </button>
            </div>
        </div>
    );
}

export function SettingsTab({ state }: Props) {
    const s = state.settings;

    const onToggleDebug = (e: React.ChangeEvent<HTMLInputElement>) => {
        window.ne_setDebugMode?.(e.target.checked ? 'true' : 'false');
    };
    const onToggleTick = (e: React.ChangeEvent<HTMLInputElement>) => {
        window.ne_setTickEnabled?.(e.target.checked ? 'true' : 'false');
    };
    const onRebind = () => window.ne_beginHotkeyRebind?.('');
    const onCommitTickInterval = (v: number) => window.ne_setTickInterval?.(String(v));
    const onCommitMinPhaseDuration = (v: number) => window.ne_setMinPhaseDuration?.(String(v));
    const onCommitPhase = (phase: string, seconds: number) => {
        window.ne_setPhaseIdealDuration?.(JSON.stringify({ phase, seconds }));
    };
    const onCommitLetterMemoryCap = (v: number) => window.ne_setLetterComposeMemoryCap?.(String(v));
    const onCommitLetterDialogueCap = (v: number) => window.ne_setLetterComposeDialogueCap?.(String(v));
    const onCommitActionSelectEventCap = (v: number) => window.ne_setActionSelectEventCap?.(String(v));
    const onCommitActionSelectLetterMemoryCap = (v: number) => window.ne_setActionSelectLetterMemoryCap?.(String(v));
    const onCommitActionSelectVisitMemoryCap = (v: number) => window.ne_setActionSelectVisitMemoryCap?.(String(v));

    return (
        <div className="tab-content settings-tab">
            {s.dashboard_hotkey_capture_active && <HotkeyRebindModal />}

            <section className="panel">
                <h2>General</h2>
                <label className="tick-toggle">
                    <input type="checkbox" checked={s.debug_mode} onChange={onToggleDebug} />
                    <span>Debug Mode</span>
                </label>
                <div className="settings-row">
                    <span className="settings-row-label">Dashboard Hotkey</span>
                    <span className="settings-hotkey-display">{s.dashboard_hotkey_display}</span>
                    <button type="button" className="bulk-button" onClick={onRebind}>
                        Rebind
                    </button>
                </div>
            </section>

            <section className="panel">
                <h2>Narrative Director</h2>
                <label className="tick-toggle">
                    <input type="checkbox" checked={s.tick_enabled} onChange={onToggleTick} />
                    <span>Enable Narrative Tick</span>
                </label>
                <div className="settings-row">
                    <span className="settings-row-label">Tick Interval</span>
                    <LiveSlider
                        value={s.tick_interval_seconds}
                        min={10}
                        max={600}
                        step={5}
                        unit="s"
                        onCommit={onCommitTickInterval}
                    />
                </div>
                <div className="settings-row">
                    <span className="settings-row-label">Min Phase Duration</span>
                    <LiveSlider
                        value={s.min_phase_duration_seconds}
                        min={0}
                        max={600}
                        step={5}
                        unit="s"
                        onCommit={onCommitMinPhaseDuration}
                    />
                </div>
            </section>

            <section className="panel">
                <h2>Narrative Cycle Phase Durations</h2>
                {PHASE_SLIDERS.map(p => (
                    <div key={p.key} className="settings-row">
                        <span className="settings-row-label">{p.label}</span>
                        <LiveSlider
                            value={s.ideal_duration_seconds[p.key]}
                            min={p.min}
                            max={p.max}
                            step={5}
                            unit="s"
                            onCommit={v => onCommitPhase(p.key, v)}
                        />
                    </div>
                ))}
            </section>

            <section className="panel">
                <h2>Action Select Prompt Caps</h2>
                <p className="settings-panel-hint">
                    Bound how much rendered context lands in the beat-select prompt
                    (which picks which action fires). Lower these if your LLM's context
                    window is tight.
                </p>
                <div className="settings-row">
                    <span className="settings-row-label">Recent Events</span>
                    <LiveSlider
                        value={s.action_select_event_render_cap}
                        min={3}
                        max={30}
                        step={1}
                        unit=""
                        onCommit={onCommitActionSelectEventCap}
                    />
                </div>
                <div className="settings-row">
                    <span className="settings-row-label">Letter Sender Memories</span>
                    <LiveSlider
                        value={s.action_select_letter_memory_render_cap}
                        min={3}
                        max={15}
                        step={1}
                        unit=""
                        onCommit={onCommitActionSelectLetterMemoryCap}
                    />
                </div>
                <div className="settings-row">
                    <span className="settings-row-label">Visit Sender Memories</span>
                    <LiveSlider
                        value={s.action_select_visit_memory_render_cap}
                        min={3}
                        max={15}
                        step={1}
                        unit=""
                        onCommit={onCommitActionSelectVisitMemoryCap}
                    />
                </div>
            </section>

            <section className="panel">
                <h2>Letter Compose Prompt Caps</h2>
                <p className="settings-panel-hint">
                    Bound how much sender context is rendered into the letter compose
                    prompt. Lower these if your LLM's context window is tight; 0 renders
                    an empty section.
                </p>
                <div className="settings-row">
                    <span className="settings-row-label">Memories</span>
                    <LiveSlider
                        value={s.letter_compose_memory_render_cap}
                        min={3}
                        max={20}
                        step={1}
                        unit=""
                        onCommit={onCommitLetterMemoryCap}
                    />
                </div>
                <div className="settings-row">
                    <span className="settings-row-label">Recent Dialogue Lines</span>
                    <LiveSlider
                        value={s.letter_compose_dialogue_render_cap}
                        min={5}
                        max={50}
                        step={1}
                        unit=""
                        onCommit={onCommitLetterDialogueCap}
                    />
                </div>
            </section>
        </div>
    );
}
