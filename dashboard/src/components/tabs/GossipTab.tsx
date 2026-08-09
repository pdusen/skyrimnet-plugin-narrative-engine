import { useState } from 'react';

import type { GossipTabState, RumorEntry } from '../../types';

interface Props {
    gossip: GossipTabState;
}

// In-world durations, rendered the way a reader thinks about them. Under a
// day the day figure is all noise — "7h" says more than "0.3d" does.
function duration(valueDays: number): string {
    if (valueDays < 1) {
        const hours = valueDays * 24;
        return hours < 1 ? `${Math.round(hours * 60)}m` : `${hours.toFixed(1)}h`;
    }
    return `${valueDays.toFixed(1)}d`;
}

function plural(n: number, one: string, many = `${one}s`): string {
    return `${n} ${n === 1 ? one : many}`;
}

// The generation window a band covers. Band selection at transmission time
// is min(generation / 3, bands - 1), so every band spans three generations
// except the last, which absorbs everything above it.
function bandRange(index: number, total: number): string {
    const lo = index * 3;
    return index === total - 1 ? `gen ${lo}+` : `gen ${lo}–${lo + 2}`;
}

function RumorRow({ rumor }: { rumor: RumorEntry }) {
    const [expanded, setExpanded] = useState(false);
    // A rumor with no text should still be identifiable, so fall back to
    // naming its source rather than rendering an empty quote.
    const headline = rumor.text || `(no text — from memory m${rumor.source_memory_id})`;

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
                title={expanded ? 'Collapse' : 'Show every generation band and provenance'}
            >
                <div className="rumor-head">
                    <span className="rumor-id">r{String(rumor.id).padStart(2, '0')}</span>
                    <span className={rumor.stalled ? 'rumor-state stalled' : 'rumor-state spreading'}>
                        {rumor.stalled ? 'Stalled' : 'Spreading'}
                    </span>
                    <span className="rumor-meta">n={rumor.notability.toFixed(2)}</span>
                    <span className="rumor-meta">{duration(rumor.age_days)} old</span>
                    <span className="rumor-expand" aria-hidden="true">
                        {expanded ? '▾' : '▸'}
                    </span>
                </div>

                <div className="rumor-text">{headline}</div>

                <div className="rumor-stats">
                    <span>
                        <b>{rumor.carriers}</b> {rumor.carriers === 1 ? 'carrier' : 'carriers'}{' '}
                        <i>({rumor.active_carriers} still telling)</i>
                    </span>
                    <span>
                        <b>{rumor.settlements}</b> {rumor.settlements === 1 ? 'settlement' : 'settlements'}
                    </span>
                    <span>
                        <b>{rumor.holds}</b> {rumor.holds === 1 ? 'hold' : 'holds'}
                    </span>
                    <span>
                        gen <b>{rumor.max_depth}</b>
                    </span>
                    <span>
                        <b>{rumor.transmissions}</b> told <i>({rumor.wasted} wasted)</i>
                    </span>
                    <span>
                        quiet <b>{duration(rumor.idle_days)}</b>
                    </span>
                </div>
            </button>

            {expanded && (
                <div className="rumor-detail">
                    <div className="rumor-origin">
                        from <b>{rumor.origin_name || 'unknown'}</b>
                        {rumor.origin_location ? ` @ ${rumor.origin_location}` : ''} · source memory{' '}
                        <b>m{rumor.source_memory_id}</b>
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

export function GossipTab({ gossip }: Props) {
    if (!gossip.enabled) {
        return (
            <div className="gossip-tab">
                <section className="panel">
                    <h2>Gossip</h2>
                    <p className="empty-state">
                        Disabled. Set bGossipEnabled=true in the [Gossip] section of NarrativeEngine.ini.
                    </p>
                </section>
            </div>
        );
    }

    const stalled = gossip.rumors.filter(r => r.stalled).length;

    return (
        <div className="gossip-tab">
            <section className="panel">
                <h2>Gossip</h2>
                <div className="gossip-summary">
                    <span>
                        <b>{gossip.rumors.length}</b> in the world
                    </span>
                    <span>
                        <b>{stalled}</b> stalled
                    </span>
                    <span>
                        <b>{gossip.queued_events}</b> queued {gossip.queued_events === 1 ? 'step' : 'steps'}
                    </span>
                    <span>
                        <b>{gossip.transmissions_session}</b> told this session{' '}
                        <i>({gossip.wasted_session} wasted)</i>
                    </span>
                    <span>
                        <b>{plural(gossip.harvest_sweeps, 'sweep')}</b>, {gossip.harvest_sent_for_generation} sent to
                        generation
                    </span>
                    <span>
                        <b>{plural(gossip.claims_outstanding, 'claim')}</b> held
                    </span>
                    {!gossip.graph_ready && <span className="gossip-warn">social graph not built</span>}
                    {!gossip.harvest_enabled && <span className="gossip-warn">auto-harvest off</span>}
                </div>
            </section>

            <section className="panel gossip-list-panel">
                <h2>Active rumors — newest first</h2>
                {gossip.rumors.length === 0 ? (
                    <p className="empty-state">
                        No rumors in the world. Sweeps look for new material every
                        fGossipHarvestIntervalGameHours.
                    </p>
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
