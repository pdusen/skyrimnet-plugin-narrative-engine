import { useState } from 'react';
import type { ReactNode } from 'react';

import { label, t } from '../i18n/strings';
import type { PlotEntry, PlotNode } from '../types';

// PlotChain — one plot as a horizontal chain of numbered nodes.
//
// The chain is the summary; every quantity that decided an outcome lives
// behind its node. A tab that renders only the fiction — who is plotting
// what against whom — is pleasant and cannot diagnose anything, so
// clicking a node expands the arithmetic underneath it.
//
// Colour is never the only signal. A failed node carries a glyph and the
// live node carries its percentage, so the four states stay
// distinguishable without relying on hue.

const NODE = 44; // diameter, px
// Connector length, and with it the column pitch the captions have to
// live inside. Widened from 34 once the caption became a phrase the
// model writes rather than the step type's one-word verb: at a 78px
// pitch every two-word caption wrapped to two cramped lines.
const GAP = 52;
// Blank space held between one caption and the next.
//
// Bounding the captions to a column stopped them OVERLAPPING, but a
// column is exactly the distance between two nodes -- so two captions
// that both fill their width end up touching, and "obtain records" and
// "find gaps" read as one phrase. This is the gutter that keeps them
// legible as two.
const GUTTER = 14;
// A caption gets one whole column less the gutter, centred on its node
// rather than on the column -- so it is pulled left of the node it
// follows by half of what is left over. Bounded, unlike the old
// caption, because nothing constrains how long a written label is.
const LABEL = NODE + GAP - GUTTER;
const RING = 3.5;
const BORDER = 2; // .plot-node border width; ProgressRing has to cancel it

interface ColourSet {
    fill: string;
    stroke: string;
    text: string;
}

function colours(state: PlotNode['state']): ColourSet {
    switch (state) {
        case 'completed':
            return { fill: '#3f8f2f', stroke: '#63c94b', text: '#ffffff' };
        case 'failed':
            return { fill: 'transparent', stroke: '#6d7581', text: '#c8ced6' };
        case 'in_progress':
            return { fill: 'transparent', stroke: '#4b93f5', text: '#ffffff' };
        default:
            return { fill: 'transparent', stroke: '#4a515b', text: '#8b939e' };
    }
}

// The connector LEAVING a node. Green once the chain has moved past it —
// including past a failure, because the plot went on — blue into the live
// node, muted beyond.
function connectorColour(from: PlotNode, to: PlotNode | undefined): string {
    if (!to) {
        return 'transparent';
    }
    if (to.state === 'in_progress') {
        return '#4b93f5';
    }
    if (from.state === 'completed' || from.state === 'failed') {
        return '#3f8f2f';
    }
    return '#3a4049';
}

function ProgressRing({ fraction }: { fraction: number }) {
    // An SVG arc with stroke-dasharray rather than a conic gradient: it is
    // the portable option and it scales cleanly.
    const r = NODE / 2 - RING / 2;
    const circumference = 2 * Math.PI * r;
    const filled = Math.max(0, Math.min(1, fraction)) * circumference;
    // Offset by the border width, not pinned to the padding box.
    //
    // The ring is drawn to sit exactly ON the node's border circle, so its
    // 44px box has to be the button's BORDER box. An absolutely positioned
    // child resolves against the padding box instead, which the global
    // `box-sizing: border-box` makes 40px — so the old `inset: 0` plus a
    // hardcoded 44px SVG anchored the ring 2px down and 2px right of the
    // circle it was meant to overlay, and the two read as two rings.
    return (
        <svg
            className="plot-node-ring"
            width={NODE}
            height={NODE}
            style={{ top: -BORDER, left: -BORDER }}
            aria-hidden="true"
        >
            <circle cx={NODE / 2} cy={NODE / 2} r={r} fill="none" stroke="#3a4049" strokeWidth={RING} />
            <circle
                cx={NODE / 2}
                cy={NODE / 2}
                r={r}
                fill="none"
                stroke="#4b93f5"
                strokeWidth={RING}
                strokeLinecap="round"
                strokeDasharray={`${filled} ${circumference - filled}`}
                // Start at twelve o'clock rather than three.
                transform={`rotate(-90 ${NODE / 2} ${NODE / 2})`}
            />
        </svg>
    );
}

