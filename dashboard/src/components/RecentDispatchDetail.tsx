import type { LetterSlot } from '../types';

interface Props {
    slot: LetterSlot;
    // Server clock at snapshot time (seconds since Unix epoch). Used to
    // render relative timestamps like "delivered 2m ago". Passed in
    // rather than read from `Date.now()` inside so the whole panel
    // stays consistent with the C++ snapshot: if the snapshot arrives
    // late, we still say "2m ago" relative to when it was taken, not
    // to now.
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
        case 'in_inventory':     return 'In Inventory';
        case 'read':             return 'Read';
    }
}

export function RecentDispatchDetail({ slot, nowSeconds }: Props) {
    const deliveredAgo = slot.delivered_at > 0
        ? formatRelative(nowSeconds - slot.delivered_at)
        : null;
    const readAgo = slot.read_at > 0
        ? formatRelative(nowSeconds - slot.read_at)
        : null;

    return (
        <section className="panel recent-dispatch">
            <h2>Most Recent Letter</h2>
            <div className="dispatch-header">
                <span className="sender-label">{slot.letter_label || '(no label)'}</span>
                <span className={stateBadgeClass(slot.state)}>{prettyState(slot.state)}</span>
            </div>
            <div className="dispatch-meta">
                {slot.topic_tag && <span className="topic-tag">{slot.topic_tag}</span>}
                {slot.mood && <span className="mood-tag">{slot.mood}</span>}
                {deliveredAgo && <span className="timestamp">delivered {deliveredAgo}</span>}
                {readAgo && <span className="timestamp">read {readAgo}</span>}
            </div>
            {slot.body_preview && (
                <div className="body-preview">{slot.body_preview}</div>
            )}
        </section>
    );
}
