import type { DirectorState } from '../types';

interface Props {
    phase: string;
    timeInPhaseSeconds: number;
    actionInFlight: DirectorState['action_in_flight'];
}

function formatDuration(seconds: number): string {
    if (seconds < 60) return `${Math.floor(seconds)}s`;
    const m = Math.floor(seconds / 60);
    const s = Math.floor(seconds % 60);
    if (m < 60) return `${m}m ${s}s`;
    const h = Math.floor(m / 60);
    return `${h}h ${m % 60}m`;
}

export function PhasePanel({ phase, timeInPhaseSeconds, actionInFlight }: Props) {
    // started_at is Unix-epoch seconds; Date.now() is ms.
    const ageSeconds = actionInFlight
        ? Math.max(0, (Date.now() / 1000) - actionInFlight.started_at)
        : 0;
    return (
        <section className="panel phase-panel">
            <h2>Phase</h2>
            <div className="phase-name">{phase}</div>
            <div className="phase-time">in phase for {formatDuration(timeInPhaseSeconds)}</div>
            {actionInFlight && (
                <div className="action-in-flight">
                    action in flight: {actionInFlight.name} (started {formatDuration(ageSeconds)} ago)
                </div>
            )}
        </section>
    );
}
