import { useState, type ReactNode } from 'react';

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
    const pct = (n: number) => (resolved > 0 ? `${Math.round((100 * n) / resolved)}%` : '—');

    return (
        <div className="plots-tab">
            <div className="plot-header">
                <div className="plot-header-top">
                    <div className="plot-budget">
                        <strong>
                            {plots.active} / {plots.budget}
                        </strong>
                        <span> slots in use</span>
                    </div>
                    <label
                        className="tick-toggle"
                        title="When off, scheduled plot ticks do not fire and the time they would have covered is skipped. Forced ticks still run."
                    >
                        <input
                            type="checkbox"
                            checked={plots.ticks_enabled}
                            onChange={e => setTicksEnabled(e.target.checked)}
                        />
                        <span>Simulation Enabled</span>
                    </label>
                </div>
                <div className="plot-counters">
                    <span>day {plots.sim_game_day.toFixed(1)}</span>
                    <span>{plots.ticks_run} ticks</span>
                    <span>{plots.plots_born} born</span>
                    <span>
                        {plots.plots_succeeded} succeeded / {plots.plots_failed} failed
                    </span>
                    <span>{plots.adaptations} adaptations</span>
                </div>
                <div className="plot-counters">
                    <span>steps: {plots.steps_succeeded} done ({pct(plots.steps_succeeded)})</span>
                    <span>{plots.steps_timed_out} timed out ({pct(plots.steps_timed_out)})</span>
                    <span>{plots.steps_caught} caught ({pct(plots.steps_caught)})</span>
                </div>
                {plots.debug_buttons && (
                    <div className="plot-debug-actions">
                        <button type="button" disabled={busy} onClick={() => forceTicks(1)}>
                            Force tick
                        </button>
                        <button type="button" disabled={busy} onClick={() => forceTicks(5)}>
                            Force 5
                        </button>
                        <button type="button" disabled={busy} onClick={() => forceTicks(10)}>
                            Force 10
                        </button>
                        <button type="button" disabled={busy} onClick={() => forceTicks(20)}>
                            Force 20
                        </button>
                        {/* A whole tuning run in one press. The plugin caps a
                            burst at 200, so this is the largest there is. */}
                        <button type="button" disabled={busy} onClick={() => forceTicks(100)}>
                            Force 100
                        </button>
                        <button type="button" disabled={busy} onClick={() => forceTicks(200)}>
                            Force 200
                        </button>
                        {/* Premade plots, for checking plots in game against a
                            known one. See PlotTestPlots.h. */}
                        <button type="button" disabled={busy} onClick={() => seedTestPlot('brelyna')}>
                            Test: Brelyna's plot
                        </button>
                        <button type="button" disabled={busy} onClick={() => seedTestPlot('tolfdir')}>
                            Test: Brelyna as agent
                        </button>
                        {busy && (
                            <span className="plot-busy">
                                <i className="plot-spinner" />
                                {plots.ticks_pending === 1
                                    ? 'running a tick…'
                                    : `running ${plots.ticks_pending} ticks…`}
                            </span>
                        )}
                    </div>
                )}
            </div>

            <PlotChainLegend />

            {plots.list.length === 0 && (
                <p className="plot-empty">
                    No plots yet. Plots are born on a tick with a free slot — force a few, or wait for the
                    world to get around to it.
                </p>
            )}

            {active.map(p => (
                <PlotChain key={p.id} plot={p} />
            ))}

            {ended.length > 0 && (
                <>
                    <h4 className="plot-section-heading">Recently ended</h4>
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
                        {` Faction hierarchies (${plots.factions.length})`}
                    </button>
                    {factionsOpen && (
                        <ul>
                            {plots.factions.map(f => (
                                <li key={f.id}>
                                    <strong>{f.name}</strong>
                                    <span>
                                        {' '}
                                        — {f.method}
                                        {f.overrides > 0 && `, ${f.overrides} override${f.overrides === 1 ? '' : 's'}`}
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
