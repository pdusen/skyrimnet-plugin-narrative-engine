import { label, t } from '../i18n/strings';
import type { DirectorState } from '../types';

interface Props {
    evaluation: DirectorState['last_evaluation'];
}

export function LastEvaluation({ evaluation }: Props) {
    if (!evaluation) {
        return (
            <section className="panel last-eval">
                <h2>{t('director.last_evaluation')}</h2>
                <div className="placeholder-inline">{t('director.no_evaluations')}</div>
            </section>
        );
    }

    return (
        <section className="panel last-eval">
            <h2>{t('director.last_evaluation')}</h2>
            <div className="tension">{t('director.tension', { score: evaluation.tension_score })}</div>
            {evaluation.advanced_to && (
                <div className="advance">
                    {t('director.advanced_to', { phase: label('phase.', evaluation.advanced_to) })}
                </div>
            )}
            {evaluation.action && (
                <div className="fired">{t('director.fired', { beat: label('beat.', evaluation.action) })}</div>
            )}
            {evaluation.failure && (
                <div className="failed">{t('director.action_failed', { reason: evaluation.failure })}</div>
            )}
            <div className="note">{t('director.quoted_note', { note: evaluation.narrative_note })}</div>
            {evaluation.alpha_canon_signals.length > 0 && (
                <div className="signals">
                    {t('director.active_signals', {
                        signals: evaluation.alpha_canon_signals.map(s => label('signal.', s)),
                    })}
                </div>
            )}
        </section>
    );
}
