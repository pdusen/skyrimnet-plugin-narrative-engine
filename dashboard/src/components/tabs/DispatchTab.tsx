import { useState } from 'react';
import type { DirectorState } from '../../types';
import { ActionDispatchTable } from '../ActionDispatchTable';

interface Props {
    state: DirectorState;
    nowSeconds: number;
}

declare global {
    interface Window {
        ne_setTickEnabled?:       (arg: string) => void;
        ne_setAllActionsEnabled?: (arg: string) => void;
        ne_abortRunningBeat?:     (arg: string) => void;
    }
}

function AbortConfirmModal({ beatName, onConfirm, onCancel }: {
    beatName: string;
    onConfirm: () => void;
    onCancel: () => void;
}) {
    return (
        <div className="settings-rebind-overlay" role="dialog" aria-modal="true">
            <div className="settings-rebind-panel">
                <h3>Abort narrative beat “{beatName}”?</h3>
                <p>
                    The in-progress beat will be stopped and reset immediately. Any
                    memories that SkyrimNet has already recorded and any narrations that
                    NPCs have already spoken will remain — only the running beat itself
                    is unwound.
                </p>
                <div className="settings-rebind-buttons">
                    <button type="button" className="bulk-button" onClick={onCancel}>
                        Cancel
                    </button>
                    <button type="button" className="bulk-button bulk-button-danger" onClick={onConfirm}>
                        Confirm Abort
                    </button>
                </div>
            </div>
        </div>
    );
}

export function DispatchTab({ state, nowSeconds }: Props) {
    const inFlightName = state.action_in_flight?.name ?? null;
    const [confirmOpen, setConfirmOpen] = useState(false);

    const onToggleTick = (e: React.ChangeEvent<HTMLInputElement>) => {
        window.ne_setTickEnabled?.(e.target.checked ? 'true' : 'false');
    };
    const onBulk = (enabled: boolean) => {
        window.ne_setAllActionsEnabled?.(enabled ? 'true' : 'false');
    };
    const onAbortClick = () => {
        if (inFlightName) setConfirmOpen(true);
    };
    const onConfirmAbort = () => {
        window.ne_abortRunningBeat?.('');
        setConfirmOpen(false);
    };
    const onCancelAbort = () => setConfirmOpen(false);

    return (
        <div className="tab-content dispatch-tab">
            {confirmOpen && inFlightName && (
                <AbortConfirmModal
                    beatName={inFlightName}
                    onConfirm={onConfirmAbort}
                    onCancel={onCancelAbort}
                />
            )}
            <section className="panel dispatch-controls">
                <h2>Controls</h2>
                <label className="tick-toggle" title="When off, the plugin skips its periodic poll: no phase advance, no evaluation, no dispatcher tick. Debug aid.">
                    <input
                        type="checkbox"
                        checked={state.status.tick_enabled}
                        onChange={onToggleTick}
                    />
                    <span>Tick Enabled</span>
                </label>
                <button
                    type="button"
                    className="bulk-button bulk-button-danger"
                    disabled={!inFlightName}
                    onClick={onAbortClick}
                    title={inFlightName
                        ? `Abort the running beat: ${inFlightName}`
                        : 'No narrative beat is currently in flight.'}
                >
                    Abort Running Beat
                </button>
            </section>
            <section className="panel action-dispatch">
                <h2>Actions</h2>
                <ActionDispatchTable
                    actions={state.actions}
                    inFlightName={inFlightName}
                    nowSeconds={nowSeconds}
                />
                <div className="bulk-buttons">
                    <button
                        type="button"
                        className="bulk-button"
                        onClick={() => onBulk(true)}
                    >
                        Enable All
                    </button>
                    <button
                        type="button"
                        className="bulk-button"
                        onClick={() => onBulk(false)}
                    >
                        Disable All
                    </button>
                </div>
            </section>
        </div>
    );
}
