import type { ReactNode } from 'react';

export type TabId = 'director' | 'letters' | 'visit' | 'dispatch';

interface Props {
    active: TabId;
    onChange: (id: TabId) => void;
}

interface TabDef {
    id: TabId;
    label: string;
}

const TABS: TabDef[] = [
    { id: 'director', label: 'Director' },
    { id: 'letters',  label: 'Letters'  },
    { id: 'visit',    label: 'Visit'    },
    { id: 'dispatch', label: 'Dispatch' },
];

export function TabBar({ active, onChange }: Props): ReactNode {
    return (
        <div className="tab-bar">
            {TABS.map(t => (
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
