import { useEffect, useRef, useState } from 'react';

import { label, t } from '../i18n/strings';
import type { ActionInfo } from '../types';

interface Props {
    actions: ActionInfo[];
    inFlightName: string | null;
    nowSeconds: number;
}

// Safety valve: how long to hold the "pending dispatch" disable if the
// server never reports an in-flight action after a click. Long enough
// to cover a slow beat-select LLM call, short enough that a silent
// failure (unknown beat, blocked by global precondition, LLM error) is
// recoverable without a page reload.
const PENDING_DISPATCH_TIMEOUT_MS = 30000;

declare global {
    interface Window {
        ne_setActionEnabled?: (arg: string) => void;
        ne_dispatchAction?:   (arg: string) => void;
    }
}

function formatRelative(deltaSeconds: number): string {
    if (deltaSeconds < 1) return t('time.just_now');
    const ago = (time: string) => t('time.ago', { time });
    if (deltaSeconds < 60) return ago(t('time.seconds', { s: Math.round(deltaSeconds) }));
    if (deltaSeconds < 3600) return ago(t('time.minutes', { m: Math.round(deltaSeconds / 60) }));
    if (deltaSeconds < 86400) return ago(t('time.hours', { h: Math.round(deltaSeconds / 3600) }));
    return ago(t('time.days', { d: Math.round(deltaSeconds / 86400) }));
}

function formatCooldownHours(hours: number): string {
    if (hours <= 0) return '—';
    if (hours < 1) return t('time.minutes', { m: Math.round(hours * 60) });
    if (hours < 24) return t('time.hours', { h: hours.toFixed(1) });
    return t('time.days', { d: (hours / 24).toFixed(1) });
}

export function ActionDispatchTable({ actions, inFlightName, nowSeconds }: Props) {
    const anyInFlight = inFlightName !== null && inFlightName !== '';

    // Client-only "pending dispatch" flag. Held between click and the
    // first server-state push that reports a non-null in-flight action.
    // Bridges the beat-select-LLM window: ForceDispatchBeat kicks off
    // asynchronously and doesn't set g_topLevelState=BEAT_RUNNING until
    // the LLM callback lands, so the initial PushFullState after the
    // click still carries a null action_in_flight. Without this flag,
    // the buttons re-enable during that gap and the player could stack
    // dispatch clicks.
    const [pendingDispatchName, setPendingDispatchName] = useState<string | null>(null);
    const pendingTimeoutRef = useRef<number | null>(null);

    const clearPendingTimeout = () => {
        if (pendingTimeoutRef.current !== null) {
            window.clearTimeout(pendingTimeoutRef.current);
            pendingTimeoutRef.current = null;
        }
    };

    // Hand off from client-pending to server-in-flight the moment the
    // server reports any action in flight. From then on, the disable is
    // driven by anyInFlight (which clears when the beat completes or
    // rolls back — the whole "until it completes or cancels" window is
    // covered by that server-side signal).
    useEffect(() => {
        if (pendingDispatchName !== null && anyInFlight) {
            setPendingDispatchName(null);
            clearPendingTimeout();
        }
    }, [pendingDispatchName, anyInFlight]);

    // Cancel a stuck timeout on unmount so the setState below can't
    // fire against a torn-down component.
    useEffect(() => clearPendingTimeout, []);

    const onToggleEnabled = (name: string, next: boolean) => {
        window.ne_setActionEnabled?.(JSON.stringify({ name, enabled: next }));
    };
    const onDispatch = (name: string) => {
        setPendingDispatchName(name);
        clearPendingTimeout();
        pendingTimeoutRef.current = window.setTimeout(() => {
            // Safety valve — the dispatch presumably failed before ever
            // populating an in-flight record (unknown beat, refused by
            // a global precondition, LLM timeout). Release the disable
            // so the player isn't stuck.
            setPendingDispatchName(null);
            pendingTimeoutRef.current = null;
        }, PENDING_DISPATCH_TIMEOUT_MS);
        window.ne_dispatchAction?.(name);
    };

    return (
        <table className="action-table">
            <thead>
                <tr>
                    <th className="col-name">{t('dispatch.column.action')}</th>
                    <th className="col-enabled">{t('dispatch.column.enabled')}</th>
                    <th className="col-last">{t('dispatch.column.last')}</th>
                    <th className="col-cooldown" title={t('dispatch.column.cooldown.title')}>
                        {t('dispatch.column.cooldown')} <span className="timer-clock">{t('clock.game_time')}</span>
                    </th>
                    <th className="col-fire">{t('dispatch.column.dispatch')}</th>
                </tr>
            </thead>
            <tbody>
                {actions.map(a => {
                    const last = a.last_dispatched_at > 0
                        ? formatRelative(nowSeconds - a.last_dispatched_at)
                        : t('dispatch.never');
                    // Dispatch button is disabled while any action is in
                    // flight (single-flight lock) OR while a click is
                    // pending — the beat-select LLM window between click
                    // and in-flight populating. Cooldown state does NOT
                    // gate the button — force-dispatch bypasses cooldowns
                    // by design.
                    const disableDispatch = anyInFlight || pendingDispatchName !== null;
                    const disableReason = anyInFlight
                        ? t('dispatch.busy', { beat: label('beat.', inFlightName ?? '') })
                        : pendingDispatchName !== null
                            ? t('dispatch.dispatching', { beat: label('beat.', pendingDispatchName) })
                            : null;
                    return (
                        <tr key={a.name}>
                            <td className="col-name">{label('beat.', a.name)}</td>
                            <td className="col-enabled">
                                <input
                                    type="checkbox"
                                    checked={a.enabled}
                                    onChange={e => onToggleEnabled(a.name, e.target.checked)}
                                />
                            </td>
                            <td className="col-last">{last}</td>
                            <td className="col-cooldown">
                                {formatCooldownHours(a.remaining_cooldown_hours)}
                            </td>
                            <td className="col-fire">
                                <button
                                    type="button"
                                    className="dispatch-button"
                                    disabled={disableDispatch}
                                    onClick={() => onDispatch(a.name)}
                                    title={disableReason ?? t('dispatch.force.title')}
                                >
                                    {t('dispatch.column.dispatch')}
                                </button>
                            </td>
                        </tr>
                    );
                })}
            </tbody>
        </table>
    );
}
