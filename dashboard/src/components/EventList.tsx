import { t } from '../i18n/strings';
import type { EventEntry } from '../types';

interface Props {
    items: EventEntry[];
}

export function EventList({ items }: Props) {
    if (items.length === 0) {
        return (
            <section className="panel event-list">
                <h2>{t('director.recent_events')}</h2>
                <div className="placeholder-inline">{t('director.no_events')}</div>
            </section>
        );
    }
    // Newest first. Sort by gameTime descending rather than relying on
    // the C++ side's ordering — defensive against future order changes
    // and unambiguous when the user is scanning the list visually.
    const newestFirst = [...items].sort((a, b) => b.gameTime - a.gameTime);
    return (
        <section className="panel event-list">
            <h2>{t('director.recent_events')}</h2>
            <ul>
                {newestFirst.map((e, i) => (
                    <li key={i}>
                        {/* SkyrimNet's own event type: a token, shown as sent. */}
                        <span className="type">[{e.type}]</span>
                        <span className="text">{e.text}</span>
                    </li>
                ))}
            </ul>
        </section>
    );
}
