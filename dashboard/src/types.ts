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
        // Runtime debug killswitch. When false, the C++ Tick module skips
        // its main-thread poll — no phase advance, no evaluation, no
        // dispatcher / combat-log ticking. Toggled via the checkbox in
        // StatusBanner which calls `window.ne_setTickEnabled('true'|'false')`.
        tick_enabled: boolean;
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
    letter_pool: LetterPoolState;
    actions: ActionInfo[];
    visit: VisitTabState;
}

// Visit tab payload — populated by the C++ DashboardUIManager per
// Phase 05 Step 16. `current` is null when no visit is in flight;
// `recent_verdicts` and `history` are per-process rings.
export type VisitMode =
    | 'idle'
    | 'composing'
    | 'salutation'
    | 'discuss'
    | 'on_hold'
    | 'reengage'
    | 'valediction'
    | 'return_home';

export type VisitOutcome =
    | 'completed'
    | 'unsatisfied'
    | 'rolled_back'
    | 'aborted';

export interface VisitCurrent {
    mode: VisitMode;
    sender_form_id: number;
    topic_tag: string;
    mood: string;
    briefing_preview: string;
    dispatched_at: number;
    ignore_nudge_count: number;
}

export interface VisitVerdict {
    fired_at: number;
    should_conclude: boolean;
    rationale: string;
}

export interface VisitHistoryEntry {
    dispatched_at: number;
    sender_name: string;
    topic_tag: string;
    outcome: VisitOutcome;
    duration_seconds: number;
}

export interface VisitTabState {
    current: VisitCurrent | null;
    recent_verdicts: VisitVerdict[];
    history: VisitHistoryEntry[];
}

// One entry per action registered with ActionRegistry. The C++ side
// emits these in registration order.
export interface ActionInfo {
    name: string;                       // snake_case (e.g. "npc_letter")
    enabled: boolean;                   // false = filtered out of candidate list
    last_dispatched_at: number;         // Unix-epoch seconds; 0 = never (session)
    remaining_cooldown_hours: number;   // in-game hours; 0 = fireable now
}

export type LetterSlotState =
    | 'free'
    | 'pending_delivery'
    | 'in_inventory'
    | 'read';

export interface LetterSlot {
    index: number;                  // 0..19
    state: LetterSlotState;
    letter_label: string;           // empty for free
    topic_tag: string;              // empty for free
    mood: string;                   // empty for free
    body_preview: string;           // empty for free; first ~200 chars
    delivered_at: number;           // 0 for free; Unix-epoch seconds
    read_at: number;                // 0 for free / pending / inventory
}

export interface LetterPoolState {
    slots: LetterSlot[];
    // Index of the slot to feature in the recent-dispatch detail, or
    // null when every slot is free.
    most_recent_dispatch_slot: number | null;
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
