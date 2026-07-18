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
        </div>
    );
}
