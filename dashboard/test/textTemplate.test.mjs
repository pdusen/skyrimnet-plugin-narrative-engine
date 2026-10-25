// The dashboard's formatter against testsupport/fixtures/text_template.json,
// the fixture TextTemplate.fixture.test.cpp runs the C++ formatter through.
// `npm test` compiles src/i18n/textTemplate.ts with tsc and runs this file
// under node --test.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

import { englishGrammar, parse, pluralCategory, render } from '../.test-build/i18n/textTemplate.js';

const fixture = JSON.parse(
    readFileSync(new URL('../../testsupport/fixtures/text_template.json', import.meta.url), 'utf8'),
);

test('every language: Intl.PluralRules gives the fixture\'s CLDR categories', () => {
    for (const { language, categories } of fixture.plurals) {
        for (const [n, category] of Object.entries(categories)) {
            assert.equal(pluralCategory(language, Number(n)), category, `${language} ${n}`);
        }
    }
});

test('renders', () => {
    for (const c of fixture.renders) {
        const parsed = parse(c.template);
        assert.deepEqual(parsed.problems, [], c.name);
        const out = render(parsed.message, c.arguments ?? {}, {
            grammar: { ...englishGrammar, ...(c.grammar ?? {}) },
            language: c.language ?? 'en',
            missing: (name) => c.missing?.[name] ?? '',
            sentence: c.sentence ?? false,
        });
        assert.equal(out, c.expected, c.name);
    }
});

test('rejects', () => {
    for (const c of fixture.rejects) {
        const parsed = parse(c.template);
        assert.notDeepEqual(parsed.problems, [], c.name);
        assert.deepEqual(parsed.message, [], c.name);
    }
});

test('without Intl.PluralRules, every plural takes its other branch', () => {
    const saved = Intl.PluralRules;
    try {
        Intl.PluralRules = undefined;
        const parsed = parse('{n, plural, one {# day} other {# days}}');
        const out = render(parsed.message, { n: 1 }, { grammar: englishGrammar, language: 'en' });
        assert.equal(out, '1 days');
    } finally {
        Intl.PluralRules = saved;
    }
});
