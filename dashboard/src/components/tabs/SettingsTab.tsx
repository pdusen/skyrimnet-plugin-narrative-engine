import { useState } from 'react';

import { t as tr } from '../../i18n/strings';
import type { DirectorState, Hotkey, PhaseName } from '../../types';

interface Props {
    state: DirectorState;
}

declare global {
    interface Window {
        ne_setDebugMode?: (arg: string) => void;
        ne_setDebugNotifications?: (arg: string) => void;
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
        ne_setStoryEvalEventTailSize?: (arg: string) => void;
        ne_setStoryEvalDecisionLogTailSize?: (arg: string) => void;
        ne_setVisitComposeMemoryCap?: (arg: string) => void;
        ne_setVisitComposeDialogueCap?: (arg: string) => void;
        ne_setVisitConclusionPollRecentLinesCap?: (arg: string) => void;
    }
}

// Per-phase slider config. Ranges match the clamps in
// DashboardUIManager::OnSetPhaseIdealDuration — if either side changes,
// both must stay in sync.
// Each is labelled by its phase's `phase.<id>` string.
const PHASE_SLIDERS: Array<{
    key: keyof DirectorState['settings']['ideal_duration_seconds'];
    phase: PhaseName;
    min: number;
    max: number;
}> = [
    { key: 'exposition', phase: 'Exposition', min: 60, max: 1200 },
    { key: 'rising_action', phase: 'RisingAction', min: 60, max: 1200 },
    { key: 'climax', phase: 'Climax', min: 30, max: 600 },
    { key: 'falling_action', phase: 'FallingAction', min: 60, max: 1200 },
    { key: 'resolution', phase: 'Resolution', min: 60, max: 1200 },
];

// The binding as the language says it: each modifier, then the key, joined
// with "+". A key with a legend of its own shows it ("F7"); a named key
// takes its `key.<name>` string; anything else shows its scan code.
function hotkeyText(hotkey: Hotkey | null): string {
    if (!hotkey) {
        return tr('key.none');
    }
    const parts: string[] = [];
    if (hotkey.ctrl) parts.push(tr('key.ctrl'));
    if (hotkey.shift) parts.push(tr('key.shift'));
    if (hotkey.alt) parts.push(tr('key.alt'));
    if (hotkey.named) {
        parts.push(tr(`key.${hotkey.key}`));
    } else {
        parts.push(hotkey.key || tr('key.code', { code: hotkey.code }));
    }
    return parts.join('+');
}

// Live-update slider. Displays the current-drag value locally (via
// oninput) so the label tracks the thumb, but only fires the C++
// listener on release (via onchange) — one INI write per drag, not one
// per pixel of drag distance.
function LiveSlider({
    value,
    min,
    max,
    step,
    seconds,
    onCommit,
}: {
    value: number;
    min: number;
    max: number;
    step: number;
    // Whether the value is a number of seconds, and says so; else a count.
    seconds: boolean;
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
            <span className="settings-slider-value">{seconds ? tr('time.seconds', { s: display }) : display}</span>
        </div>
    );
}

