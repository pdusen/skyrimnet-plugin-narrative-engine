import { useMemo } from 'react';

import { t } from '../i18n/strings';
import type { LetterSlot } from '../types';

interface Props {
    slots: LetterSlot[];
    nowSeconds: number;
}

function formatRelative(deltaSeconds: number): string {
    if (deltaSeconds < 1) return t('time.just_now');
    const ago = (time: string) => t('time.ago', { time });
    if (deltaSeconds < 60) return ago(t('time.seconds', { s: Math.round(deltaSeconds) }));
    if (deltaSeconds < 3600) return ago(t('time.minutes', { m: Math.round(deltaSeconds / 60) }));
    if (deltaSeconds < 86400) return ago(t('time.hours', { h: Math.round(deltaSeconds / 3600) }));
    return ago(t('time.days', { d: Math.round(deltaSeconds / 86400) }));
}

function stateBadgeClass(state: LetterSlot['state']): string {
    return `state-badge state-${state}`;
}

// The table's narrow column takes each state's short form.
function prettyState(state: LetterSlot['state']): string {
    return t(`slot_state.${state}.short`);
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
            <h2>{t('letters.pool')}</h2>
            <div className="pool-stats">
                <span>{t('letters.count', { state: t('slot_state.free'), n: stats.free })}</span>
                <span>{t('letters.count', { state: t('slot_state.pending_delivery'), n: stats.pending })}</span>
                <span>{t('letters.count', { state: t('slot_state.in_inventory'), n: stats.inv })}</span>
                <span>{t('letters.count', { state: t('slot_state.read'), n: stats.read })}</span>
            </div>
            <table className="pool-table">
                <thead>
                    <tr>
                        <th className="col-idx">#</th>
                        <th className="col-state">{t('letters.column.state')}</th>
                        <th className="col-sender">{t('letters.column.sender')}</th>
                        <th className="col-topic">{t('letters.column.topic')}</th>
                        <th className="col-age">{t('letters.column.age')}</th>
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
