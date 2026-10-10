import type { ReactNode } from 'react';
import type { MaybeTimer, TimerClock } from '../types';

// Shared rendering for the per-tab timing readouts.
//
// Two things this component exists to get right, both of which are easy to
// get wrong at each call site if every tab formats its own:
//
//   1. The clock is part of the value. "120s" against the global beat
//      cooldown and "120s" against the repetition window are durations that
//      elapse at different rates — one is frozen in combat and dialogue,
//      the other is not. Every row is labelled with which clock it is on.
//   2. null is not zero. A cooldown configured off never blocks anything;
//      an expired one just stopped blocking. Rendering both as "0s" would
//      answer "why is this beat not firing?" wrongly.
//
// Nothing here animates. The dashboard is only ever on screen with the game
// paused, so three of the four clocks cannot advance while it is open; see
// the TimersState comment in types.ts.

const CLOCK_LABEL: Record<TimerClock, string> = {
    wall_clock: 'real',
    unpaused_real: 'unpaused',
    active_play_real: 'active play',
    game_time: 'game time',
};

const CLOCK_TITLE: Record<TimerClock, string> = {
    wall_clock: 'Wall-clock time. Runs through pause, combat, dialogue and menus.',
    unpaused_real: 'Real time while unpaused. Frozen whenever the game is paused — including now.',
    active_play_real:
        'Real time during active play only. Frozen while paused, in combat, and in dialogue — including now.',
    game_time: 'In-world time. Frozen while the game is paused — including now.',
};

function formatSeconds(seconds: number): string {
    if (seconds < 1) return 'now';
    if (seconds < 60) return `${Math.floor(seconds)}s`;
    const m = Math.floor(seconds / 60);
    const s = Math.floor(seconds % 60);
    if (m < 60) return s > 0 ? `${m}m ${s}s` : `${m}m`;
    const h = Math.floor(m / 60);
    return `${h}h ${m % 60}m`;
}

function formatGameHours(hours: number): string {
    if (hours < 1 / 60) return 'now';
    if (hours < 1) return `${Math.round(hours * 60)}m`;
    if (hours < 24) {
        const h = Math.floor(hours);
        const m = Math.round((hours - h) * 60);
        return m > 0 ? `${h}h ${m}m` : `${h}h`;
    }
    const d = Math.floor(hours / 24);
    const h = Math.round(hours % 24);
    return h > 0 ? `${d}d ${h}h` : `${d}d`;
}

export function formatTimer(timer: MaybeTimer): string {
    if (!timer) return '—';
    return timer.clock === 'game_time' ? formatGameHours(timer.remaining) : formatSeconds(timer.remaining);
}

export function TimerValue({ timer }: { timer: MaybeTimer }): ReactNode {
    if (!timer) {
        // Deliberately not "0s". See the header note.
        return <span className="timer-value timer-inactive" title="Not scheduled, or configured off">—</span>;
    }
    return (
        <span className="timer-value">
            {formatTimer(timer)}
            <span className="timer-clock" title={CLOCK_TITLE[timer.clock]}>
                {CLOCK_LABEL[timer.clock]}
            </span>
        </span>
    );
}

export function TimerRow({ label, timer, note }: { label: string; timer: MaybeTimer; note?: string }): ReactNode {
    return (
        <div className="timer-row">
            <span className="timer-label">{label}</span>
            <TimerValue timer={timer} />
            {note && <span className="timer-note">{note}</span>}
        </div>
    );
}

// The panel wrapper every tab uses, carrying the "as of" caveat once
// rather than per row. Without it a reader watching a number that never
// moves has no way to tell a frozen clock from a broken one.
export function TimerPanel({ title, children }: { title: string; children: ReactNode }): ReactNode {
    return (
        <section className="panel timer-panel">
            <h2>{title}</h2>
            {children}
            <div className="timer-asof">as of when the dashboard was opened — the game is paused while it is</div>
        </section>
    );
}
