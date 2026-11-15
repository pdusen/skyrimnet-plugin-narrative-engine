import { Fragment, type ReactNode } from 'react';

import type { Arguments } from './textTemplate';
import { t } from './strings';

// A string whose template marks emphasis with <b>, <strong> or <i>, so that a
// translation can move the emphasised number along with the words around it.
// Those three tags are the whole markup: anything else is text.

const TAG = /<(\/?)(b|strong|i)>/g;

interface Frame {
    tag: string;
    children: ReactNode[];
}

function build(text: string): ReactNode[] {
    const stack: Frame[] = [{ tag: '', children: [] }];
    let at = 0;
    let key = 0;
    for (const match of text.matchAll(TAG)) {
        const top = stack[stack.length - 1]!;
        const index = match.index ?? 0;
        if (index > at) {
            top.children.push(text.slice(at, index));
        }
        at = index + match[0].length;
        const [, closing, tag] = match;
        if (!closing) {
            stack.push({ tag: tag!, children: [] });
        } else if (stack.length > 1 && top.tag === tag) {
            stack.pop();
            const Tag = top.tag as 'b' | 'strong' | 'i';
            stack[stack.length - 1]!.children.push(<Tag key={key++}>{top.children}</Tag>);
        } else {
            top.children.push(match[0]);
        }
    }
    stack[stack.length - 1]!.children.push(text.slice(at));
    // An unclosed tag keeps its text, unemphasised.
    while (stack.length > 1) {
        const open = stack.pop()!;
        stack[stack.length - 1]!.children.push(...open.children);
    }
    return stack[0]!.children;
}

export function Rich({ k, args }: { k: string; args?: Arguments }): ReactNode {
    return <Fragment>{build(t(k, args))}</Fragment>;
}
