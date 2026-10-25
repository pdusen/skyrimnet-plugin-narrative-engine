import type { ReactNode } from 'react';

import { t } from '../i18n/strings';

export type TabId = 'director' | 'letters' | 'visit' | 'gossip' | 'plots' | 'dispatch' | 'settings';

interface Props {
    active: TabId;
    onChange: (id: TabId) => void;
    // Tabs whose subsystem is switched off in the INI. They are dropped
    // from the bar entirely rather than rendered disabled — a greyed-out
    // button invites a click that can never do anything.
    hidden?: ReadonlySet<TabId>;
}

// In bar order. Each is labelled by its `tab.<id>` string.
const TABS: TabId[] = ['director', 'letters', 'visit', 'gossip', 'plots', 'dispatch', 'settings'];

export function TabBar({ active, onChange, hidden }: Props): ReactNode {
    return (
        <div className="tab-bar">
            {TABS.filter(id => !hidden?.has(id)).map(id => (
                <button
                    key={id}
                    type="button"
                    className={id === active ? 'tab-button active' : 'tab-button'}
                    onClick={() => onChange(id)}
                >
                    {t(`tab.${id}`)}
                </button>
            ))}
        </div>
    );
}
