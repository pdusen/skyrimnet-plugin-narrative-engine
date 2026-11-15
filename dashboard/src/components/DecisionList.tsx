import { label, t } from '../i18n/strings';
import type { DecisionEntry } from '../types';

interface Props {
    items: DecisionEntry[];
}

export function DecisionList({ items }: Props) {
    if (items.length === 0) {
        return (
            <section className="panel decision-list">
                <h2>{t('director.recent_decisions')}</h2>
                <div className="placeholder-inline">{t('director.no_decisions')}</div>
            </section>
        );
    }
    // Newest first. Sort on timestamp descending rather than relying on
    // the C++ side's ordering — defensive against future order changes
    // and unambiguous when the user is scanning the list visually.
    const newestFirst = [...items].sort((a, b) => b.timestamp - a.timestamp);
    return (
        <section className="panel decision-list">
            <h2>{t('director.recent_decisions')}</h2>
            <ul>
                {newestFirst.map((d, i) => (
                    <li key={i}>
                        <span className="tension">{d.tension_score}</span>
                        <span className="phase">{label('phase.', d.phase)}</span>
                        <span className="note">{d.narrative_note}</span>
                    </li>
                ))}
            </ul>
        </section>
    );
}
