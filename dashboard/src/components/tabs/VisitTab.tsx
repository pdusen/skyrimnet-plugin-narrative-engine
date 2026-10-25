import type { ReactNode } from 'react';

import { t } from '../../i18n/strings';
import type { VisitMode, VisitOutcome, VisitTabState, VisitTimers } from '../../types';
import { TimerPanel, TimerRow } from '../TimerPanel';

interface Props {
    visit: VisitTabState;
    timers: VisitTimers;
    nowSeconds: number;
}

// The phase's name for the badge, from its `visit_mode.<mode>` string. Kept
// small so it fits in the badge chip without wrapping.
function phaseLabel(mode: VisitMode): string {
    return t(`visit_mode.${mode}`);
}

function outcomeLabel(o: VisitOutcome): string {
    return t(`visit_outcome.${o}`);
}

function formatRelativeSeconds(secondsAgo: number): string {
    if (secondsAgo < 0) return t('time.in_the_future');
    const ago = (time: string) => t('time.ago', { time });
    if (secondsAgo < 60) return ago(t('time.seconds', { s: Math.floor(secondsAgo) }));
    if (secondsAgo < 3600) return ago(t('time.minutes', { m: Math.floor(secondsAgo / 60) }));
    return ago(t('time.hours', { h: Math.floor(secondsAgo / 3600) }));
}

export function VisitTab({ visit, timers, nowSeconds }: Props): ReactNode {
    const poll = timers.conclusion_poll;
    return (
        <div className="visit-tab">
            <TimerPanel title={t('timer.next_up')}>
                <TimerRow label={t('visit.beat_cooldown')} timer={timers.beat_cooldown} />
                <TimerRow
                    label={t('timer.senders_held_back')}
                    timer={timers.sender_cooldowns.soonest}
                    note={
                        timers.sender_cooldowns.count > 0
                            ? t('timer.on_cooldown', { n: timers.sender_cooldowns.count })
                            : t('timer.none_on_cooldown')
                    }
                />
                <TimerRow label={t('visit.approach_gives_up')} timer={timers.approach_timeout} />
                <TimerRow label={t('visit.return_home_gives_up')} timer={timers.return_home_timeout} />
                {poll.armed ? (
                    <>
                        <TimerRow
                            label={t('visit.poll_on_silence')}
                            timer={poll.silence}
                            note={t('visit.poll_first_trips')}
                        />
                        <TimerRow label={t('visit.poll_on_interval')} timer={poll.interval} />
                        <div className="timer-row">
                            <span className="timer-label">{t('visit.poll_on_turns')}</span>
                            <span className="timer-value">
                                {poll.turns_remaining === null
                                    ? '—'
                                    : t('visit.turns_to_go', { n: poll.turns_remaining })}
                            </span>
                        </div>
                    </>
                ) : (
                    <div className="timer-row">
                        <span className="timer-label">{t('visit.conclusion_poll')}</span>
                        <span className="timer-value timer-inactive">{t('visit.not_armed')}</span>
                    </div>
                )}
            </TimerPanel>
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
                <h3>{t('visit.current')}</h3>
                <p className="empty-state">{t('visit.none_in_progress')}</p>
            </section>
        );
    }
    const c = visit.current;
    const elapsedSec = c.dispatched_at > 0 ? nowSeconds - c.dispatched_at : 0;
    return (
        <section className="panel">
            <h3>{t('visit.current')}</h3>
            <div className="visit-current">
                <div className="visit-current-row">
                    <span className={`phase-badge phase-${c.mode}`}>{phaseLabel(c.mode)}</span>
                    <span className="visit-elapsed">
                        {t('visit.elapsed', { time: t('time.seconds', { s: Math.floor(elapsedSec) }) })}
                    </span>
                    {c.ignore_nudge_count > 0 && (
                        <span className="nudge-count">{t('visit.nudges', { n: c.ignore_nudge_count })}</span>
                    )}
                </div>
                <div className="visit-current-row">
                    <span className="topic-tag">{c.topic_tag || t('visit.no_topic')}</span>
                    <span className="mood-tag">{c.mood || t('visit.no_mood')}</span>
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
                <h3>{t('visit.recent_verdicts')}</h3>
                <p className="empty-state">{t('visit.no_verdicts')}</p>
            </section>
        );
    }
    // Newest first.
    const rows = [...verdicts].reverse();
    return (
        <section className="panel">
            <h3>{t('visit.recent_verdicts')}</h3>
            <ul className="verdict-list">
                {rows.map((v, i) => (
                    <li key={i} className="verdict-row">
                        <span className={v.should_conclude ? 'verdict-badge conclude' : 'verdict-badge continue'}>
                            {v.should_conclude ? t('visit.verdict.conclude') : t('visit.verdict.continue')}
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
                <h3>{t('visit.recent_visits')}</h3>
                <p className="empty-state">{t('visit.no_visits')}</p>
            </section>
        );
    }
    const rows = [...history].reverse();
    return (
        <section className="panel">
            <h3>{t('visit.recent_visits')}</h3>
            <ul className="visit-history">
                {rows.map((h, i) => (
                    <li key={i} className="visit-history-row">
                        <span className={`outcome-pill outcome-${h.outcome}`}>{outcomeLabel(h.outcome)}</span>
                        <span className="visit-history-sender">{h.sender_name || t('visit.unknown_sender')}</span>
                        <span className="visit-history-topic">{h.topic_tag || t('visit.no_topic')}</span>
                        <span className="visit-history-time">
                            {formatRelativeSeconds(nowSeconds - h.dispatched_at)}
                        </span>
                        <span className="visit-history-duration">
                            {t('time.seconds', { s: Math.round(h.duration_seconds) })}
                        </span>
                    </li>
                ))}
            </ul>
        </section>
    );
}
