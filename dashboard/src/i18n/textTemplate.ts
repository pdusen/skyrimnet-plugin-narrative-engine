// The dashboard's port of the plugin's template formatter (TextTemplate.h),
// for the dashboard's own strings. It must render exactly as the C++ does;
// both run testsupport/fixtures/text_template.json (`npm test` here).
//
// The syntax is the same small subset of ICU MessageFormat:
//
//   {agent}                                     the argument's text; a person's name
//   {agent, select, female {...} male {...} other {...}}
//                                               on a person, their sex; on text, its value
//   {days, plural, one {# day} other {# days}}  the CLDR plural category; # is the number
//   {names, list}                               a list, joined with the language's separators
//   {scheme, phrase}                            text trimmed to sit inside a sentence
//
// Two things differ in where they come from, not in what they give:
//
// - Plural categories come from the browser's Intl.PluralRules for the
//   language's code, where the C++ uses its hand-written families; the
//   fixture holds both to CLDR. Without Intl.PluralRules every plural takes
//   its `other` branch.
// - Case mapping uses the browser's, kept to utf8proc's simple one-letter
//   mapping: a letter whose full mapping is several letters keeps itself,
//   except for the few that have a simple mapping, which kSimpleUpper and
//   kSimpleLower supply. A letter newer than the browser's Unicode is left
//   alone.
//
// List joining does not use Intl.ListFormat: the separators are the language
// file's own, overlay edits included.

export type Sex = 'male' | 'female' | 'other';

export interface Person {
    name: string;
    sex: Sex;
}

export type Value = string | Person | number | string[];
export type Arguments = Record<string, Value>;

export interface Grammar {
    listSeparator: string;
    listPairSeparator: string;
    listFinalSeparator: string;
    capitalizeSentenceStart: boolean;
    phraseTrailingPunctuation: string;
    phraseLowercaseFirst: boolean;
    // Words that elide before a value opening with a vowel (French "de" ->
    // "d'Aela"); see TextTemplate.h's Grammar.
    elideBeforeVowel: string[];
}

export const englishGrammar: Grammar = {
    listSeparator: ', ',
    listPairSeparator: ' and ',
    listFinalSeparator: ', and ',
    capitalizeSentenceStart: true,
    phraseTrailingPunctuation: '.!?;,',
    phraseLowercaseFirst: true,
    elideBeforeVowel: [],
};

export interface RenderOptions {
    grammar: Grammar;
    // The language's code, for its plural rules.
    language: string;
    // What an argument with no value renders as, by its name. An unfilled
    // select or plural takes its `other` branch.
    missing?: (name: string) => string;
    // Upper-case the first letter of the result, when the grammar does.
    sentence?: boolean;
}

type Node = string | Placeholder | PluralNumber;

interface Branch {
    key: string;
    message: Message;
}

interface Placeholder {
    kind: 'placeholder';
    name: string;
    format: 'simple' | 'select' | 'plural' | 'list' | 'phrase';
    branches: Branch[];
}

interface PluralNumber {
    kind: 'number';
}

export type Message = Node[];

export interface Parsed {
    message: Message;
    problems: string[];
}