// A plot's title and ambition as a heading and a sentence. Both are the
// model's words, and it starts them in lower case about as often as not:
// it writes them as a goal to slot into a sentence, not as one.
function sentence(text: string): string {
    return text.charAt(0).toUpperCase() + text.slice(1);
}

function num(v: number, places = 2): string {
    return Number.isFinite(v) ? v.toFixed(places) : '—';
}

function NodeDetail({ node }: { node: PlotNode }) {
    const held = node.rolls.filter(r => r.held).length;
    return (
        <div className="plot-node-detail">
            <div className="plot-detail-row">
                <span className="plot-detail-label">{t('plots.detail.race')}</span>
                <span>
                    {t('plots.detail.progress', {
                        progress: num(node.progress),
                        threshold: num(node.threshold),
                        pct: t('plots.percent', { n: Math.round(node.fraction * 100) }),
                    })}
                    {'  ·  '}
                    {t('plots.detail.tick', { elapsed: node.elapsed, budget: node.budget })}
                    {held > 0 && (
                        <>
                            {'  ·  '}
                            {t('plots.detail.held', { n: held })}
                        </>
                    )}
                </span>
            </div>
            <div className="plot-detail-row">
                <span className="plot-detail-label">{t('plots.detail.cast')}</span>
                <span>
                    {node.actor || t('plots.detail.not_dispatched')}
                    {node.target ? ` → ${node.target}` : ''}
                </span>
            </div>
            <div className="plot-detail-row">
                <span className="plot-detail-label">{t('plots.detail.sizing')}</span>
                <span>
                    {t('plots.detail.sizing_values', {
                        travel: num(node.sizing.travel),
                        importance: num(node.sizing.importance),
                        competence: num(node.sizing.competence),
                        suitability: num(node.sizing.suitability),
                    })}
                </span>
            </div>
            {node.rolls.length > 0 && (
                <div className="plot-detail-row">
                    <span className="plot-detail-label">{t('plots.detail.rolls')}</span>
                    <span className="plot-rolls">
                        {node.rolls.map((r, i) => (
                            <span
                                key={i}
                                className={r.caught ? 'plot-roll caught' : r.held ? 'plot-roll held' : 'plot-roll'}
                                title={t('plots.detail.roll', { tick: i + 1, added: num(r.added), after: num(r.after) })}
                            >
                                {r.held ? '–' : r.caught ? '✕' : num(r.added, 1)}
                            </span>
                        ))}
                    </span>
                </div>
            )}
            {/* LAST, deliberately. The rows above are the arithmetic
                that decided the outcome, which is what the panel is
                opened to read; the sentence is context for them, and a
                line of prose at the top pushes the numbers down. */}
            {node.description && (
                <div className="plot-detail-row plot-detail-description">
                    <span className="plot-detail-label">{t('plots.detail.step')}</span>
                    <span>{node.description}</span>
                </div>
            )}
        </div>
    );
}

