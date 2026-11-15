// The dashboard's words: `t(key)` renders the session's [Dashboard] string
// for `key`, from the table C++ sends with every state (see LanguageState in
// types.ts). C++ has already chosen each string — the player's overlay, the
// language's file, or English — and validated it, so the table is complete.
//
// Before the first state arrives there is no table, and `t` answers with the
// key itself; so does a key the table lacks, which is a typo worth seeing.

import { englishGrammar, parse, pluralsAvailable, render, type Arguments, type Message } from './textTemplate';
import type { LanguageState } from '../types';

let current: LanguageState | null = null;
const parsed = new Map<string, Message | null>();

// The language is chosen once per session, so only the first table, or one
// for another language, replaces what is cached.
export function setLanguage(language: LanguageState | undefined): void {
    if (!language || (current && current.code === language.code)) {
        return;
    }
    current = language;
    parsed.clear();
    document.documentElement.lang = language.code;
}

export function language(): LanguageState | null {
    return current;
}

export function has(key: string): boolean {
    return current !== null && Object.prototype.hasOwnProperty.call(current.strings, key);
}

function message(key: string): Message | null {
    let m = parsed.get(key);
    if (m === undefined) {
        const text = current && has(key) ? current.strings[key] : undefined;
        const result = text === undefined ? null : parse(text);
        m = result && result.problems.length === 0 ? result.message : null;
        parsed.set(key, m);
    }
    return m;
}

export function t(key: string, args: Arguments = {}, sentence = false): string {
    const m = current ? message(key) : null;
    if (!current || !m) {
        return key;
    }
    return render(m, args, {
        grammar: { ...englishGrammar, ...current.grammar },
        language: current.code,
        sentence,
    });
}

// `prefix + id`'s label when the table has one, else the id as sent: for ids
// the plugin's own code can add to (a test beat, a phase, an outcome).
export function label(prefix: string, id: string): string {
    return has(prefix + id) ? t(prefix + id) : id;
}

export { pluralsAvailable };
