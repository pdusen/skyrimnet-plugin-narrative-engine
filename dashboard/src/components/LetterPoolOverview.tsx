import { useMemo } from 'react';

import type { LetterSlot } from '../types';

interface Props {
    slots: LetterSlot[];
    nowSeconds: number;
}

function formatRelative(deltaSeconds: number): string {
    if (deltaSeconds < 1)     return 'just now';
    if (deltaSeconds < 60)    return `${Math.round(deltaSeconds)}s ago`;
    if (deltaSeconds < 3600)  return `${Math.round(deltaSeconds / 60)}m ago`;
    if (deltaSeconds < 86400) return `${Math.round(deltaSeconds / 3600)}h ago`;
    return `${Math.round(deltaSeconds / 86400)}d ago`;
}

function stateBadgeClass(state: LetterSlot['state']): string {
    return `state-badge state-${state}`;
}

function prettyState(state: LetterSlot['state']): string {
    switch (state) {
        case 'free':             return 'Free';
        case 'pending_delivery': return 'Pending';
        case 'in_inventory':     return 'In Inv';
        case 'read':             return 'Read';
    }
}

function truncate(s: string, max: number): string {
    if (s.length <= max) return s;
    return s.slice(0, max - 1) + '…';
}

export function LetterPoolOverview({ slots, nowSeconds }: Props) {
    // Derive the header counts client-side from the same slot array the
    // table renders. Keeps the payload smaller and avoids drift between
    // header and rows.
    const stats = useMemo(() => {
        let free = 0, pending = 0, inv = 0, read = 0;
        for (const s of slots) {
            switch (s.state) {
                case 'free':             ++free; break;
                case 'pending_delivery': ++pending; break;
                case 'in_inventory':     ++inv; break;
                case 'read':             ++read; break;
            }
        }
        return { free, pending, inv, read };
    }, [slots]);

    return (
        <section className="panel letter-pool-overview">
            <h2>Pool</h2>
            <div className="pool-stats">
                <span>Free: {stats.free}</span>
                <span>Pending: {stats.pending}</span>
                <span>In Inventory: {stats.inv}</span>
                <span>Read: {stats.read}</span>
            </div>
            <table className="pool-table">
                <thead>
                    <tr>
                        <th className="col-idx">#</th>
                        <th className="col-state">State</th>
                        <th className="col-sender">Sender</th>
                        <th className="col-topic">Topic</th>
                        <th className="col-age">Age</th>
                    </tr>
                </thead>
                <tbody>
                    {slots.map(s => {
                        const isFree = s.state === 'free';
                        const age = (() => {
                            if (isFree) return '—';
                            const ref = s.read_at > 0 ? s.read_at : s.delivered_at;
                            if (ref <= 0) return '—';
                            return formatRelative(nowSeconds - ref);
                        })();
                        return (
                            <tr key={s.index}>
                                <td className="col-idx">{s.index + 1}</td>
                                <td className="col-state">
                                    <span className={stateBadgeClass(s.state)}>
                                        {prettyState(s.state)}
                                    </span>
                                </td>
                                <td className="col-sender">
                                    {isFree ? '' : truncate(s.letter_label, 20)}
                                </td>
                                <td className="col-topic">
                                    {isFree ? '' : truncate(s.topic_tag, 30)}
                                </td>
                                <td className="col-age">{age}</td>
                            </tr>
                        );
                    })}
                </tbody>
            </table>
        </section>
    );
}
