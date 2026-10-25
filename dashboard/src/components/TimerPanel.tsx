import type { ReactNode } from 'react';

import { t } from '../i18n/strings';
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

// Each clock is labelled by its `clock.<id>` string and explained by its
// `clock.<id>.title` one.
const clockLabel = (clock: TimerClock): string => t(`clock.${clock}`);
const clockTitle = (clock: TimerClock): string => t(`clock.${clock}.title`);

function formatSeconds(seconds: number): string {
    if (seconds < 1) return t('time.now');
    if (seconds < 60) return t('time.seconds', { s: Math.floor(seconds) });
    const m = Math.floor(seconds / 60);
    const s = Math.floor(seconds % 60);
    if (m < 60) return s > 0 ? t('time.minutes_seconds', { m, s }) : t('time.minutes', { m });
    const h = Math.floor(m / 60);
    return t('time.hours_minutes', { h, m: m % 60 });
}

function formatGameHours(hours: number): string {
    if (hours < 1 / 60) return t('time.now');
    if (hours < 1) return t('time.minutes', { m: Math.round(hours * 60) });
    if (hours < 24) {
        const h = Math.floor(hours);
        const m = Math.round((hours - h) * 60);
        return m > 0 ? t('time.hours_minutes', { h, m }) : t('time.hours', { h });
    }
    const d = Math.floor(hours / 24);
    const h = Math.round(hours % 24);
    return h > 0 ? t('time.days_hours', { d, h }) : t('time.days', { d });
}

export function formatTimer(timer: MaybeTimer): string {
    if (!timer) return '—';
    return timer.clock === 'game_time' ? formatGameHours(timer.remaining) : formatSeconds(timer.remaining);
}

export function TimerValue({ timer }: { timer: MaybeTimer }): ReactNode {
    if (!timer) {
        // Deliberately not "0s". See the header note.
        return (
            <span className="timer-value timer-inactive" title={t('timer.inactive')}>
                —
            </span>
        );
    }
    return (
        <span className="timer-value">
            {formatTimer(timer)}
            <span className="timer-clock" title={clockTitle(timer.clock)}>
                {clockLabel(timer.clock)}
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
            <div className="timer-asof">{t('timer.as_of')}</div>
        </section>
    );
}
