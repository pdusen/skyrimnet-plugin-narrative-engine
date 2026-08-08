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
    gossip: GossipTabState;
    settings: SettingsTabState;
}

// Settings tab payload — populated by the C++ DashboardUIManager per
// Phase 08. The write surface (see WriteMcmOverride in Settings.cpp) is
// narrower than the fields represented here — every field is a live
// mirror of Config, but only the ones with a UI control in SettingsTab
// have a writer wired up.
export interface SettingsTabState {
    debug_mode: boolean;
    // Human-readable display of the current dashboard hotkey binding
    // (e.g. "F7", "Ctrl+F7"). Composed on the C++ side so the friendly
    // name matches what the input sink actually matches against.
    dashboard_hotkey_display: string;
    // When true, HotkeySink is in temporary capture mode: the next
    // non-modifier keypress becomes the new binding. Drives the Rebind
    // modal's visibility as a pure function of server state.
    dashboard_hotkey_capture_active: boolean;
    // Duplicated on status.tick_enabled for the Dispatch tab; both
    // mount points read from the same underlying Tick::IsEnabled().
    tick_enabled: boolean;
    tick_interval_seconds: number;
    // Uniform floor (all phases) on how many unpaused real-time seconds
    // the current phase must run before PhaseTracker::EvaluateAdvance is
    // allowed to transition. Distinct from ideal_duration_seconds below,
    // which gates whether the beat system may fire an event.
    min_phase_duration_seconds: number;
    ideal_duration_seconds: {
        exposition: number;
        rising_action: number;
        climax: number;
        falling_action: number;
        resolution: number;
    };
    // Upper bounds on how much sender context renders into the letter
    // compose prompt (narrative_engine_letter_compose.prompt). Users
    // running local LLMs against a tight context window can dial these
    // down to shrink the rendered prompt.
    letter_compose_memory_render_cap: number;
    letter_compose_dialogue_render_cap: number;
    // Upper bounds on the visit compose prompt's content
    // (narrative_engine_visit_compose.prompt).
    visit_compose_memory_render_cap: number;
    visit_compose_dialogue_render_cap: number;
    // Upper bounds on the beat-select prompt's content
    // (narrative_engine_action_select.prompt). Same
    // shrink-for-local-LLMs motivation as the compose caps.
    action_select_event_render_cap: number;
    action_select_letter_memory_render_cap: number;
    action_select_visit_memory_render_cap: number;
    // Upper bounds on the story-eval (per-tick tension score) prompt's
    // content (narrative_engine_story_eval.prompt).
    story_eval_event_tail_size: number;
    story_eval_decision_log_tail_size: number;
    // Cap on the recent-lines block in the per-poll visit-conclusion
    // LLM check (narrative_engine_visit_conclusion_poll.prompt).
    visit_conclusion_poll_recent_lines_render_cap: number;
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

// --- Gossip tab -----------------------------------------------------------

export interface RumorEntry {
    id: number;
    // Band 0 — the freshest telling, and the rumor's identity in the list.
    // Empty only if generation somehow produced no bands, in which case the
    // list falls back to naming the source memory.
    text: string;
    // Every generation band, shown in the expanded row. Band selection at
    // transmission time is min(generation / 3, bands - 1).
    bands: string[];
    // True when every still-infectious carrier has run out of named
    // contacts who do not already carry the rumor. Live but going nowhere.
    // See GossipSim::RumorView for why the province channel is excluded.
    stalled: boolean;
    // False only between the last carrier retiring and the reap at the end
    // of that same poll, so in practice always true here.
    live: boolean;
    notability: number;             // 0..1, the per-conversation β
    age_days: number;               // in-world days since seeding
    idle_days: number;              // in-world days since the last telling
    carriers: number;               // everyone who has ever held it
    active_carriers: number;        // still infectious
    settlements: number;
    holds: number;
    max_depth: number;              // deepest generation reached
    transmissions: number;
    wasted: number;                 // tellings that landed on someone who knew
    origin_name: string;
    origin_location: string;
    source_memory_id: number;
}

export interface GossipTabState {
    enabled: boolean;               // bGossipEnabled
    harvest_enabled: boolean;       // bGossipHarvestEnabled
    graph_ready: boolean;
    participants: number;
    queued_events: number;
    transmissions_session: number;
    wasted_session: number;
    memories_written: number;
    harvest_sweeps: number;
    harvest_sent_for_generation: number;
    claims_outstanding: number;
    // Newest first. Holds exactly the rumors that have not been reaped —
    // a rumor is listed until its last carrier retires.
    rumors: RumorEntry[];
}
