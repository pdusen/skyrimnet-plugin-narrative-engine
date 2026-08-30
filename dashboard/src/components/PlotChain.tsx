import { useState } from 'react';
import type { ReactNode } from 'react';

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
const GAP = 34; // connector length between nodes
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

function num(v: number, places = 2): string {
    return Number.isFinite(v) ? v.toFixed(places) : '—';
}

function NodeDetail({ node }: { node: PlotNode }) {
    const held = node.rolls.filter(r => r.held).length;
    return (
        <div className="plot-node-detail">
            <div className="plot-detail-row">
                <span className="plot-detail-label">race</span>
                <span>
                    progress {num(node.progress)} / {num(node.threshold)} ({Math.round(node.fraction * 100)}%)
                    {'  ·  '}
                    tick {node.elapsed} / {node.budget}
                    {held > 0 ? `  ·  held ${held}` : ''}
                </span>
            </div>
            <div className="plot-detail-row">
                <span className="plot-detail-label">cast</span>
                <span>
                    {node.actor || '(not yet dispatched)'}
                    {node.target ? ` → ${node.target}` : ''}
                </span>
            </div>
            <div className="plot-detail-row">
                <span className="plot-detail-label">sizing</span>
                <span>
                    travel {num(node.sizing.travel)} · importance {num(node.sizing.importance)} · competence{' '}
                    {num(node.sizing.competence)} · suitability {num(node.sizing.suitability)}
                </span>
            </div>
            {node.rolls.length > 0 && (
                <div className="plot-detail-row">
                    <span className="plot-detail-label">rolls</span>
                    <span className="plot-rolls">
                        {node.rolls.map((r, i) => (
                            <span
                                key={i}
                                className={r.caught ? 'plot-roll caught' : r.held ? 'plot-roll held' : 'plot-roll'}
                                title={`tick ${i + 1}: +${num(r.added)} → ${num(r.after)}`}
                            >
                                {r.held ? '–' : r.caught ? '✕' : num(r.added, 1)}
                            </span>
                        ))}
                    </span>
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
    const label = node.label.split(' ');
    const firstLine = label.slice(0, Math.ceil(label.length / 2)).join(' ');
    const secondLine = label.slice(Math.ceil(label.length / 2)).join(' ');

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
                    aria-label={`Step ${node.number}, ${node.label}, ${node.state.replace('_', ' ')}`}
                >
                    {node.state === 'in_progress' && <ProgressRing fraction={node.fraction} />}
                    <span className="plot-node-number">{node.number}</span>
                    {node.state === 'in_progress' && (
                        <span className="plot-node-pct">{Math.round(node.fraction * 100)}%</span>
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
            <div className="plot-node-label" style={{ width: NODE + GAP }}>
                <div>{firstLine}</div>
                {secondLine && <div>{secondLine}</div>}
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
                <h3 className="plot-title">{plot.title}</h3>
                <span className="plot-meta">
                    {plot.mastermind}
                    {plot.adaptations > 0 && `  ·  ${plot.adaptations}/${plot.max_adaptations} adaptations`}
                    {plot.status !== 'active' && `  ·  ${plot.status} (${plot.outcome.replace(/_/g, ' ')})`}
                </span>
            </div>
            {plot.ambition && <p className="plot-ambition">{plot.ambition}</p>}

            {/* A chain wider than its card scrolls horizontally; nodes keep
                their size rather than shrinking to fit. */}
            <div className="plot-chain-scroll">
                <div className="plot-chain">
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

            {/* Outside the button, so a button is never nested in a button. */}
            {node && <NodeDetail node={node} />}
        </div>
    );
}

export function PlotChainLegend(): ReactNode {
    return (
        <div className="plot-legend">
            <span>
                <i className="plot-swatch completed" /> Completed
            </span>
            <span>
                <i className="plot-swatch in-progress" /> In progress
            </span>
            <span>
                <i className="plot-swatch pending" /> Pending
            </span>
            <span>
                <i className="plot-swatch failed">✕</i> Failed
            </span>
        </div>
    );
}
