import type { ReactNode } from 'react';

import type { VisitTabState } from '../../types';

interface Props {
    visit: VisitTabState;
    nowSeconds: number;
}

// Human-readable phase name for the badge. Kept small so it fits in
// the badge chip without wrapping.
function phaseLabel(mode: string): string {
    switch (mode) {
        case 'idle':        return 'Idle';
        case 'composing':   return 'Composing';
        case 'salutation':  return 'Salutation';
        case 'discuss':     return 'Discuss';
        case 'on_hold':     return 'On Hold';
        case 'reengage':    return 'Re-engage';
        case 'valediction': return 'Valediction';
        case 'return_home': return 'Return Home';
        default:            return mode;
    }
}

function outcomeLabel(o: string): string {
    switch (o) {
        case 'completed':   return 'completed';
        case 'unsatisfied': return 'unsatisfied';
        case 'rolled_back': return 'rolled back';
        case 'aborted':     return 'aborted';
        default:            return o;
    }
}

function formatRelativeSeconds(secondsAgo: number): string {
    if (secondsAgo < 0) return 'in the future';
    if (secondsAgo < 60) return `${Math.floor(secondsAgo)}s ago`;
    if (secondsAgo < 3600) return `${Math.floor(secondsAgo / 60)}m ago`;
    return `${Math.floor(secondsAgo / 3600)}h ago`;
}

export function VisitTab({ visit, nowSeconds }: Props): ReactNode {
    return (
        <div className="visit-tab">
            <CurrentConversationPanel visit={visit} nowSeconds={nowSeconds} />
            <RecentPollsPanel verdicts={visit.recent_verdicts} nowSeconds={nowSeconds} />
            <VisitHistoryPanel history={visit.history} nowSeconds={nowSeconds} />
        </div>
    );
}

function CurrentConversationPanel({ visit, nowSeconds }: {
    visit: VisitTabState;
    nowSeconds: number;
}): ReactNode {
    if (!visit.current) {
        return (
            <section className="panel">
                <h3>Current Conversation</h3>
                <p className="empty-state">No visit in progress.</p>
            </section>
        );
    }
    const c = visit.current;
    const elapsedSec = c.dispatched_at > 0 ? nowSeconds - c.dispatched_at : 0;
    return (
        <section className="panel">
            <h3>Current Conversation</h3>
            <div className="visit-current">
                <div className="visit-current-row">
                    <span className={`phase-badge phase-${c.mode}`}>{phaseLabel(c.mode)}</span>
                    <span className="visit-elapsed">{`elapsed ${Math.floor(elapsedSec)}s`}</span>
                    {c.ignore_nudge_count > 0 && (
                        <span className="nudge-count">{`nudges: ${c.ignore_nudge_count}`}</span>
                    )}
                </div>
                <div className="visit-current-row">
                    <span className="topic-tag">{c.topic_tag || '(no topic)'}</span>
                    <span className="mood-tag">{c.mood || '(no mood)'}</span>
                </div>
                {c.briefing_preview && (
                    <p className="briefing-preview">{c.briefing_preview}</p>
                )}
            </div>
        </section>
    );
}

function RecentPollsPanel({ verdicts, nowSeconds }: {
    verdicts: VisitTabState['recent_verdicts'];
    nowSeconds: number;
}): ReactNode {
    if (verdicts.length === 0) {
        return (
            <section className="panel">
                <h3>Recent Poll Verdicts</h3>
                <p className="empty-state">No verdicts yet.</p>
            </section>
        );
    }
    // Newest first.
    const rows = [...verdicts].reverse();
    return (
        <section className="panel">
            <h3>Recent Poll Verdicts</h3>
            <ul className="verdict-list">
                {rows.map((v, i) => (
                    <li key={i} className="verdict-row">
                        <span className={v.should_conclude ? 'verdict-badge conclude' : 'verdict-badge continue'}>
                            {v.should_conclude ? 'conclude' : 'continue'}
                        </span>
                        <span className="verdict-time">{formatRelativeSeconds(nowSeconds - v.fired_at)}</span>
                        <span className="verdict-rationale">{v.rationale}</span>
                    </li>
                ))}
            </ul>
        </section>
    );
}

function VisitHistoryPanel({ history, nowSeconds }: {
    history: VisitTabState['history'];
    nowSeconds: number;
}): ReactNode {
    if (history.length === 0) {
        return (
            <section className="panel">
                <h3>Recent Visits</h3>
                <p className="empty-state">No visits dispatched yet.</p>
            </section>
        );
    }
    const rows = [...history].reverse();
    return (
        <section className="panel">
            <h3>Recent Visits</h3>
            <ul className="visit-history">
                {rows.map((h, i) => (
                    <li key={i} className="visit-history-row">
                        <span className={`outcome-pill outcome-${h.outcome}`}>{outcomeLabel(h.outcome)}</span>
                        <span className="visit-history-sender">{h.sender_name || '(unknown)'}</span>
                        <span className="visit-history-topic">{h.topic_tag || '(no topic)'}</span>
                        <span className="visit-history-time">
                            {formatRelativeSeconds(nowSeconds - h.dispatched_at)}
                        </span>
                        <span className="visit-history-duration">
                            {`${Math.round(h.duration_seconds)}s`}
                        </span>
                    </li>
                ))}
            </ul>
        </section>
    );
}
