import type { LetterPoolState } from '../../types';
import { LetterPoolOverview } from '../LetterPoolOverview';
import { RecentDispatchDetail } from '../RecentDispatchDetail';

interface Props {
    pool: LetterPoolState;
    nowSeconds: number;
}

export function LettersTab({ pool, nowSeconds }: Props) {
    // Recent-dispatch detail unmounts when every slot is Free — no
    // meaningful "most recent" to feature.
    const featured =
        pool.most_recent_dispatch_slot !== null
            ? pool.slots[pool.most_recent_dispatch_slot] ?? null
            : null;

    return (
        <div className="tab-content letters-tab">
            {featured && <RecentDispatchDetail slot={featured} nowSeconds={nowSeconds} />}
            <LetterPoolOverview slots={pool.slots} nowSeconds={nowSeconds} />
        </div>
    );
}