const isNameStart = (c: string): boolean => (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c === '_';
const isNameChar = (c: string): boolean => isNameStart(c) || (c >= '0' && c <= '9');
const isAsciiSpace = (c: string): boolean =>
    c === ' ' || c === '\t' || c === '\n' || c === '\r' || c === '\f' || c === '\v';

// A recursive-descent reader over one template, as TextTemplate.cpp's. Offsets
// in problems count UTF-16 units, not bytes.
class Reader {
    private at = 0;
    private problem = '';

    constructor(private readonly text: string) {}

    read(): Parsed {
        let message = this.readMessage(false, false);
        if (!this.failed() && this.at < this.text.length) {
            this.fail(`a '}' that closes nothing at ${this.at}`);
        }
        if (this.failed()) {
            message = [];
            return { message, problems: [this.problem] };
        }
        return { message, problems: [] };
    }

    private fail(why: string): void {
        if (!this.problem) {
            this.problem = why;
        }
    }

    private failed(): boolean {
        return this.problem !== '';
    }

    private peek(): string {
        return this.text.charAt(this.at);
    }

    private skipSpace(): void {
        while (this.at < this.text.length && isAsciiSpace(this.peek())) {
            ++this.at;
        }
    }

    private readMessage(nested: boolean, inPlural: boolean): Message {
        const out: Message = [];
        let text = '';
        const flush = (): void => {
            if (text) {
                out.push(text);
                text = '';
            }
        };
        while (this.at < this.text.length && !this.failed()) {
            const c = this.peek();
            if (c === '}') {
                if (!nested) {
                    break;
                }
                flush();
                return out;
            }
            if (c === '{') {
                flush();
                const placeholder = this.readPlaceholder();
                if (!this.failed()) {
                    out.push(placeholder);
                }
                continue;
            }
            if (c === '#' && inPlural) {
                flush();
                out.push({ kind: 'number' });
                ++this.at;
                continue;
            }
            text += c;
            ++this.at;
        }
        if (nested && !this.failed()) {
            this.fail('a branch that is never closed');
        }
        flush();
        return out;
    }

    private readName(): string {
        const start = this.at;
        if (this.at < this.text.length && isNameStart(this.peek())) {
            ++this.at;
            while (this.at < this.text.length && isNameChar(this.peek())) {
                ++this.at;
            }
        }
        return this.text.slice(start, this.at);
    }

    private readKey(): string {
        const start = this.at;
        while (this.at < this.text.length && isNameChar(this.peek())) {
            ++this.at;
        }
        return this.text.slice(start, this.at);
    }

    private expect(c: string, what: string): boolean {
        this.skipSpace();
        if (this.at < this.text.length && this.peek() === c) {
            ++this.at;
            return true;
        }
        this.fail(`expected ${what} at ${this.at}`);
        return false;
    }

    private readPlaceholder(): Placeholder {
        const open = this.at;
        ++this.at; // '{'
        const out: Placeholder = { kind: 'placeholder', name: '', format: 'simple', branches: [] };
        this.skipSpace();
        out.name = this.readName();
        if (!out.name) {
            this.fail(`a '{' at ${open} that opens no placeholder`);
            return out;
        }
        this.skipSpace();
        if (this.at < this.text.length && this.peek() === '}') {
            ++this.at;
            return out;
        }
        if (!this.expect(',', "',' or '}'")) {
            return out;
        }
        this.skipSpace();
        const format = this.readName();
        if (format === 'list' || format === 'phrase' || format === 'select' || format === 'plural') {
            out.format = format;
        } else {
            this.fail(`'${format}' is not a format, in the placeholder at ${open}`);
            return out;
        }
        if (out.format === 'list' || out.format === 'phrase') {
            this.expect('}', "'}'");
            return out;
        }
        if (!this.expect(',', "',' before the branches")) {
            return out;
        }
        while (!this.failed()) {
            this.skipSpace();
            if (this.at < this.text.length && this.peek() === '}') {
                ++this.at;
                break;
            }
            const key = this.readKey();
            if (!key) {
                this.fail(`expected a branch key or '}' at ${this.at}`);
                break;
            }
            if (!this.expect('{', "'{' opening a branch")) {
                break;
            }
            const message = this.readMessage(true, out.format === 'plural');
            if (!this.failed()) {
                ++this.at; // the branch's '}'
                out.branches.push({ key, message });
            }
        }
        if (!this.failed() && out.branches.length === 0) {
            this.fail(`a ${format} with no branches, in the placeholder at ${open}`);
        }
        return out;
    }
}

export function parse(text: string): Parsed {
    return new Reader(text).read();
}

// Intl.PluralRules per language code, or null where the browser has none.
const pluralRules = new Map<string, Intl.PluralRules | null>();

export function pluralsAvailable(): boolean {
    return typeof Intl !== 'undefined' && typeof Intl.PluralRules === 'function';
}

export function pluralCategory(language: string, n: number): string {
    if (!pluralsAvailable()) {
        return 'other';
    }
    let rules = pluralRules.get(language);
    if (rules === undefined) {
        try {
            rules = new Intl.PluralRules(language);
        } catch {
            rules = null;
        }
        pluralRules.set(language, rules);
    }
    return rules ? rules.select(n) : 'other';
}

// utf8proc's simple mappings where the browser's full mapping is several
// letters (ß is "SS" there, ᾳ "ΑΙ", İ "i̇").
const kSimpleUpper = new Map<number, number>([
    [0x00df, 0x1e9e],
    [0x1fb3, 0x1fbc],
    [0x1fc3, 0x1fcc],
    [0x1ff3, 0x1ffc],
    ...[0x1f80, 0x1f90, 0x1fa0].flatMap((base) =>
        Array.from({ length: 8 }, (_, i): [number, number] => [base + i, base + i + 8]),
    ),
]);
const kSimpleLower = new Map<number, number>([[0x0130, 0x0069]]);

function mapFirst(text: string, simple: Map<number, number>, full: (c: string) => string): string {
    const first = text.codePointAt(0);
    if (first === undefined) {
        return text;
    }
    const c = String.fromCodePoint(first);
    const rest = text.slice(c.length);
    const special = simple.get(first);
    if (special !== undefined) {
        return String.fromCodePoint(special) + rest;
    }
    const mapped = full(c);
    return [...mapped].length === 1 ? mapped + rest : text;
}

const uppercaseFirst = (text: string): string => mapFirst(text, kSimpleUpper, (c) => c.toUpperCase());
const lowercaseFirst = (text: string): string => mapFirst(text, kSimpleLower, (c) => c.toLowerCase());

function isSpace(c: number): boolean {
    return (
        c === 0x20 ||
        (c >= 0x09 && c <= 0x0d) ||
        c === 0x00a0 ||
        (c >= 0x2000 && c <= 0x200a) ||
        c === 0x202f ||
        c === 0x205f ||
        c === 0x3000
    );
}

// `phrase` on its own: what `{x, phrase}` makes of `text`.
export function phrase(text: string, grammar: Grammar): string {
    const set = new Set([...grammar.phraseTrailingPunctuation].map((c) => c.codePointAt(0)));
    const points = [...text];
    let start = 0;
    while (start < points.length && isSpace(points[start]!.codePointAt(0)!)) {
        ++start;
    }
    let end = start;
    for (let at = start; at < points.length; ++at) {
        const c = points[at]!.codePointAt(0)!;
        if (!isSpace(c) && !set.has(c)) {
            end = at + 1;
        }
    }
    const trimmed = points.slice(start, end).join('');
    return grammar.phraseLowercaseFirst ? lowercaseFirst(trimmed) : trimmed;
}

// `list` on its own: what `{x, list}` makes of `items`.
export function joinList(items: readonly string[], grammar: Grammar): string {
    let out = '';
    items.forEach((item, i) => {
        if (i > 0) {
            if (items.length === 2) {
                out += grammar.listPairSeparator;
            } else if (i + 1 === items.length) {
                out += grammar.listFinalSeparator;
            } else {
                out += grammar.listSeparator;
            }
        }
        out += item;
    });
    return out;
}

const isPerson = (value: Value): value is Person => typeof value === 'object' && !Array.isArray(value);

function branchFor(placeholder: Placeholder, key: string): Message | null {
    let other: Message | null = null;
    for (const branch of placeholder.branches) {
        if (branch.key === key) {
            return branch.message;
        }
        if (branch.key === 'other') {
            other = branch.message;
        }
    }
    return other;
}

const kVowels = new Set([...'aeiouyàâäéèêëîïôöùûüÿæœáí']);

const fold = (s: string): string => s.toLowerCase();

// A trailing "<word> " turned into "<wor>'" when the grammar elides the word,
// which keeps its own case. As TextTemplate.cpp's Elide.
function elide(out: string, grammar: Grammar): string {
    if (out.length < 2 || !out.endsWith(' ')) {
        return out;
    }
    const end = out.length - 1;
    let start = end;
    while (start > 0 && out[start - 1] !== ' ' && out[start - 1] !== '\n' && out[start - 1] !== '(') {
        --start;
    }
    const word = out.slice(start, end);
    if (!word || !grammar.elideBeforeVowel.some(w => fold(w) === fold(word))) {
        return out;
    }
    return out.slice(0, end - 1) + "'";
}

export function render(message: Message, args: Arguments, options: RenderOptions): string {
    const missing = (name: string): string => (options.missing ? options.missing(name) : '');
    const asText = (value: Value): string => {
        if (typeof value === 'string') {
            return value;
        }
        if (typeof value === 'number') {
            return String(value);
        }
        if (Array.isArray(value)) {
            return joinList(value, options.grammar);
        }
        return value.name;
    };
    // One buffer through every branch, as the C++ renders: elision reads what
    // came before a value, wherever it was written.
    let out = '';
    const insert = (text: string): void => {
        const first = text.codePointAt(0);
        if (options.grammar.elideBeforeVowel.length > 0 && first !== undefined
            && kVowels.has(String.fromCodePoint(first).toLowerCase())) {
            out = elide(out, options.grammar);
        }
        out += text;
    };
    const into = (nodes: Message, number: number | null): void => {
        for (const node of nodes) {
            if (typeof node === 'string') {
                out += node;
            } else if (node.kind === 'number') {
                if (number !== null) {
                    out += String(number);
                }
            } else {
                placeholder(node, number);
            }
        }
    };
    const placeholder = (p: Placeholder, number: number | null): void => {
        const value = Object.prototype.hasOwnProperty.call(args, p.name) ? args[p.name] : undefined;
        switch (p.format) {
            case 'simple':
            case 'list':
                insert(value !== undefined ? asText(value) : missing(p.name));
                return;
            case 'phrase':
                if (value === undefined) {
                    insert(missing(p.name));
                    return;
                }
                insert(typeof value === 'string' ? phrase(value, options.grammar) : asText(value));
                return;
            case 'select': {
                let key = 'other';
                if (value !== undefined) {
                    key = isPerson(value) ? value.sex : asText(value);
                }
                const branch = branchFor(p, key);
                if (branch) {
                    into(branch, number);
                }
                return;
            }
            case 'plural': {
                const n = typeof value === 'number' ? value : null;
                const branch = branchFor(p, n !== null ? pluralCategory(options.language, n) : 'other');
                if (branch) {
                    into(branch, n);
                }
                return;
            }
        }
    };
    into(message, null);
    return options.sentence && options.grammar.capitalizeSentenceStart ? uppercaseFirst(out) : out;
}
