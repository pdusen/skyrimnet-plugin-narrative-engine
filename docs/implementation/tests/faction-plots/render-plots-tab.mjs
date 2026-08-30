// Render the Plots tab to a string and assert the markup, without a browser.
//
// The chain widget's correctness is not "does it compile" -- it is whether a
// failed node keeps its number, whether the progress ring's arc matches the
// fraction, and whether the nodes left of the cursor renumber when adaptation
// rewrites the tail. None of that is visible from the TypeScript and all of it
// is one string search away once the component has actually run.
//
// Whether it LOOKS right is Step 11's business.
//
// Usage:  node render-plots-tab.mjs
// Step 10 of docs/implementation/PHASE_14_FACTION_PLOTS.md.

import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createRequire } from 'node:module';

const here = dirname(fileURLToPath(import.meta.url));
const repo = join(here, '..', '..', '..', '..');
const dash = join(repo, 'dashboard');
const require = createRequire(join(dash, 'package.json'));

const React = require('react');
const { renderToStaticMarkup } = require('react-dom/server');
const ts = require('typescript');

let failures = 0;
function expect(cond, what) {
    if (!cond) {
        console.log(`  FAIL  ${what}`);
        failures += 1;
    }
}

// Compile the components on the fly. Rollup builds a browser bundle; this
// needs CommonJS modules it can require, and transpiling the three files
// involved is cheaper than maintaining a second build config.
const cache = new Map();
function load(relPath) {
    if (cache.has(relPath)) return cache.get(relPath);
    const full = join(dash, 'src', relPath);
    const source = readFileSync(full, 'utf8');
    const js = ts.transpileModule(source, {
        compilerOptions: {
            jsx: ts.JsxEmit.React,
            module: ts.ModuleKind.CommonJS,
            target: ts.ScriptTarget.ES2020,
        },
    }).outputText;

    const module = { exports: {} };
    const localRequire = (spec) => {
        if (spec === 'react') return React;
        if (spec.startsWith('.')) {
            // Resolve relative to the importing file, and add .tsx.
            const base = join(dirname(relPath), spec).replace(/\\/g, '/');
            return load(base.endsWith('.tsx') ? base : `${base}.tsx`);
        }
        return require(spec);
    };
    // `React` has to be in scope: ts.JsxEmit.React emits
    // React.createElement calls, and the source files import only types.
    new Function('require', 'module', 'exports', 'React', js)(
        localRequire, module, module.exports, React);
    cache.set(relPath, module.exports);
    return module.exports;
}

const { PlotsTab } = load('components/tabs/PlotsTab.tsx');
const sample = JSON.parse(readFileSync(join(dash, 'src', 'fixtures', 'plots.sample.json'), 'utf8'));
const empty = JSON.parse(readFileSync(join(dash, 'src', 'fixtures', 'plots.empty.json'), 'utf8'));

const html = renderToStaticMarkup(React.createElement(PlotsTab, { plots: sample }));

// --- the chain renders one node per entry ---------------------------------
const amulet = sample.list[0];
const nodeCount = (html.match(/class="plot-node /g) || []).length;
expect(nodeCount === amulet.chain.length + 15 + 1, `one node per chain entry (got ${nodeCount})`);

// --- a failed node keeps its number AND carries a glyph --------------------
//
// Colour is never the only signal, and a failed step must stay in the
// position it occupied rather than vanishing when the plot moves on.
expect(html.includes('Step 4, Suborn the Guard, failed'), 'the failed node is labelled with its number');
expect(html.includes('plot-node-cross'), 'the failed node carries the cross glyph');
expect((html.match(/plot-node-cross/g) || []).length >= 1, 'at least one cross rendered');

// --- the live node shows its ring AND its percentage ----------------------
expect(html.includes('plot-node-ring'), 'the live node draws a progress ring');
expect(html.includes('42%'), 'and states its percentage, so colour is not the only signal');

// The arc length must match the fraction. r = 44/2 - 3.5/2 = 20.25.
const circumference = 2 * Math.PI * 20.25;
const wanted = (0.42 * circumference).toFixed(2).slice(0, 5);
const dashArrays = [...html.matchAll(/stroke-dasharray="([\d.]+) ([\d.]+)"/g)];
expect(dashArrays.length >= 1, 'the ring uses stroke-dasharray rather than a conic gradient');
expect(
    dashArrays.some(m => Number(m[1]).toFixed(2).startsWith(wanted.slice(0, 4))),
    `the arc length matches the 42% fraction (wanted ~${wanted}, got ${dashArrays.map(m => m[1]).join(', ')})`
);

// --- numbering runs over history ++ live ++ remaining, not the plan --------
//
// The property that breaks if the chain is built from the plan array:
// adaptation rewrites the tail, and nodes behind the cursor must not shift.
for (let i = 1; i <= 9; i += 1) {
    expect(html.includes(`>${i}</span>`), `node ${i} is numbered`);
}

// --- a long chain scrolls rather than shrinking ---------------------------
expect(html.includes('plot-chain-scroll'), 'the chain sits in a horizontally scrollable container');

// --- terminal plots are kept, in their own section ------------------------
expect(html.includes('Recently ended'), 'terminal plots are retained under their own heading');
expect(html.includes('adaptation cap hit'), 'and say why they ended');

// --- accessibility: real buttons, and the detail is OUTSIDE them ----------
expect(html.includes('aria-expanded="false"'), 'nodes are real buttons announcing their state');
expect(!/<button[^>]*>(?:(?!<\/button>)[\s\S])*<button/.test(html), 'no button is nested inside another');

// --- the empty state renders rather than throwing -------------------------
const emptyHtml = renderToStaticMarkup(React.createElement(PlotsTab, { plots: empty }));
expect(emptyHtml.includes('No plots yet'), 'an empty list renders the empty state');
expect(!emptyHtml.includes('plot-node '), 'and draws no nodes');

if (failures === 0) {
    console.log('plots tab render: OK');
    process.exit(0);
}
console.log(`plots tab render: ${failures} FAILURE(S)`);
process.exit(1);