function ChainNode({
    node,
    next,
    expanded,
    onToggle,
}: {
    node: PlotNode;
    next: PlotNode | undefined;
    expanded: boolean;
    onToggle: () => void;
}) {
    const c = colours(node.state);

    return (
        <div className="plot-node-column">
            <div className="plot-node-row">
                {/* A real <button>, so the node is keyboard-reachable and
                    announces its state. The GossipTab rumor rows do the
                    same for the same reason. */}
                <button
                    type="button"
                    className={`plot-node ${node.state}${expanded ? ' expanded' : ''}`}
                    style={{ width: NODE, height: NODE, background: c.fill, borderColor: c.stroke, color: c.text }}
                    onClick={onToggle}
                    aria-expanded={expanded}
                    aria-label={t('plots.node', {
                        number: node.number,
                        label: node.label,
                        state: t(`plot_state.${node.state}`),
                    })}
                >
                    {node.state === 'in_progress' && <ProgressRing fraction={node.fraction} />}
                    <span className="plot-node-number">{node.number}</span>
                    {node.state === 'in_progress' && (
                        <span className="plot-node-pct">{t('plots.percent', { n: Math.round(node.fraction * 100) })}</span>
                    )}
                    {node.state === 'failed' && (
                        <span className="plot-node-cross" aria-hidden="true">
                            ✕
                        </span>
                    )}
                </button>
                <span
                    className="plot-connector"
                    style={{ width: GAP, background: connectorColour(node, next) }}
                    aria-hidden="true"
                />
            </div>
            {/* The step's short caption, not its description. A phrase
                under a 44px node stays scannable across a chain of
                eight; a sentence does not, and the sentence is one
                click away.

                `title` because the caption is clamped to two lines: a
                model that answers with a clause instead of a phrase
                should still be readable rather than silently cut. */}
            <div
                className="plot-node-label"
                style={{ width: LABEL, marginLeft: -(GAP - GUTTER) / 2 }}
                title={node.type}
            >
                {node.type}
            </div>
        </div>
    );
}

export function PlotChain({ plot }: { plot: PlotEntry }): ReactNode {
    const [expanded, setExpanded] = useState<number | null>(null);
    const node = plot.chain.find(n => n.number === expanded);

    return (
        <div className={`plot-card ${plot.status}`}>
            <div className="plot-card-header">
                <h3 className="plot-title">{sentence(plot.title)}</h3>
                <span className="plot-meta">
                    {plot.mastermind}
                    {plot.adaptations > 0 && (
                        <>
                            {'  ·  '}
                            {t('plots.adapted', { n: plot.adaptations, max: plot.max_adaptations })}
                        </>
                    )}
                    {plot.status !== 'active' && (
                        <>
                            {'  ·  '}
                            {t('plots.ended', {
                                status: label('plot_status.', plot.status),
                                outcome: label('plot_outcome.', plot.outcome),
                            })}
                        </>
                    )}
                </span>
            </div>
            {plot.ambition && <p className="plot-ambition">{sentence(plot.ambition)}</p>}

            {/* A chain wider than its card scrolls horizontally; nodes keep
                their size rather than shrinking to fit. */}
            <div className="plot-chain-scroll">
                {/* Room on the left for the overhang, because the first
                    node's caption is the one thing in the chain that
                    reaches back past its own column. The right needs
                    none: the last node still draws a full-width
                    transparent connector after it. */}
                <div className="plot-chain" style={{ paddingLeft: (GAP - GUTTER) / 2 }}>
                    {plot.chain.map((n, i) => (
                        <ChainNode
                            key={n.number}
                            node={n}
                            next={plot.chain[i + 1]}
                            expanded={expanded === n.number}
                            onToggle={() => setExpanded(expanded === n.number ? null : n.number)}
                        />
                    ))}
                </div>
            </div>

            {/* Outside the button, so a button is never nested in a button.

                Keyed by the node, so moving from one step to another
                replaces the panel rather than rewriting its text in place.
                Ultralight repaints a rewritten text node only as far as the
                new text reaches: the old sentence stayed on screen beneath
                the new one, showing through wherever the new was shorter. */}
            {node && <NodeDetail key={node.number} node={node} />}
        </div>
    );
}

export function PlotChainLegend(): ReactNode {
    return (
        <div className="plot-legend">
            <span>
                <i className="plot-swatch completed" /> {t('plot_state.completed')}
            </span>
            <span>
                <i className="plot-swatch in-progress" /> {t('plot_state.in_progress')}
            </span>
            <span>
                <i className="plot-swatch pending" /> {t('plot_state.pending')}
            </span>
            <span>
                <i className="plot-swatch failed">✕</i> {t('plot_state.failed')}
            </span>
        </div>
    );
}