function HotkeyRebindModal() {
    const onCancel = () => window.ne_cancelHotkeyRebind?.('');
    return (
        <div className="settings-rebind-overlay" role="dialog" aria-modal="true">
            <div className="settings-rebind-panel">
                <h3>{tr('settings.rebind_title')}</h3>
                <p>{tr('settings.rebind_body')}</p>
                <button type="button" className="bulk-button" onClick={onCancel}>
                    {tr('common.cancel')}
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
    const onToggleDebugNotifications = (e: React.ChangeEvent<HTMLInputElement>) => {
        window.ne_setDebugNotifications?.(e.target.checked ? 'true' : 'false');
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
    const onCommitStoryEvalEventTail = (v: number) => window.ne_setStoryEvalEventTailSize?.(String(v));
    const onCommitStoryEvalDecisionLogTail = (v: number) => window.ne_setStoryEvalDecisionLogTailSize?.(String(v));
    const onCommitVisitMemoryCap = (v: number) => window.ne_setVisitComposeMemoryCap?.(String(v));
    const onCommitVisitDialogueCap = (v: number) => window.ne_setVisitComposeDialogueCap?.(String(v));
    const onCommitVisitConclusionPollRecentLinesCap = (v: number) =>
        window.ne_setVisitConclusionPollRecentLinesCap?.(String(v));

    return (
        <div className="tab-content settings-tab">
            {s.dashboard_hotkey_capture_active && <HotkeyRebindModal />}

            <section className="panel">
                <h2>{tr('settings.general')}</h2>
                <label className="tick-toggle">
                    <input type="checkbox" checked={s.debug_mode} onChange={onToggleDebug} />
                    <span>{tr('settings.debug_mode')}</span>
                </label>
                <label className="tick-toggle">
                    <input
                        type="checkbox"
                        checked={s.debug_notifications}
                        onChange={onToggleDebugNotifications}
                    />
                    <span>{tr('settings.debug_notifications')}</span>
                </label>
                <div className="settings-row">
                    <span className="settings-row-label">{tr('settings.hotkey')}</span>
                    <span className="settings-hotkey-display">{hotkeyText(s.dashboard_hotkey)}</span>
                    <button type="button" className="bulk-button" onClick={onRebind}>
                        {tr('settings.rebind')}
                    </button>
                </div>
            </section>

            <section className="panel">
                <h2>{tr('settings.director')}</h2>
                <label className="tick-toggle">
                    <input type="checkbox" checked={s.tick_enabled} onChange={onToggleTick} />
                    <span>{tr('settings.tick_enabled')}</span>
                </label>
                <div className="settings-row">
                    <span className="settings-row-label">{tr('settings.tick_interval')}</span>
                    <LiveSlider
                        value={s.tick_interval_seconds}
                        min={10}
                        max={600}
                        step={5}
                        seconds
                        onCommit={onCommitTickInterval}
                    />
                </div>
                <div className="settings-row">
                    <span className="settings-row-label">{tr('settings.min_phase_duration')}</span>
                    <LiveSlider
                        value={s.min_phase_duration_seconds}
                        min={0}
                        max={600}
                        step={5}
                        seconds
                        onCommit={onCommitMinPhaseDuration}
                    />
                </div>
            </section>

            <section className="panel">
                <h2>{tr('settings.phase_durations')}</h2>
                {PHASE_SLIDERS.map(p => (
                    <div key={p.key} className="settings-row">
                        <span className="settings-row-label">{tr(`phase.${p.phase}`)}</span>
                        <LiveSlider
                            value={s.ideal_duration_seconds[p.key]}
                            min={p.min}
                            max={p.max}
                            step={5}
                            seconds
                            onCommit={v => onCommitPhase(p.key, v)}
                        />
                    </div>
                ))}
            </section>

            <section className="panel">
                <h2>{tr('settings.story_eval_caps')}</h2>
                <p className="settings-panel-hint">{tr('settings.story_eval_caps.hint')}</p>
                <div className="settings-row">
                    <span className="settings-row-label">{tr('settings.recent_events')}</span>
                    <LiveSlider
                        value={s.story_eval_event_tail_size}
                        min={5}
                        max={100}
                        step={1}
                        seconds={false}
                        onCommit={onCommitStoryEvalEventTail}
                    />
                </div>
                <div className="settings-row">
                    <span className="settings-row-label">{tr('settings.decision_log_tail')}</span>
                    <LiveSlider
                        value={s.story_eval_decision_log_tail_size}
                        min={3}
                        max={30}
                        step={1}
                        seconds={false}
                        onCommit={onCommitStoryEvalDecisionLogTail}
                    />
                </div>
            </section>

            <section className="panel">
                <h2>{tr('settings.action_select_caps')}</h2>
                <p className="settings-panel-hint">{tr('settings.action_select_caps.hint')}</p>
                <div className="settings-row">
                    <span className="settings-row-label">{tr('settings.recent_events')}</span>
                    <LiveSlider
                        value={s.action_select_event_render_cap}
                        min={3}
                        max={30}
                        step={1}
                        seconds={false}
                        onCommit={onCommitActionSelectEventCap}
                    />
                </div>
                <div className="settings-row">
                    <span className="settings-row-label">{tr('settings.letter_sender_memories')}</span>
                    <LiveSlider
                        value={s.action_select_letter_memory_render_cap}
                        min={3}
                        max={15}
                        step={1}
                        seconds={false}
                        onCommit={onCommitActionSelectLetterMemoryCap}
                    />
                </div>
                <div className="settings-row">
                    <span className="settings-row-label">{tr('settings.visit_sender_memories')}</span>
                    <LiveSlider
                        value={s.action_select_visit_memory_render_cap}
                        min={3}
                        max={15}
                        step={1}
                        seconds={false}
                        onCommit={onCommitActionSelectVisitMemoryCap}
                    />
                </div>
            </section>

            <section className="panel">
                <h2>{tr('settings.letter_compose_caps')}</h2>
                <p className="settings-panel-hint">{tr('settings.letter_compose_caps.hint')}</p>
                <div className="settings-row">
                    <span className="settings-row-label">{tr('settings.memories')}</span>
                    <LiveSlider
                        value={s.letter_compose_memory_render_cap}
                        min={3}
                        max={20}
                        step={1}
                        seconds={false}
                        onCommit={onCommitLetterMemoryCap}
                    />
                </div>
                <div className="settings-row">
                    <span className="settings-row-label">{tr('settings.recent_dialogue_lines')}</span>
                    <LiveSlider
                        value={s.letter_compose_dialogue_render_cap}
                        min={5}
                        max={50}
                        step={1}
                        seconds={false}
                        onCommit={onCommitLetterDialogueCap}
                    />
                </div>
            </section>

            <section className="panel">
                <h2>{tr('settings.visit_compose_caps')}</h2>
                <p className="settings-panel-hint">{tr('settings.visit_compose_caps.hint')}</p>
                <div className="settings-row">
                    <span className="settings-row-label">{tr('settings.memories')}</span>
                    <LiveSlider
                        value={s.visit_compose_memory_render_cap}
                        min={3}
                        max={20}
                        step={1}
                        seconds={false}
                        onCommit={onCommitVisitMemoryCap}
                    />
                </div>
                <div className="settings-row">
                    <span className="settings-row-label">{tr('settings.recent_dialogue_lines')}</span>
                    <LiveSlider
                        value={s.visit_compose_dialogue_render_cap}
                        min={5}
                        max={50}
                        step={1}
                        seconds={false}
                        onCommit={onCommitVisitDialogueCap}
                    />
                </div>
                <div className="settings-row">
                    <span className="settings-row-label">{tr('settings.conclusion_poll_lines')}</span>
                    <LiveSlider
                        value={s.visit_conclusion_poll_recent_lines_render_cap}
                        min={3}
                        max={20}
                        step={1}
                        seconds={false}
                        onCommit={onCommitVisitConclusionPollRecentLinesCap}
                    />
                </div>
            </section>
        </div>
    );
}
