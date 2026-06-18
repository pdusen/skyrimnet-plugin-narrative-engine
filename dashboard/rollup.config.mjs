// Rollup config for the NarrativeEngine in-game dashboard.
//
// Produces a single IIFE bundle (`dist/dashboard.js`) plus the static
// `index.html` and renamed `dashboard.css` shell. PrismaUI loads
// `dist/index.html` as a browser view; the script tag pulls in the bundle,
// which mounts the React app into `#root` and registers
// `window.updateFullState` for the C++ side to call.

import typescript from '@rollup/plugin-typescript';
import nodeResolve from '@rollup/plugin-node-resolve';
import commonjs from '@rollup/plugin-commonjs';
import replace from '@rollup/plugin-replace';
import copy from 'rollup-plugin-copy';

export default {
    input: 'src/index.tsx',
    output: {
        file: 'dist/dashboard.js',
        format: 'iife',
        name: 'NarrativeEngineDashboard',
        sourcemap: true,
    },
    plugins: [
        // Strip React's dev-only assertions and warnings so the bundle
        // ships production-tuned. Without this, react/react-dom would
        // include a lot of dev scaffolding we don't need at runtime.
        replace({
            preventAssignment: true,
            values: {
                'process.env.NODE_ENV': JSON.stringify('production'),
            },
        }),
        nodeResolve({
            browser: true,
            extensions: ['.js', '.ts', '.tsx'],
        }),
        commonjs(),
        typescript({
            tsconfig: './tsconfig.json',
        }),
        // Mirror the static shell into dist/ so the deploy step has a
        // single source folder to copy. styles.css renames on the way so
        // the deployed name matches dashboard.js.
        copy({
            targets: [
                { src: 'index.html', dest: 'dist' },
                { src: 'styles.css', dest: 'dist', rename: 'dashboard.css' },
            ],
            hook: 'writeBundle',
        }),
    ],
};
