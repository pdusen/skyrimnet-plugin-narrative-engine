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
            <TimerPanel title="Next up">
                <TimerRow label="Letter beat cooldown" timer={timers.beat_cooldown} />
                <TimerRow
                    label="Senders held back"
                    timer={timers.sender_cooldowns.soonest}
                    note={
                        timers.sender_cooldowns.count > 0
                            ? `${timers.sender_cooldowns.count} on cooldown`
                            : 'none on cooldown'
                    }
                />
                {timers.pending_delivery.map(p => (
                    <TimerRow
                        key={p.slot_index}
                        label={`Slot ${p.slot_index} delivery gives up in`}
                        timer={p.timeout}
                    />
                ))}
            </TimerPanel>
            {featured && <RecentDispatchDetail slot={featured} nowSeconds={nowSeconds} />}
            <LetterPoolOverview slots={pool.slots} nowSeconds={nowSeconds} />
        </div>
    );
}
