import { t } from '../../i18n/strings';
import type { LetterPoolState, LetterTimers } from '../../types';
import { TimerPanel, TimerRow } from '../TimerPanel';
import { LetterPoolOverview } from '../LetterPoolOverview';
import { RecentDispatchDetail } from '../RecentDispatchDetail';

interface Props {
    pool: LetterPoolState;
    timers: LetterTimers;
    nowSeconds: number;
}

export function LettersTab({ pool, timers, nowSeconds }: Props) {
    // Recent-dispatch detail unmounts when every slot is Free — no
    // meaningful "most recent" to feature.
    const featured =
        pool.most_recent_dispatch_slot !== null
            ? pool.slots[pool.most_recent_dispatch_slot] ?? null
            : null;

    return (
        <div className="tab-content letters-tab">
            <TimerPanel title={t('timer.next_up')}>
                <TimerRow label={t('letters.beat_cooldown')} timer={timers.beat_cooldown} />
                <TimerRow
                    label={t('timer.senders_held_back')}
                    timer={timers.sender_cooldowns.soonest}
                    note={
                        timers.sender_cooldowns.count > 0
                            ? t('timer.on_cooldown', { n: timers.sender_cooldowns.count })
                            : t('timer.none_on_cooldown')
                    }
                />
                {timers.pending_delivery.map(p => (
                    <TimerRow
                        key={p.slot_index}
                        label={t('letters.delivery_gives_up', { slot: p.slot_index })}
                        timer={p.timeout}
                    />
                ))}
            </TimerPanel>
            {featured && <RecentDispatchDetail slot={featured} nowSeconds={nowSeconds} />}
            <LetterPoolOverview slots={pool.slots} nowSeconds={nowSeconds} />
        </div>
    );
}
