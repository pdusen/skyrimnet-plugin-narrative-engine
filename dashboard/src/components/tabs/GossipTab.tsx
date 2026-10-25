import { useState } from 'react';

import { Rich } from '../../i18n/Rich';
import { t } from '../../i18n/strings';
import type { GossipTabState, GossipTimers, RumorEntry } from '../../types';
import { TimerPanel, TimerRow } from '../TimerPanel';

interface Props {
    gossip: GossipTabState;
    timers: GossipTimers;
}

// In-world durations, rendered the way a reader thinks about them. Under a
// day the day figure is all noise — "7h" says more than "0.3d" does.
function duration(valueDays: number): string {
    if (valueDays < 1) {
        const hours = valueDays * 24;
        return hours < 1 ? t('time.minutes', { m: Math.round(hours * 60) }) : t('time.hours', { h: hours.toFixed(1) });
    }
    return t('time.days', { d: valueDays.toFixed(1) });
}

// The generation window a band covers. Band selection at transmission time
// is min(generation / 3, bands - 1), so every band spans three generations
// except the last, which absorbs everything above it.
function bandRange(index: number, total: number): string {
    const lo = index * 3;
    return index === total - 1 ? t('gossip.band_from', { lo }) : t('gossip.band_range', { lo, hi: lo + 2 });
}

function RumorRow({ rumor }: { rumor: RumorEntry }) {
    const [expanded, setExpanded] = useState(false);
    // A rumor with no text should still be identifiable, so fall back to
    // naming its source rather than rendering an empty quote.
    const headline = rumor.text || t('gossip.no_text', { memory: rumor.source_memory_id });

    return (
        <li className={rumor.stalled ? 'rumor stalled' : 'rumor'}>
            {/* The whole summary is the toggle, not just the chevron — a
                12px arrow is a poor target for something every row does.
                A real <button> rather than a click handler on a div, so it
                is keyboard-reachable and announces its state; the chevron
                is now a passive indicator inside it, since nesting a
                button inside a button is invalid. The expanded detail
                below is deliberately OUTSIDE the button, so selecting band
                text does not collapse the row. */}
            <button
                type="button"
                className="rumor-summary"
                onClick={() => setExpanded(v => !v)}
                aria-expanded={expanded}
                title={expanded ? t('gossip.collapse') : t('gossip.expand')}
            >
                <div className="rumor-head">
                    <span className="rumor-id">{t('gossip.rumor_id', { id: String(rumor.id).padStart(2, '0') })}</span>
                    <span className={rumor.stalled ? 'rumor-state stalled' : 'rumor-state spreading'}>
                        {rumor.stalled ? t('gossip.stalled') : t('gossip.spreading')}
                    </span>
                    <span className="rumor-meta">{t('gossip.notability', { n: rumor.notability.toFixed(2) })}</span>
                    <span className="rumor-meta">{t('gossip.age', { age: duration(rumor.age_days) })}</span>
                    <span className="rumor-expand" aria-hidden="true">
                        {expanded ? '▾' : '▸'}
                    </span>
                </div>

                <div className="rumor-text">{headline}</div>

                <div className="rumor-stats">
                    <span>
                        <Rich k="gossip.carriers" args={{ n: rumor.carriers, active: rumor.active_carriers }} />
                    </span>
                    <span>
                        <Rich k="gossip.settlements" args={{ n: rumor.settlements }} />
                    </span>
                    <span>
                        <Rich k="gossip.holds" args={{ n: rumor.holds }} />
                    </span>
                    <span>
                        <Rich k="gossip.generation" args={{ n: rumor.max_depth }} />
                    </span>
                    <span>
                        <Rich k="gossip.told" args={{ n: rumor.transmissions, wasted: rumor.wasted }} />
                    </span>
                    <span>
                        <Rich k="gossip.quiet" args={{ time: duration(rumor.idle_days) }} />
                    </span>
                </div>
            </button>

            {expanded && (
                <div className="rumor-detail">
                    <div className="rumor-origin">
                        <Rich
                            k={rumor.origin_location ? 'gossip.origin_at' : 'gossip.origin'}
                            args={{
                                name: rumor.origin_name || t('gossip.unknown_origin'),
                                location: rumor.origin_location,
                                memory: rumor.source_memory_id,
                            }}
                        />
                    </div>
                    <ol className="rumor-bands">
                        {rumor.bands.map((b, i) => (
                            <li key={i}>
                                <span className="band-label">{bandRange(i, rumor.bands.length)}</span>
                                <span className="band-text">{b}</span>
                            </li>
                        ))}
                    </ol>
                </div>
            )}
        </li>
    );
}

export function GossipTab({ gossip, timers }: Props) {
    if (!gossip.enabled) {
        return (
            <div className="gossip-tab">
                <section className="panel">
                    <h2>{t('tab.gossip')}</h2>
                    <p className="empty-state">{t('gossip.disabled')}</p>
                </section>
            </div>
        );
    }

    const stalled = gossip.rumors.filter(r => r.stalled).length;

    return (
        <div className="gossip-tab">
            <TimerPanel title={t('timer.next_up')}>
                <TimerRow label={t('gossip.next_schedule_check')} timer={timers.next_schedule_check} />
                <TimerRow label={t('gossip.next_tick')} timer={timers.next_tick} note={t('gossip.next_tick_note')} />
            </TimerPanel>
            <section className="panel">
                <h2>{t('tab.gossip')}</h2>
                <div className="gossip-summary">
                    <span>
                        <Rich k="gossip.in_the_world" args={{ n: gossip.rumors.length }} />
                    </span>
                    <span>
                        <Rich k="gossip.stalled_count" args={{ n: stalled }} />
                    </span>
                    <span>
                        <Rich k="gossip.queued" args={{ n: gossip.queued_events }} />
                    </span>
                    <span>
                        <Rich
                            k="gossip.told_this_session"
                            args={{ n: gossip.transmissions_session, wasted: gossip.wasted_session }}
                        />
                    </span>
                    <span>
                        <Rich
                            k="gossip.sweeps"
                            args={{ n: gossip.harvest_sweeps, sent: gossip.harvest_sent_for_generation }}
                        />
                    </span>
                    <span>
                        <Rich k="gossip.claims" args={{ n: gossip.claims_outstanding }} />
                    </span>
                    {!gossip.graph_ready && <span className="gossip-warn">{t('gossip.graph_not_built')}</span>}
                    {!gossip.harvest_enabled && <span className="gossip-warn">{t('gossip.harvest_off')}</span>}
                </div>
            </section>

            <section className="panel gossip-list-panel">
                <h2>{t('gossip.active_rumors')}</h2>
                {gossip.rumors.length === 0 ? (
                    <p className="empty-state">{t('gossip.no_rumors')}</p>
                ) : (
                    <ul className="rumor-list">
                        {gossip.rumors.map(r => (
                            <RumorRow key={r.id} rumor={r} />
                        ))}
                    </ul>
                )}
            </section>
        </div>
    );
}
