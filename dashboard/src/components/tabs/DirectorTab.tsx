import { t as tr } from '../../i18n/strings';
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
            <TimerPanel title={tr('timer.next_up')}>
                <TimerRow label={tr('director.next_evaluation')} timer={t.next_evaluation} />
                <TimerRow label={tr('director.phase_may_advance')} timer={t.phase_advance_unlock} />
                <TimerRow label={tr('timer.global_beat_cooldown')} timer={t.global_beat_cooldown} />
                <TimerRow
                    label={tr('timer.repetition_window')}
                    timer={t.repetition_window.soonest}
                    note={
                        t.repetition_window.suppressed_count > 0
                            ? tr('timer.suppressed', { n: t.repetition_window.suppressed_count })
                            : tr('timer.nothing_suppressed')
                    }
                />
            </TimerPanel>
            <LastEvaluation evaluation={state.last_evaluation} />
            <DecisionList items={state.recent_decisions} />
            <EventList items={state.recent_events} />
        </div>
    );
}
