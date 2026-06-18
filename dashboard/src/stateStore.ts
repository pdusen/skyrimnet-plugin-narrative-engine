// Tiny pub-sub store for the dashboard's DirectorState. The C++ side
// (PrismaUI's InteropCall) writes via `window.updateFullState`, which
// hands the parsed object (or an error) to `stateStore.set` / `setError`.
// The React App subscribes via useEffect.

import type { DirectorState } from './types';

export interface Snapshot {
    state: DirectorState | null;
    error: string | null;
}

type Listener = () => void;

class StateStore {
    private snapshot: Snapshot = { state: null, error: null };
    private listeners = new Set<Listener>();

    getSnapshot(): Snapshot {
        return this.snapshot;
    }

    set(state: DirectorState): void {
        this.snapshot = { state, error: null };
        this.notify();
    }

    setError(message: string): void {
        this.snapshot = { state: this.snapshot.state, error: message };
        this.notify();
    }

    subscribe(listener: Listener): () => void {
        this.listeners.add(listener);
        return () => {
            this.listeners.delete(listener);
        };
    }

    private notify(): void {
        this.listeners.forEach((l) => l());
    }
}

export const stateStore = new StateStore();
