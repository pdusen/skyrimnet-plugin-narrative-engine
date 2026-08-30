import { useEffect, useState } from 'react';

import { StatusBanner } from './components/StatusBanner';
import { TabBar, type TabId } from './components/TabBar';
import { DirectorTab } from './components/tabs/DirectorTab';
import { DispatchTab } from './components/tabs/DispatchTab';
import { PlotsTab } from './components/tabs/PlotsTab';
import { GossipTab } from './components/tabs/GossipTab';
import { LettersTab } from './components/tabs/LettersTab';
import { SettingsTab } from './components/tabs/SettingsTab';
import { VisitTab } from './components/tabs/VisitTab';
import { stateStore, type Snapshot } from './stateStore';

// Module-level constants so the sets are stable identities across renders.
const NO_HIDDEN_TABS: ReadonlySet<TabId> = new Set();
const GOSSIP_HIDDEN: ReadonlySet<TabId> = new Set<TabId>(['gossip']);
const PLOTS_HIDDEN: ReadonlySet<TabId> = new Set<TabId>(['plots']);
const BOTH_HIDDEN: ReadonlySet<TabId> = new Set<TabId>(['gossip', 'plots']);

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

    // bGossipEnabled=false takes the whole tab away rather than showing an
    // empty one. Derived per render instead of synced into state so a
    // config reload that flips the setting can't strand the view on a tab
    // that no longer has a button.
    // A subsystem that is switched off loses its tab entirely rather than
    // rendering it disabled — a greyed-out button invites a click that can
    // never do anything.
    const hiddenTabs = s.gossip.enabled
        ? (s.plots.enabled ? NO_HIDDEN_TABS : PLOTS_HIDDEN)
        : (s.plots.enabled ? GOSSIP_HIDDEN : BOTH_HIDDEN);
    const shownTab = hiddenTabs.has(activeTab) ? 'director' : activeTab;

    return (
        <div className="dashboard">
            <StatusBanner status={s.status} />
            <TabBar active={shownTab} onChange={setActiveTab} hidden={hiddenTabs} />
            {shownTab === 'director' && <DirectorTab state={s} />}
            {shownTab === 'letters'  && <LettersTab pool={s.letter_pool} nowSeconds={nowSeconds} />}
            {shownTab === 'visit'    && <VisitTab visit={s.visit} nowSeconds={nowSeconds} />}
            {shownTab === 'gossip'   && <GossipTab gossip={s.gossip} />}
            {shownTab === 'plots'    && <PlotsTab plots={s.plots} />}
            {shownTab === 'dispatch' && <DispatchTab state={s} nowSeconds={nowSeconds} />}
            {shownTab === 'settings' && <SettingsTab state={s} />}
        </div>
    );
}
