import { useState } from 'react';
import { label, t } from '../../i18n/strings';
import type { DirectorState } from '../../types';
import { ActionDispatchTable } from '../ActionDispatchTable';
import { TimerPanel, TimerRow } from '../TimerPanel';

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
                <h3>{t('dispatch.abort_confirm', { beat: label('beat.', beatName) })}</h3>
                <p>{t('dispatch.abort_confirm.body')}</p>
                <div className="settings-rebind-buttons">
                    <button type="button" className="bulk-button" onClick={onCancel}>
                        {t('common.cancel')}
                    </button>
                    <button type="button" className="bulk-button bulk-button-danger" onClick={onConfirm}>
                        {t('dispatch.confirm_abort')}
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
    // The two gates that apply to the table as a whole, as opposed to the
    // per-beat cooldown in its own column.
    const gates = state.timers.director;

    return (
        <div className="tab-content dispatch-tab">
            {confirmOpen && inFlightName && (
                <AbortConfirmModal
                    beatName={inFlightName}
                    onConfirm={onConfirmAbort}
                    onCancel={onCancelAbort}
                />
            )}
            <TimerPanel title={t('dispatch.gates')}>
                <TimerRow label={t('timer.global_beat_cooldown')} timer={gates.global_beat_cooldown} />
                <TimerRow
                    label={t('timer.repetition_window')}
                    timer={gates.repetition_window.soonest}
                    note={
                        gates.repetition_window.suppressed_count > 0
                            ? t('timer.suppressed', { n: gates.repetition_window.suppressed_count })
                            : t('timer.nothing_suppressed')
                    }
                />
            </TimerPanel>
            <section className="panel dispatch-controls">
                <h2>{t('dispatch.controls')}</h2>
                <label className="tick-toggle" title={t('dispatch.tick_enabled.title')}>
                    <input
                        type="checkbox"
                        checked={state.status.tick_enabled}
                        onChange={onToggleTick}
                    />
                    <span>{t('dispatch.tick_enabled')}</span>
                </label>
                <button
                    type="button"
                    className="bulk-button bulk-button-danger"
                    disabled={!inFlightName}
                    onClick={onAbortClick}
                    title={inFlightName
                        ? t('dispatch.abort.title', { beat: label('beat.', inFlightName) })
                        : t('dispatch.abort.none')}
                >
                    {t('dispatch.abort')}
                </button>
            </section>
            <section className="panel action-dispatch">
                <h2>{t('dispatch.actions')}</h2>
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
                        {t('dispatch.enable_all')}
                    </button>
                    <button
                        type="button"
                        className="bulk-button"
                        onClick={() => onBulk(false)}
                    >
                        {t('dispatch.disable_all')}
                    </button>
                </div>
            </section>
        </div>
    );
}
