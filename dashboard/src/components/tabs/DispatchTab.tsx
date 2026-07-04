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
    }
}

export function DispatchTab({ state, nowSeconds }: Props) {
    const inFlightName = state.action_in_flight?.name ?? null;

    const onToggleTick = (e: React.ChangeEvent<HTMLInputElement>) => {
        window.ne_setTickEnabled?.(e.target.checked ? 'true' : 'false');
    };
    const onBulk = (enabled: boolean) => {
        window.ne_setAllActionsEnabled?.(enabled ? 'true' : 'false');
    };

    return (
        <div className="tab-content dispatch-tab">
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
