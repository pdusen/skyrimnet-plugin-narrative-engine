// Schema contract between the C++ DashboardUIManager (Step 16) and the
// React app. The C++ side composes JSON matching exactly this shape;
// changing this file means a coordinated C++ change is also required.

export type PhaseName =
    | "Exposition"
    | "RisingAction"
    | "Climax"
    | "FallingAction"
    | "Resolution";

export interface DirectorState {
    status: {
        skyrim_net_available: boolean;
        skyrim_net_version: number;
        director_enabled: boolean;
        prisma_ui_available: boolean;
    };
    current_phase: PhaseName;
    time_in_phase_seconds: number;
    last_evaluation: {
        timestamp: number;            // realTimeSec from the DecisionRecord
        tension_score: number;        // 0..100
        narrative_note: string;
        advanced_to: string | null;   // PhaseName when the eval triggered an advance
        alpha_canon_signals: string[];
        // Action selected for this evaluation, if any. Either a snake_case
        // action name (e.g. "ambush") for a successful start, or a string
        // starting with "(failed:" carrying the failure reason. Null when
        // no action fired this tick.
        action: string | null;
    } | null;
    // Currently-running action, or null. `started_at` is Unix-epoch seconds
    // (same time base as `last_evaluation.timestamp`).
    action_in_flight: {
        name: string;
        started_at: number;
    } | null;
    recent_decisions: DecisionEntry[];
    recent_events: EventEntry[];
}

export interface DecisionEntry {
    timestamp: number;
    tension_score: number;
    phase: string;
    action: string | null;
    narrative_note: string;
}

export interface EventEntry {
    // Mirrors the synthesized event payload the C++ side produces — `text`
    // is a human-readable rendering of `data.dialogue` / `data.killer` /
    // etc., depending on `type`. `gameTime` is in-game seconds since the
    // game's calendar epoch; we sort on it to keep "newest first" lined up
    // with the relative-time labels rendered into `text`.
    type: string;
    text: string;
    gameTime: number;
    originatingActorName: string;
    targetActorName: string;
}
