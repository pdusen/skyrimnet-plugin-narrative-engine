import { label, t } from '../i18n/strings';
import type { DirectorState, PhaseName } from '../types';

interface Props {
    phase: PhaseName;
    timeInPhaseSeconds: number;
    actionInFlight: DirectorState['action_in_flight'];
}

function formatDuration(seconds: number): string {
    if (seconds < 60) return t('time.seconds', { s: Math.floor(seconds) });
    const m = Math.floor(seconds / 60);
    const s = Math.floor(seconds % 60);
    if (m < 60) return t('time.minutes_seconds', { m, s });
    const h = Math.floor(m / 60);
    return t('time.hours_minutes', { h, m: m % 60 });
}

export function PhasePanel({ phase, timeInPhaseSeconds, actionInFlight }: Props) {
    // started_at is Unix-epoch seconds; Date.now() is ms.
    const ageSeconds = actionInFlight
        ? Math.max(0, (Date.now() / 1000) - actionInFlight.started_at)
        : 0;
    return (
        <section className="panel phase-panel">
            <h2>{t('director.phase')}</h2>
            <div className="phase-name">{label('phase.', phase)}</div>
            <div className="phase-time">{t('director.in_phase_for', { time: formatDuration(timeInPhaseSeconds) })}</div>
            {actionInFlight && (
                <div className="action-in-flight">
                    {t('director.action_in_flight', {
                        beat: label('beat.', actionInFlight.name),
                        ago: t('time.ago', { time: formatDuration(ageSeconds) }),
                    })}
                </div>
            )}
        </section>
    );
}
