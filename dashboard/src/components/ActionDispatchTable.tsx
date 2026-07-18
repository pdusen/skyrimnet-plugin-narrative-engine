import { useEffect, useRef, useState } from 'react';

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
    if (deltaSeconds < 1)     return 'just now';
    if (deltaSeconds < 60)    return `${Math.round(deltaSeconds)}s ago`;
    if (deltaSeconds < 3600)  return `${Math.round(deltaSeconds / 60)}m ago`;
    if (deltaSeconds < 86400) return `${Math.round(deltaSeconds / 3600)}h ago`;
    return `${Math.round(deltaSeconds / 86400)}d ago`;
}

function formatCooldownHours(hours: number): string {
    if (hours <= 0) return '—';
    if (hours < 1)  return `${Math.round(hours * 60)}m`;
    if (hours < 24) return `${hours.toFixed(1)}h`;
    return `${(hours / 24).toFixed(1)}d`;
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
                    <th className="col-name">Action</th>
                    <th className="col-enabled">Enabled</th>
                    <th className="col-last">Last Dispatched</th>
                    <th className="col-cooldown">Cooldown</th>
                    <th className="col-fire">Dispatch</th>
                </tr>
            </thead>
            <tbody>
                {actions.map(a => {
                    const last = a.last_dispatched_at > 0
                        ? formatRelative(nowSeconds - a.last_dispatched_at)
                        : 'never';
                    // Dispatch button is disabled while any action is in
                    // flight (single-flight lock) OR while a click is
                    // pending — the beat-select LLM window between click
                    // and in-flight populating. Cooldown state does NOT
                    // gate the button — force-dispatch bypasses cooldowns
                    // by design.
                    const disableDispatch = anyInFlight || pendingDispatchName !== null;
                    const disableReason = anyInFlight
                        ? `Another action is in flight (${inFlightName})`
                        : pendingDispatchName !== null
                            ? `Dispatching ${pendingDispatchName}…`
                            : null;
                    return (
                        <tr key={a.name}>
                            <td className="col-name">{a.name}</td>
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
                                    title={disableReason ?? 'Force-dispatch this action, bypassing cooldowns'}
                                >
                                    Dispatch
                                </button>
                            </td>
                        </tr>
                    );
                })}
            </tbody>
        </table>
    );
}
