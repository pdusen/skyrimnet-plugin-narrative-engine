import type { DirectorState } from '../types';

interface Props {
    evaluation: DirectorState['last_evaluation'];
}

export function LastEvaluation({ evaluation }: Props) {
    if (!evaluation) {
        return (
            <section className="panel last-eval">
                <h2>Last Evaluation</h2>
                <div className="placeholder-inline">No evaluations yet.</div>
            </section>
        );
    }
    // Failed actions arrive as "(failed: <detail>)" — strip the wrapper
    // for display.
    let firedAction: string | null = null;
    let failedAction: string | null = null;
    if (evaluation.action) {
        const m = /^\(failed:\s*(.*)\)$/.exec(evaluation.action);
        if (m && m[1] !== undefined) {
            failedAction = m[1];
        } else {
            firedAction = evaluation.action;
        }
    }

    return (
        <section className="panel last-eval">
            <h2>Last Evaluation</h2>
            <div className="tension">tension {evaluation.tension_score}</div>
            {evaluation.advanced_to && (
                <div className="advance">→ advanced to {evaluation.advanced_to}</div>
            )}
            {firedAction && (
                <div className="fired">→ fired: {firedAction}</div>
            )}
            {failedAction && (
                <div className="failed">→ action failed: {failedAction}</div>
            )}
            <div className="note">"{evaluation.narrative_note}"</div>
            {evaluation.alpha_canon_signals.length > 0 && (
                <div className="signals">
                    Active signals: {evaluation.alpha_canon_signals.join(', ')}
                </div>
            )}
        </section>
    );
}
