import { useEffect, useState } from 'react';

import { StatusBanner } from './components/StatusBanner';
import { TabBar, type TabId } from './components/TabBar';
import { DirectorTab } from './components/tabs/DirectorTab';
import { LettersTab } from './components/tabs/LettersTab';
import { stateStore, type Snapshot } from './stateStore';

export function App() {
    const [snap, setSnap] = useState<Snapshot>(() => stateStore.getSnapshot());
    const [activeTab, setActiveTab] = useState<TabId>('director');

    useEffect(() => {
        // Re-sync once on mount in case set() fired between initial render
        // and effect, then subscribe for the lifetime of the component.
        setSnap(stateStore.getSnapshot());
        return stateStore.subscribe(() => setSnap(stateStore.getSnapshot()));
    }, []);

    if (snap.error) {
        return <div className="dashboard error">{snap.error}</div>;
    }
    if (!snap.state) {
        return <div className="dashboard placeholder">Awaiting first Director evaluation…</div>;
    }

    const s = snap.state;
    // Client-wall-clock "now" for relative timestamps. Snapshots don't
    // carry a serverNow field, so this is best-effort — good enough for
    // "2m ago" precision on a dashboard the player looks at manually.
    const nowSeconds = Date.now() / 1000;

    return (
        <div className="dashboard">
            <StatusBanner status={s.status} />
            <TabBar active={activeTab} onChange={setActiveTab} />
            {activeTab === 'director'
                ? <DirectorTab state={s} />
                : <LettersTab pool={s.letter_pool} nowSeconds={nowSeconds} />}
        </div>
    );
}
