import { useState, type ReactNode } from 'react';

import { Rich } from '../../i18n/Rich';
import { label, t } from '../../i18n/strings';
import type { PlotEntry, PlotsTabState } from '../../types';
import { PlotChain, PlotChainLegend } from '../PlotChain';

// PlotsTab — a budget header above a vertically scrolling list of plot
// cards. Active plots first, then a "recently ended" section holding
// terminal plots until they are reaped, because the question worth
// answering after the fact is almost always *why did that one fail* and
// the answer is gone if the card disappears the moment the plot ends.
//
// This is a debugging instrument first. It is also the primary
// verification surface for Phase A: reading it over accelerated in-world
// time is how the resolution math, the birth rule and the casting
// distribution get judged before any content is attached.

declare global {
    interface Window {
        ne_forcePlotTicks?: (count: string) => void;
        ne_setPlotTicksEnabled?: (enabled: string) => void;
        // Seeds a premade College plot. See PlotTestPlots.h.
        ne_seedTestPlot?: (which: string) => void;
    }
}

function forceTicks(count: number) {
    window.ne_forcePlotTicks?.(String(count));
}

function setTicksEnabled(enabled: boolean) {
    window.ne_setPlotTicksEnabled?.(enabled ? 'true' : 'false');
}

function seedTestPlot(which: string) {
    window.ne_seedTestPlot?.(which);
}

export function PlotsTab({ plots }: { plots: PlotsTabState }): ReactNode {
    // Collapsed by default: the faction list is reference, rarely what
    // the tab is opened to see, and it is long enough to bury the plots.
    const [factionsOpen, setFactionsOpen] = useState(false);
    // Newest first, in both sections. The backend publishes plots in
    // birth order, which puts the one that just appeared at the bottom
    // of a scrolling list -- exactly where nobody is looking after
    // forcing a tick.
    const newestFirst = (a: PlotEntry, b: PlotEntry) => b.id - a.id;
    const active = plots.list.filter(p => p.status === 'active').sort(newestFirst);
    const ended = plots.list.filter(p => p.status !== 'active').sort(newestFirst);
    const resolved = plots.steps_succeeded + plots.steps_timed_out + plots.steps_caught;
    // A tick born a plot makes a blocking LLM round trip, and an adapted
    // one makes another. Without this the buttons looked idle for the
    // whole wait and invited a second click, which just queued a second
    // burst behind the first.
    const busy = plots.ticks_pending > 0;
    const pct = (n: number) => (resolved > 0 ? t('plots.percent', { n: Math.round((100 * n) / resolved) }) : '—');

    return (
        <div className="plots-tab">
            <div className="plot-header">
                <div className="plot-header-top">
                    <div className="plot-budget">
                        <Rich k="plots.slots_in_use" args={{ active: plots.active, budget: plots.budget }} />
                    </div>
                    <label className="tick-toggle" title={t('plots.simulation_enabled.title')}>
                        <input
                            type="checkbox"
                            checked={plots.ticks_enabled}
                            onChange={e => setTicksEnabled(e.target.checked)}
                        />
                        <span>{t('plots.simulation_enabled')}</span>
                    </label>
                </div>
                <div className="plot-counters">
                    <span>{t('plots.day', { day: plots.sim_game_day.toFixed(1) })}</span>
                    <span>{t('plots.ticks_run', { n: plots.ticks_run })}</span>
                    <span>{t('plots.born', { n: plots.plots_born })}</span>
                    <span>
                        {t('plots.succeeded_failed', { succeeded: plots.plots_succeeded, failed: plots.plots_failed })}
                    </span>
                    <span>{t('plots.adaptations', { n: plots.adaptations })}</span>
                </div>
                <div className="plot-counters">
                    <span>{t('plots.steps_done', { n: plots.steps_succeeded, pct: pct(plots.steps_succeeded) })}</span>
                    <span>{t('plots.steps_timed_out', { n: plots.steps_timed_out, pct: pct(plots.steps_timed_out) })}</span>
                    <span>{t('plots.steps_caught', { n: plots.steps_caught, pct: pct(plots.steps_caught) })}</span>
                </div>
                {plots.debug_buttons && (
                    <div className="plot-debug-actions">
                        <button type="button" disabled={busy} onClick={() => forceTicks(1)}>
                            {t('plots.force_tick')}
                        </button>
                        {/* The largest is a whole tuning run in one press. The
                            plugin caps a burst at 200, so that is the largest
                            there is. */}
                        {[5, 10, 20, 100, 200].map(n => (
                            <button key={n} type="button" disabled={busy} onClick={() => forceTicks(n)}>
                                {t('plots.force_n', { n })}
                            </button>
                        ))}
                        {/* Premade plots, for checking plots in game against a
                            known one. See PlotTestPlots.h. */}
                        <button type="button" disabled={busy} onClick={() => seedTestPlot('brelyna')}>
                            {t('plots.test_brelyna')}
                        </button>
                        <button type="button" disabled={busy} onClick={() => seedTestPlot('tolfdir')}>
                            {t('plots.test_brelyna_agent')}
                        </button>
                        {busy && (
                            <span className="plot-busy">
                                <i className="plot-spinner" />
                                {t('plots.running_ticks', { n: plots.ticks_pending })}
                            </span>
                        )}
                    </div>
                )}
            </div>

            <PlotChainLegend />

            {plots.list.length === 0 && (
                <p className="plot-empty">{t('plots.none_yet')}</p>
            )}

            {active.map(p => (
                <PlotChain key={p.id} plot={p} />
            ))}

            {ended.length > 0 && (
                <>
                    <h4 className="plot-section-heading">{t('plots.recently_ended')}</h4>
                    {ended.map(p => (
                        <PlotChain key={p.id} plot={p} />
                    ))}
                </>
            )}

            {plots.factions.length > 0 && (
                <div className="plot-factions">
                    {/* A real <button>, as the gossip rows use, so it is
                        keyboard-reachable and announces its state. */}
                    <button
                        type="button"
                        className="plot-section-heading plot-section-toggle"
                        onClick={() => setFactionsOpen(v => !v)}
                        aria-expanded={factionsOpen}
                    >
                        <span aria-hidden="true">{factionsOpen ? '▾' : '▸'}</span>
                        {' '}
                        {t('plots.faction_hierarchies', { n: plots.factions.length })}
                    </button>
                    {factionsOpen && (
                        <ul>
                            {plots.factions.map(f => (
                                <li key={f.id}>
                                    <strong>{f.name}</strong>
                                    <span>
                                        {' '}
                                        {f.overrides > 0
                                            ? t('plots.faction_method_overrides', {
                                                  method: label('faction_method.', f.method),
                                                  n: f.overrides,
                                              })
                                            : t('plots.faction_method', { method: label('faction_method.', f.method) })}
                                    </span>
                                </li>
                            ))}
                        </ul>
                    )}
                </div>
            )}
        </div>
    );
}
