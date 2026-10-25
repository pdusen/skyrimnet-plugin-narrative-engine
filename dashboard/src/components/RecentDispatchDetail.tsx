import { t } from '../i18n/strings';
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

function prettyState(state: LetterSlot['state']): string {
    return t(`slot_state.${state}`);
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
            <h2>{t('letters.most_recent')}</h2>
            <div className="dispatch-header">
                <span className="sender-label">{slot.letter_label || t('letters.no_label')}</span>
                <span className={stateBadgeClass(slot.state)}>{prettyState(slot.state)}</span>
            </div>
            <div className="dispatch-meta">
                {slot.topic_tag && <span className="topic-tag">{slot.topic_tag}</span>}
                {slot.mood && <span className="mood-tag">{slot.mood}</span>}
                {deliveredAgo && <span className="timestamp">{t('letters.delivered', { ago: deliveredAgo })}</span>}
                {readAgo && <span className="timestamp">{t('letters.read', { ago: readAgo })}</span>}
            </div>
            {slot.body_preview && (
                <div className="body-preview">{slot.body_preview}</div>
            )}
        </section>
    );
}
