import type { ReactNode } from 'react';

export type TabId = 'director' | 'letters' | 'visit' | 'gossip' | 'dispatch' | 'settings';

interface Props {
    active: TabId;
    onChange: (id: TabId) => void;
    // Tabs whose subsystem is switched off in the INI. They are dropped
    // from the bar entirely rather than rendered disabled — a greyed-out
    // button invites a click that can never do anything.
    hidden?: ReadonlySet<TabId>;
}

interface TabDef {
    id: TabId;
    label: string;
}

const TABS: TabDef[] = [
    { id: 'director', label: 'Director' },
    { id: 'letters',  label: 'Letters'  },
    { id: 'visit',    label: 'Visit'    },
    { id: 'gossip',   label: 'Gossip'   },
    { id: 'dispatch', label: 'Dispatch' },
    { id: 'settings', label: 'Settings' },
];

export function TabBar({ active, onChange, hidden }: Props): ReactNode {
    return (
        <div className="tab-bar">
            {TABS.filter(t => !hidden?.has(t.id)).map(t => (
                <button
                    key={t.id}
                    type="button"
                    className={t.id === active ? 'tab-button active' : 'tab-button'}
                    onClick={() => onChange(t.id)}
                >
                    {t.label}
                </button>
            ))}
        </div>
    );
}
