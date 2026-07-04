import type { ActionInfo } from '../types';

interface Props {
    actions: ActionInfo[];
    inFlightName: string | null;
    nowSeconds: number;
}

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

    const onToggleEnabled = (name: string, next: boolean) => {
        window.ne_setActionEnabled?.(JSON.stringify({ name, enabled: next }));
    };
    const onDispatch = (name: string) => {
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
                    // Dispatch button is disabled only while any action
                    // is in-flight (single-flight lock). Cooldown state
                    // does NOT gate the button — force-dispatch bypasses
                    // cooldowns by design.
                    const disableDispatch = anyInFlight;
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
                                    title={disableDispatch
                                        ? `Another action is in flight (${inFlightName})`
                                        : 'Force-dispatch this action, bypassing cooldowns'}
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
