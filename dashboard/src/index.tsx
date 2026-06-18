// Dashboard entry point. Mounts the React app into #root and exposes
// `window.updateFullState(jsonString)` for the C++ side to drive.
//
// PrismaUI calls `InteropCall(view, "updateFullState", jsonString)` from
// DashboardUIManager (Step 16) after each Director ApplyDecision; that
// dispatch ends up calling the function we register here.

import { createRoot } from 'react-dom/client';

import { App } from './App';
import { stateStore } from './stateStore';
import type { DirectorState } from './types';

declare global {
    interface Window {
        updateFullState: (jsonString: string) => void;
    }
}

// PrismaUI renders the view at the game's native pixel resolution, so on
// a 4K monitor 13px text is ~half the size it'd be on a 1080p screen.
// CSS units don't track DPI for us here (devicePixelRatio is always 1
// inside the Ultralight view). Scale via `zoom` against a 1080p baseline
// so the dashboard reads at roughly the same physical size regardless of
// the player's resolution. Clamped so unusual viewport sizes don't
// produce a microscopic or comically huge panel.
const baseWidth = 1920;
const scale = Math.min(3, Math.max(0.8, window.innerWidth / baseWidth));
// `zoom` is non-standard CSS but Ultralight (PrismaUI's renderer) and all
// Chromium-derived engines support it, and unlike `transform: scale()` it
// reflows layout so flex/grid still work correctly.
(document.documentElement.style as CSSStyleDeclaration & { zoom: string }).zoom =
    scale.toString();

const container = document.getElementById('root');
if (!container) {
    throw new Error('NarrativeEngine dashboard: missing #root element');
}

const root = createRoot(container);
root.render(<App />);

window.updateFullState = (jsonString: string): void => {
    try {
        const parsed = JSON.parse(jsonString) as DirectorState;
        stateStore.set(parsed);
    } catch (e) {
        const msg = e instanceof Error ? e.message : String(e);
        stateStore.setError(`updateFullState parse failure: ${msg}`);
    }
};
