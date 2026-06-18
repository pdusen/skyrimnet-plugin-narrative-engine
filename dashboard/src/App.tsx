import { useEffect, useState } from 'react';

import { DecisionList } from './components/DecisionList';
import { EventList } from './components/EventList';
import { LastEvaluation } from './components/LastEvaluation';
import { PhasePanel } from './components/PhasePanel';
import { StatusBanner } from './components/StatusBanner';
import { stateStore, type Snapshot } from './stateStore';

export function App() {
    const [snap, setSnap] = useState<Snapshot>(() => stateStore.getSnapshot());

    useEffect(() => {
        // Re-sync once on mount in case set() fired between initial render
        // and effect, then subscribe for the lifetime of the component.
        setSnap(stateStore.getSnapshot());
        return stateStore.subscribe(() => setSnap(stateStore.getSnapshot()));
    }, []);

    if (snap.error) {
        return <div className="dashboard error">{snap.error}</div>;
    }
    if (!snap.state) {
        return <div className="dashboard placeholder">Awaiting first Director evaluation…</div>;
    }

    const s = snap.state;
    return (
        <div className="dashboard">
            <StatusBanner status={s.status} />
            <PhasePanel phase={s.current_phase} timeInPhaseSeconds={s.time_in_phase_seconds} />
            <LastEvaluation evaluation={s.last_evaluation} />
            <DecisionList items={s.recent_decisions} />
            <EventList items={s.recent_events} />
        </div>
    );
}
