import type { DirectorState } from '../../types';
import { DecisionList } from '../DecisionList';
import { EventList } from '../EventList';
import { LastEvaluation } from '../LastEvaluation';
import { PhasePanel } from '../PhasePanel';

interface Props {
    state: DirectorState;
}

export function DirectorTab({ state }: Props) {
    return (
        <div className="tab-content director-tab">
            <PhasePanel
                phase={state.current_phase}
                timeInPhaseSeconds={state.time_in_phase_seconds}
                actionInFlight={state.action_in_flight}
            />
            <LastEvaluation evaluation={state.last_evaluation} />
            <DecisionList items={state.recent_decisions} />
            <EventList items={state.recent_events} />
        </div>
    );
}
