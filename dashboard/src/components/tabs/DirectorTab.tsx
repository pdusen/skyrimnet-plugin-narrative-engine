import type { DirectorState } from '../../types';
import { TimerPanel, TimerRow } from '../TimerPanel';
import { DecisionList } from '../DecisionList';
import { EventList } from '../EventList';
import { LastEvaluation } from '../LastEvaluation';
import { PhasePanel } from '../PhasePanel';

interface Props {
    state: DirectorState;
}

export function DirectorTab({ state }: Props) {
    const t = state.timers.director;
    return (
        <div className="tab-content director-tab">
            <PhasePanel
                phase={state.current_phase}
                timeInPhaseSeconds={state.time_in_phase_seconds}
                actionInFlight={state.action_in_flight}
            />
            <TimerPanel title="Next up">
                <TimerRow label="Next evaluation" timer={t.next_evaluation} />
                <TimerRow label="Phase may advance in" timer={t.phase_advance_unlock} />
                <TimerRow label="Global beat cooldown" timer={t.global_beat_cooldown} />
                <TimerRow
                    label="Repetition window"
                    timer={t.repetition_window.soonest}
                    note={
                        t.repetition_window.suppressed_count > 0
                            ? `${t.repetition_window.suppressed_count} beat${
                                  t.repetition_window.suppressed_count === 1 ? '' : 's'
                              } suppressed`
                            : 'nothing suppressed'
                    }
                />
            </TimerPanel>
            <LastEvaluation evaluation={state.last_evaluation} />
            <DecisionList items={state.recent_decisions} />
            <EventList items={state.recent_events} />
        </div>
    );
}
