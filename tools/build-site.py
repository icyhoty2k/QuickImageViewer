#!/usr/bin/env python3
"""
Build the QuickImageViewer site. One command, two jobs.

    python build-site.py            build everything
    python build-site.py --check    build nothing; exit 1 if anything is stale

JOB 1 — INJECT THE SHARED BLOCKS
--------------------------------
The header, footer and store row were copied into five HTML files and kept in
step by hand. They did not stay in step. They live in docs/_partials/ now, and
each page carries a fence this script fills:

    <!-- SHARED-FOOTER-START -->   ...replaced...   <!-- SHARED-FOOTER-END -->

A page opts IN by having the fence, which is how index.html keeps its own hero
variant of the store row while the document pages share one. Adding a new shared
block needs no edit to this script: drop <name>.html in _partials/ and put a
<!-- SHARED-<NAME>-START/END --> fence wherever it belongs.

JOB 2 — GENERATE THE SHORTCUT REFERENCE
---------------------------------------
docs/shortcuts.html is generated from src/UI/FloatingPanels/HelpWnd.cpp, which
is the app's own help panel and therefore the only thing that cannot be wrong:
its labels are built from Shortcuts.h constants through K()/Ctrl()/Shift(), so
remapping a key changes the help text automatically.

WHY A NEW PAGE RATHER THAN GENERATING INTO README.md AND index.html. Those two
were considered and rejected on the evidence: HelpWnd defines 207 shortcuts,
README documents 102 and index.html mentions 31 across 30 different tables. They
are not three copies that drifted — they are three deliberately different
documents. The app lists everything; the README lists the useful subset; the
landing page shows a taste inside tables that are each part of a surrounding
argument. Generating all 207 into the landing page would replace 30 contextual
tables with one reference dump and wreck the page, and nobody chooses a viewer
by reading 207 keybindings.

So this is additive: the complete list gets a page of its own, always current,
and the two curated documents stay curated. What keeps THEM honest is the
separate checker, check-help-docs.ps1, which asks whether every label HelpWnd
renders is mentioned — not whether the tables match.

WHAT THIS SCRIPT DELIBERATELY DOES NOT DO
-----------------------------------------
It does not compile anything, it does not touch the app, and it writes only
inside docs/. It cannot break a build because it is not part of one: what it
writes is committed, and GitHub Pages only hands the files over.
"""

import io
import os
import re
import sys
from datetime import date

# The repository root, found from this file's own location: tools/ sits
# directly under it. A hardcoded absolute path would work on exactly one
# machine, and this also has to run on a CI runner and on a second PC.
# QIV_REPO still overrides, for running the script from somewhere else.
REPO = os.environ.get(
    'QIV_REPO',
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DOCS = os.path.join(REPO, 'docs')
PARTIALS = os.path.join(DOCS, '_partials')
HELPWND = os.path.join(REPO, 'src', 'UI', 'FloatingPanels', 'HelpWnd.cpp')
SHORTCUTS_H = os.path.join(REPO, 'src', 'Input', 'Shortcuts.h')
ICONS_H = os.path.join(REPO, 'src', 'Platform', 'ConstantsIcons.h')
OUT_PAGE = os.path.join(DOCS, 'shortcuts.html')

CURRENT_PAGE = {
    'index.html': 'home',
    'qiv-privacy.html': 'priv',
    'qiv-remote.html': 'remote',
    'qiv-remote-privacy.html': 'remotepriv',
}

TOKEN = re.compile(r'\{\{CURRENT:(\w+)\}\}')


def read(p):
    return io.open(p, encoding='utf-8', errors='replace').read()


def write(p, t):
    io.open(p, 'w', encoding='utf-8', newline='\n').write(t)


# =====================================================================
# JOB 1 — partials
# =====================================================================

def inject_partials(check):
    if not os.path.isdir(PARTIALS):
        print('  no _partials/ — skipped')
        return [], []

    partials = {}
    for f in sorted(os.listdir(PARTIALS)):
        if f.endswith('.html'):
            partials[os.path.splitext(f)[0].upper()] = read(os.path.join(PARTIALS, f))

    pages = sorted(f for f in os.listdir(DOCS)
                   if f.endswith('.html') and not f.startswith('google'))

    stale, written, used = [], [], {k: 0 for k in partials}

    for page in pages:
        path = os.path.join(DOCS, page)
        original = read(path)
        text = original
        current = CURRENT_PAGE.get(page)

        for name, body in partials.items():
            start = '<!-- SHARED-%s-START' % name
            end = '<!-- SHARED-%s-END -->' % name
            if start not in text:
                continue
            if end not in text:
                raise SystemExit('  %s: %s has an opening fence and no closing one'
                                 % (page, name))
            filled = TOKEN.sub(
                lambda m: ' class="current"' if m.group(1) == current else '', body)
            a = text.index('-->', text.index(start)) + 3
            b = text.index(end, a)
            text = text[:a] + '\n' + filled.strip('\n') + '\n' + text[b:]
            used[name] += 1

        if text == original:
            continue
        (stale if check else written).append(page)
        if not check:
            write(path, text)

    for name, n in sorted(used.items()):
        if n == 0:
            print('  note: _partials/%s.html is used by no page' % name.lower())

    return stale, written


# =====================================================================
# JOB 2 — the shortcut reference, parsed out of HelpWnd.cpp
# =====================================================================

# The VK names KeyName() spells out itself. Everything else is a letter, a digit
# or OEM punctuation, which the app resolves against the ACTIVE KEYBOARD LAYOUT
# at runtime — so this table mirrors KeyName() for the named keys and falls back
# to the US layout for the rest, which is what the documentation has always
# assumed and what the existing checker assumes too.
VK_NAMED = {
    'VK_ESCAPE': 'Esc', 'VK_TAB': 'Tab', 'VK_RETURN': 'Enter', 'VK_SPACE': 'Space',
    'VK_BACK': 'Backspace', 'VK_DELETE': 'Delete', 'VK_INSERT': 'Insert',
    'VK_HOME': 'Home', 'VK_END': 'End', 'VK_PRIOR': 'Page Up', 'VK_NEXT': 'Page Down',
    'VK_LEFT': 'Left', 'VK_RIGHT': 'Right', 'VK_UP': 'Up', 'VK_DOWN': 'Down',
    'VK_ADD': 'Num +', 'VK_SUBTRACT': 'Num −', 'VK_MULTIPLY': 'Num *',
    'VK_DIVIDE': 'Num /',
    # OEM punctuation on a US layout. VK_OEM_PLUS is the unshifted '=' key —
    # naming it '+' is the mistake this table exists to avoid.
    'VK_OEM_PLUS': '=', 'VK_OEM_MINUS': '-', 'VK_OEM_COMMA': ',', 'VK_OEM_PERIOD': '.',
    'VK_OEM_1': ';', 'VK_OEM_2': '/', 'VK_OEM_3': '`', 'VK_OEM_4': '[',
    'VK_OEM_5': '\\', 'VK_OEM_6': ']', 'VK_OEM_7': "'",
}

WRAPPERS = {
    'K': '', 'Ctrl': 'Ctrl+', 'Shift': 'Shift+', 'Alt': 'Alt+',
    'CtrlShift': 'Ctrl+Shift+', 'CtrlAlt': 'Ctrl+Alt+', 'CtrlAltShift': 'Ctrl+Alt+Shift+',
}


def load_constants():
    """Every `constexpr UINT NAME = VALUE;` in Shortcuts.h, resolved to a label."""
    text = read(SHORTCUTS_H)
    raw = dict(re.findall(r'constexpr\s+UINT\s+(\w+)\s*=\s*([^;]+);', text))
    out = {}
    for name, value in raw.items():
        v = value.strip()
        seen = 0
        while v in raw and seen < 8:          # one constant defined as another
            v = raw[v].strip()
            seen += 1
        if v in VK_NAMED:
            out[name] = VK_NAMED[v]
        elif re.fullmatch(r"'(.)'", v):
            out[name] = v[1].upper()
        elif re.fullmatch(r'VK_F(\d+)', v):
            out[name] = 'F' + re.fullmatch(r'VK_F(\d+)', v).group(1)
        elif re.fullmatch(r'VK_NUMPAD(\d)', v):
            out[name] = 'Num ' + re.fullmatch(r'VK_NUMPAD(\d)', v).group(1)
        else:
            out[name] = None                  # unresolved; reported, never guessed
    return out


def decode_cpp_escapes(lit):
    r"""Decode the escapes ConstantsIcons.h uses: \xNNNN, \uNNNN, \UNNNNNNNN.

    Python's own unicode_escape codec is not usable here. C's \x consumes EVERY
    hex digit that follows it, Python's consumes exactly two — so L"\x2022" comes
    back from the codec as "\x20" followed by the text "22" instead of a bullet.
    """
    out = []
    i = 0
    while i < len(lit):
        if lit[i] != '\\':
            out.append(lit[i])
            i += 1
            continue
        i += 1
        if i >= len(lit):
            break
        esc = lit[i]
        i += 1
        if esc == 'x':
            start = i
            while i < len(lit) and lit[i] in '0123456789abcdefABCDEF':
                i += 1
            if i > start:
                out.append(chr(int(lit[start:i], 16)))
        elif esc in ('u', 'U'):
            width = 4 if esc == 'u' else 8
            digits = lit[i:i + width]
            i += width
            if len(digits) == width:
                out.append(chr(int(digits, 16)))
        elif esc == 'n':
            out.append('\n')
        elif esc == 't':
            out.append('\t')
        elif esc == 'r':
            out.append('\r')
        else:
            out.append(esc)                   # \\ and \" land here
    return ''.join(out)


def load_icons():
    """QIV_ICON_* macro name -> the glyph it expands to.

    HELPWND STOPPED WRITING ITS GLYPHS AS LITERALS. "•" and "→" used to sit in
    the L"..." fragments this file reads; they are now ConstantsIcons.h macros,
    invisible to a parser that only collects string literals. Dropping the
    bullet silently merged the two groups of

        L"Shift+Num + / −   " QIV_ICON_BULLET L"   Shift+" + K(SC_COLOR_GAMMA_UP)

    into one run, and the page published a <kbd> reading "Shift+− • Shift+="
    for a key nobody can press. Every macro in that header is ordinary Unicode
    or emoji — no Segoe MDL2 private-use codepoints — so all of them are safe to
    put on a page the site serves without an icon font.
    """
    icons = {}
    if not os.path.exists(ICONS_H):
        return icons                          # header is new; older trees lack it
    text = read(ICONS_H)
    for m in re.finditer(r'#define\s+(QIV_ICON_\w+)[ \t]+([^\r\n]+)', text):
        name, body = m.group(1), m.group(2)
        alias = re.match(r'\s*(QIV_ICON_\w+)', body)
        if alias:                             # QIV_ICON_SECTION_ANTENNA = QIV_ICON_ANTENNA
            if alias.group(1) in icons:
                icons[name] = icons[alias.group(1)]
            continue
        glyph = ''
        for lit in re.findall(r'L"((?:[^"\\]|\\.)*)"', body):
            glyph += decode_cpp_escapes(lit)
        if glyph:
            icons[name] = glyph
    return icons


def resolve_label(expr, consts, unresolved, icons):
    """Turn an Add()'s first argument into the string the panel would print."""
    parts = []
    for tok in re.finditer(r'(\w+)\s*\(\s*(?:\w+::)?(\w+)\s*\)'
                           r'|L"((?:[^"\\]|\\.)*)"'
                           r'|\b(QIV_ICON_\w+)\b',
                           expr):
        fn, const, lit = tok.group(1), tok.group(2), tok.group(3)
        if lit is not None:
            parts.append(lit.replace('\\"', '"'))
            continue
        if tok.group(4) is not None:
            parts.append(icons.get(tok.group(4), ''))
            continue
        if fn not in WRAPPERS:
            continue
        # A Shortcuts.h constant, or a raw VK the call site passed directly —
        # HelpWnd does both, and K(VK_DELETE) is just as valid as K(SC::SC_X).
        key = consts.get(const) or VK_NAMED.get(const)
        if key is None:
            unresolved.add(const)
            key = '?'
        parts.append(WRAPPERS[fn] + key)
    return re.sub(r'\s+', ' ', ''.join(parts)).strip()


def split_args(text):
    """Split an Add(...) argument list on TOP-LEVEL commas.

    Naive splitting breaks on both of the things this file is full of: nested
    calls like K(SC::X), and commas inside string literals. Depth counting plus
    a string-aware scan is the whole trick.
    """
    out, depth, buf, in_str, esc_next = [], 0, [], False, False
    for ch in text:
        if in_str:
            buf.append(ch)
            if esc_next:
                esc_next = False
            elif ch == '\\':
                esc_next = True
            elif ch == '"':
                in_str = False
            continue
        if ch == '"':
            in_str = True
            buf.append(ch)
        elif ch in '([':
            depth += 1
            buf.append(ch)
        elif ch in ')]':
            depth -= 1
            buf.append(ch)
        elif ch == ',' and depth == 0:
            out.append(''.join(buf))
            buf = []
        else:
            buf.append(ch)
    out.append(''.join(buf))
    return [a.strip() for a in out]


def joined_literals(expr, icons):
    """Join adjacent C++ string literals into the sentence they form.

    A description is routinely four L"..." fragments across four lines, which
    the compiler concatenates. Reading only one of them is how a description
    comes out as " per step." instead of the whole line.

    Icon macros count as fragments too — the remote-log description reads
    "numbered — QIV_ICON_ARROW_RIGHT for a send", and without the arrow the
    sentence published as "numbered — for a send".
    """
    parts = []
    for tok in re.finditer(r'L"((?:[^"\\]|\\.)*)"|\b(QIV_ICON_\w+)\b', expr):
        if tok.group(1) is not None:
            parts.append(tok.group(1).replace('\\"', '"'))
        else:
            parts.append(icons.get(tok.group(2), ''))
    return re.sub(r'\s+', ' ', ''.join(parts)).strip()


def parse_helpwnd():
    """[(section title, [(keys, description), …]), …] in the panel's own order."""
    text = read(HELPWND)
    consts = load_constants()
    icons = load_icons()
    unresolved = set()

    sections = {}
    for m in re.finditer(r'const\s+int\s+(\w+)\s*=\s*Sec\([^,]+,\s*L"([^"]+)"', text):
        sections[m.group(1)] = m.group(2)
    # A few Sec() calls put the title on the next line.
    for m in re.finditer(r'const\s+int\s+(\w+)\s*=\s*Sec\(\s*[^,]+,\s*\n\s*L"([^"]+)"', text):
        sections.setdefault(m.group(1), m.group(2))

    order, rows = [], {}
    for m in re.finditer(r'\bAdd\(\s*(.*?)\s*\);', text, re.S):
        args = split_args(m.group(1))
        if len(args) < 3:
            continue                    # not the (keys, description, section) form

        sec = args[-1].strip()
        if sec not in sections:
            continue                    # a section variable this pass did not see

        # ARGUMENT 0 IS THE LABEL, ARGUMENT 1 THE DESCRIPTION, and they must be
        # split on a TOP-LEVEL comma. Taking the last string literal instead —
        # which is what this did first — produced "Left / RightGo to the
        # previous..." because the label and the description ran together, and
        # dropped most of every description because C++ concatenates adjacent
        # literals and only the final fragment survived.
        label = resolve_label(args[0], consts, unresolved, icons)
        desc = joined_literals(args[1], icons)
        if not label or not desc:
            continue

        if sec not in rows:
            rows[sec] = []
            order.append(sec)
        rows[sec].append((label, desc))

    return [(sections[s], rows[s]) for s in order], sorted(unresolved)


def esc(s):
    return (s.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;'))


MODIFIER = re.compile(r'^((?:Ctrl\+|Alt\+|Shift\+)+)')

# ONE PASS OVER ALL SEPARATORS, not the first one that happens to be present.
# The old loop returned on the first separator it found, so a label carrying two
# kinds kept the second one inside a <kbd>: "Shift+Num + / −  •  Shift+= / -"
# came out with an element reading "Shift+− • Shift+=", a key nobody can press.
# The separator is captured so it can be put back between the elements verbatim.
SEPARATORS = re.compile(r'( / | – | — | • )')


def kbdify(label):
    """Turn a composite label into one <kbd> PER KEY.

    HelpWnd prints groups as a single string — "W / A / S / D",
    "Alt+W / A / S / D", "1 – 5" — because it draws them as one line of text.
    Wrapping that whole string in one <kbd> makes a key look like a sentence,
    and it hides the individual keys from check-help-docs.ps1, which asks
    whether the documentation mentions each label the app shows. It does not
    find "Alt+A" inside "Alt+W / A / S / D".

    So each key gets its own element, and A LEADING MODIFIER IS DISTRIBUTED
    across the group: "Alt+W / A / S / D" documents Alt+W, Alt+A, Alt+S and
    Alt+D, which is what the app actually binds and what the checker expects.
    Separators stay outside the elements, where they belong — they are prose,
    not keys.
    """
    pieces = SEPARATORS.split(label)
    if len(pieces) == 1:
        return '<kbd>%s</kbd>' % esc(label)

    mod = MODIFIER.match(pieces[0].strip())
    mod = mod.group(1) if mod else ''

    out = []
    first = True
    for piece in pieces:
        if SEPARATORS.fullmatch(piece):
            out.append(piece)
            continue
        part = piece.strip()
        if not first and mod and not MODIFIER.match(part):
            part = mod + part
        first = False
        out.append('<kbd>%s</kbd>' % esc(part))
    return ''.join(out)


# One per section title, because the app's own section glyphs are Segoe MDL2 and
# the site vendors no icon font — an MDL2 codepoint renders as a box anywhere but
# Windows. Emoji is what the rest of the site already uses (⚠, 🔒, 🖥).
SECTION_ICONS = {
    'IMAGE NAVIGATION': '\U0001F9ED', 'ZOOM, PAN & VIEW MODES': '\U0001F50D',
    'MOUSE CONTROLS': '\U0001F5B1', 'WINDOW MANAGEMENT': '\U0001FA9F',
    'PANELS & TOOLS': '\U0001F9F0', 'THUMBNAIL STRIPS': '\U0001F5BC',
    'HISTORY PANEL': '\U0001F4DC', 'SLIDESHOW': '▶',
    'INFO OVERLAYS': 'ℹ', 'EFFECTS & COLOR': '\U0001F3A8',
    'FILES, CLIPBOARD & SORTING': '\U0001F4BE', 'APPLICATION & APPEARANCE': '⚙',
    'SYSTEM TRAY': '\U0001F514', 'DEDICATED SCREENS': '\U0001F5A5',
    'COMMAND-LINE ARGUMENTS': '⌨', 'REMOTE CONTROL & MIRRORING': '\U0001F4E1',
}

# A row is one to three lines. Past this it is cut and the rest moves to "In depth".
ROW_MAX_CHARS = 200


def helpwnd_paragraphs(desc):
    r"""Split a HelpWnd description into paragraphs.

    HelpWnd writes its paragraph breaks as the C++ escape \r\n INSIDE a wide
    string literal, so the parser hands them over as four literal characters —
    backslash, r, backslash, n — not as a newline. esc() only escapes HTML, so
    they used to travel all the way onto the published page and render as the
    text "\r\n"; there were fourteen of them live before this split existed.
    """
    return [p.strip() for p in re.split(r'\\r\\n|\r\n', desc) if p.strip()]


def shorten_for_row(text, limit=ROW_MAX_CHARS):
    """Trim to a sentence boundary. Returns (text, was_cut)."""
    if len(text) <= limit:
        return text, False
    cut = text[:limit]
    dot = max(cut.rfind('. '), cut.rfind(' — '))
    if dot > 80:
        return cut[:dot + 1], True
    return cut.rsplit(' ', 1)[0] + '…', True


def build_shortcuts_page():
    data, unresolved = parse_helpwnd()
    total = sum(len(r) for _, r in data)

    body = []
    body.append('    <h1>QuickImageViewer (qIV) — Shortcuts</h1>')
    body.append('    <p class="meta">')
    body.append('        Generated from the app\'s own help panel — press '
                '<kbd>F1</kbd> in QuickImageViewer to see the same list on your')
    body.append('        machine. %d shortcuts in %d sections.' % (total, len(data)))
    body.append('    </p>')
    body.append('')
    body.append('<!-- SHARED-STOREROW-START -->')
    body.append('<!-- SHARED-STOREROW-END -->')
    body.append('')
    body.append('    <input class="sc-filter" id="sc-filter" type="search"'
                ' placeholder="Filter shortcuts — try &quot;folder&quot;,'
                ' &quot;alt&quot;, &quot;wheel&quot; or a section name"'
                ' autocomplete="off">')
    body.append('    <p class="sc-status" id="sc-status"></p>')
    body.append('')

    # EVERY SECTION VISIBLE, two dense columns inside each.
    #
    # This replaced one collapsed <details> per section. Collapsed drawers keep
    # the page short, but they hide the reference behind a click each and a
    # visitor cannot scan or Ctrl+F what is not in the DOM. Laid flat in two
    # columns the whole thing is a few screens, and the filter box above finds
    # any one of 209 in a keystroke.
    #
    # AND THE ROWS STAY SHORT. Only the first paragraph goes in the row, capped
    # at ROW_MAX_CHARS; anything longer moves to "In depth" below and the row
    # links to it. Some descriptions run past three thousand characters — F9 and
    # Ctrl+F12 are essays — and printed in full they turned one section into a
    # wall that dwarfed the other fifteen. The full text is still on the page,
    # still searchable, just not in the way of the list.
    deep = []
    seen_ids = {}
    for title, entries in data:
        rows = []
        for keys, desc in entries:
            paragraphs = helpwnd_paragraphs(desc)
            head = paragraphs[0] if paragraphs else ''
            short, was_cut = shorten_for_row(head)
            link = ''
            if was_cut or len(paragraphs) > 1:
                base = re.sub(r'[^a-z0-9]+', '-',
                              (title + '-' + keys).lower()).strip('-')[:60]
                seen_ids[base] = seen_ids.get(base, 0) + 1
                anchor = base if seen_ids[base] == 1 else '%s-%d' % (base, seen_ids[base])
                link = ' <a class="sc-more" href="#%s">details</a>' % anchor
                deep.append(
                    '    <article class="sc-deep" id="%s">\n'
                    '        <h3>%s<span class="sc-deep-sec">%s</span></h3>\n%s\n'
                    '    </article>'
                    % (anchor, kbdify(keys), esc(title.title()),
                       '\n'.join('        <p>%s</p>' % esc(p) for p in paragraphs)))
            rows.append('            <div class="sc-row"><div class="sc-keys">%s</div>'
                        '<p class="sc-desc">%s%s</p></div>'
                        % (kbdify(keys), esc(short), link))

        body.append('    <section class="sc-sec" data-title="%s">' % esc(title.title()))
        body.append('        <h2><span class="sc-ico">%s</span>%s'
                    '<span class="sc-count">%d</span></h2>'
                    % (SECTION_ICONS.get(title, '⌨'), esc(title.title()), len(entries)))
        body.append('        <div class="sc-cols">')
        body.extend(rows)
        body.append('        </div>')
        body.append('    </section>')

    if deep:
        body.append('')
        body.append('    <h2 class="sc-deep-head">In depth</h2>')
        body.append('    <p class="sc-deep-intro">The %d shortcuts above whose '
                    'behaviour needs more than a line. Every one is linked from '
                    'its row.</p>' % len(deep))
        body.extend(deep)

    body.append('')
    body.append('<!-- SHARED-EXAMPLES-START -->')
    body.append('<!-- SHARED-EXAMPLES-END -->')

    inner = '\n'.join(body)

    page = '''<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>QuickImageViewer (qIV) — every keyboard shortcut</title>

    <meta name="description"
          content="The complete keyboard shortcut reference for QuickImageViewer, generated from the application's own help panel so it is never out of date.">
    <meta name="robots" content="index, follow">
    <link rel="canonical" href="https://icyhoty2k.github.io/QuickImageViewer/shortcuts.html">

    <link rel="icon" type="image/svg+xml" href="favicon.svg">
    <link rel="icon" type="image/x-icon" href="favicon.ico">
    <meta name="theme-color" content="#0f172a">

    <!-- Open Graph and Twitter. Without og:image a shared link renders as a
         blank grey box on Facebook, Messenger, Viber and Slack.

         THESE LIVE IN THE GENERATOR, NOT IN shortcuts.html. That file is written
         from scratch on every build, so a tag added to it by hand survives until
         the next run and then vanishes without a word. -->
    <meta property="og:type" content="website">
    <meta property="og:site_name" content="QuickImageViewer">
    <meta property="og:title" content="Every QuickImageViewer keyboard shortcut">
    <meta property="og:description"
          content="The complete reference: 224 shortcuts in 16 sections, generated from the application's own help panel.">
    <meta property="og:url" content="https://icyhoty2k.github.io/QuickImageViewer/shortcuts.html">
    <meta property="og:image" content="https://icyhoty2k.github.io/QuickImageViewer/screenshots/less300kb/10.webp">
    <meta property="og:image:alt" content="QuickImageViewer showing a photo with a thumbnail strip">
    <meta property="og:locale" content="en_GB">
    <meta name="twitter:card" content="summary_large_image">
    <meta name="twitter:title" content="Every QuickImageViewer keyboard shortcut">
    <meta name="twitter:description"
          content="The complete reference: 224 shortcuts in 16 sections, generated from the application's own help panel.">
    <meta name="twitter:image" content="https://icyhoty2k.github.io/QuickImageViewer/screenshots/less300kb/10.webp">

<script type="application/ld+json">
{
  "@context": "https://schema.org",
  "@type": "WebPage",
  "name": "Every QuickImageViewer keyboard shortcut",
  "url": "https://icyhoty2k.github.io/QuickImageViewer/shortcuts.html",
  "isPartOf": { "@type": "WebSite", "name": "QuickImageViewer", "url": "https://icyhoty2k.github.io/QuickImageViewer/" },
  "author": { "@type": "Person", "name": "Ivan Hristov Yanev", "alternateName": "icyhoty2k", "url": "https://icyhoty2k.github.io/" }
}
</script>

    <link rel="stylesheet" href="site.css">
</head>
<!--
    GENERATED FILE — do not edit.

    Written by build-site.py from src/UI/FloatingPanels/HelpWnd.cpp, which is the
    panel the application itself shows. Its labels come from Shortcuts.h through
    K()/Ctrl()/Shift(), so remapping a key updates the app, this page and F1
    together. Editing this file by hand puts it out of step with the binary,
    which is the entire problem it exists to solve.

    Last generated: %s
-->
<body>

<!-- SHARED-HEADER-START -->
<!-- SHARED-HEADER-END -->

<main class="container">

%s

</main>

<!-- SHARED-FOOTER-START -->
<!-- SHARED-FOOTER-END -->

<script>
// Filters rows in place. Vendored, like everything else on this site — no
// library, and it degrades to a plain full list if scripting is off, which is
// why the filter box is the only thing that would be missing rather than the
// reference itself.
(function () {
    var f = document.getElementById('sc-filter'),
        s = document.getElementById('sc-status');
    if (!f) return;
    var secs = [].slice.call(document.querySelectorAll('.sc-sec'));
    secs.forEach(function (c) { c._rows = [].slice.call(c.querySelectorAll('.sc-row')); });
    f.addEventListener('input', function () {
        var q = f.value.trim().toLowerCase(), hits = 0;
        secs.forEach(function (c) {
            // A section-name match keeps the whole section, so "tray" answers
            // "show me the tray" rather than only rows containing the word.
            var whole = q && c.dataset.title.toLowerCase().indexOf(q) > -1, vis = 0;
            c._rows.forEach(function (r) {
                var m = !q || whole || r.textContent.toLowerCase().indexOf(q) > -1;
                r.classList.toggle('sc-hidden', !m);
                if (m) vis++;
            });
            c.classList.toggle('sc-hidden', vis === 0);
            hits += vis;
        });
        s.textContent = q ? (hits + ' shortcut' + (hits === 1 ? '' : 's') +
                             ' match "' + f.value.trim() + '"') : '';
    });
})();
</script>

</body>
</html>
''' % (date.today().isoformat(), inner)
    return page, total, len(data), unresolved



# =====================================================================
# JOB 3 — repository statistics, fetched at BUILD time
# =====================================================================

# WHY BUILD TIME AND NOT THE BROWSER. index.html used to fetch
# api.github.com on every page view to show a download total. That is a
# third-party request: it hands GitHub's API the visitor's IP, user-agent and
# referring page, on a site that promises it contacts nobody. Fetching the same
# numbers HERE, once, and writing them into the page means the visitor's browser
# talks only to icyhoty2k.github.io — and the numbers cost nothing to display
# because they are already in the HTML.
#
# The trade is that they are as fresh as the last build rather than live. For
# star counts and download totals that is the right trade: nobody needs those to
# the second, and a stale number is a far smaller problem than a broken promise.
#
# WHY NOT shields.io IMAGES, which the README uses. GitHub proxies README images
# through its own camo servers, so shields.io never sees a reader — safe there.
# The same <img> on this site would be a direct request to shields.io from every
# visitor. Same picture, completely different privacy consequence.
#
# OFFLINE IS NOT AN ERROR. If the API cannot be reached the existing partial is
# left exactly as it is, so a build on a train keeps yesterday's numbers rather
# than blanking them.

GH_API = 'https://api.github.com/repos/icyhoty2k/QuickImageViewer'
STATS_PARTIAL = os.path.join(PARTIALS, 'stats.html')
VERSION_PARTIAL = os.path.join(PARTIALS, 'version.html')


def fetch_json(url):
    import urllib.request
    headers = {
        'Accept': 'application/vnd.github+json',
        'User-Agent': 'qiv-build-site',
    }

    # AUTHENTICATE WHEN A TOKEN IS THERE, and work without one when it is not.
    # Unauthenticated the API allows 60 requests an hour PER IP — and on a CI
    # runner that IP is shared with every other job on the same host, so the
    # budget can already be spent before this starts. The token raises it to
    # 1,000 for this repository. Locally there is usually no token and 60 an
    # hour is more than one person needs, so its absence is not an error.
    token = os.environ.get('GITHUB_TOKEN') or os.environ.get('GH_TOKEN')
    if token:
        headers['Authorization'] = 'Bearer ' + token

    req = urllib.request.Request(url, headers=headers)
    with urllib.request.urlopen(req, timeout=15) as r:
        import json
        return json.load(r)


def human(n):
    if n >= 1000000:
        return '%.1fM' % (n / 1000000.0)
    if n >= 1000:
        return '%.1fk' % (n / 1000.0)
    return str(n)


def build_stats():
    """Fetch the repository's public numbers and render the strip."""
    try:
        repo = fetch_json(GH_API)
        releases = fetch_json(GH_API + '/releases?per_page=100')
    except Exception as e:
        return None, None, 'offline or rate-limited (%s)' % type(e).__name__

    downloads = 0
    for rel in releases:
        for asset in rel.get('assets') or []:
            downloads += asset.get('download_count') or 0

    latest = next((r.get('tag_name') for r in releases
                   if not r.get('prerelease')), None) or repo.get('default_branch')

    # EVERY ENTRY MUST READ AS ONE PHRASE, value first then label, because that
    # is the order they are drawn in: "284 downloads", "10 stars", "AGPLv3
    # licence". The Windows entry was ('10 | 11', 'Windows') and therefore
    # rendered "10 | 11 Windows", which is the sentence backwards — the value
    # and the label had been filled in the way a shields.io badge takes them,
    # where the label is drawn FIRST.
    #
    # The last field is a floor: an entry whose number is below it is dropped.
    # A brand-new project legitimately has 0 forks, and printing "0 forks" on
    # the page that is meant to persuade somebody to try it argues the opposite
    # case for you. Absent says nothing; zero says nobody wanted it.
    # --- Facts about the CODE, read straight off disk -------------------
    #
    # No network and no API budget: these come from the repository this script
    # is already standing in. They are also the most persuasive numbers here —
    # "207 shortcuts" and "49 formats" say what the app IS, where a star count
    # only says how many people have noticed it so far.
    loc = files = formats = shortcuts = 0
    try:
        for dirpath, _, names in os.walk(os.path.join(REPO, 'src')):
            for n in names:
                if n.endswith(('.cpp', '.h', '.hpp')):
                    files += 1
                    loc += read(os.path.join(dirpath, n)).count('\n')
        shortcuts = len(re.findall(r'\bAdd\(', read(HELPWND)))
        formats = len(set(re.findall(r'L"\.([a-z0-9]{2,5})"',
                                     read(os.path.join(REPO, 'src', 'Platform',
                                                       'Constants.h')))))
    except Exception:
        pass                      # a missing source tree is not a build failure

    # The shipped executable's own size — the portability claim, measured
    # rather than asserted.
    exe_mb = 0.0
    for rel in releases:
        if rel.get('prerelease'):
            continue
        for asset in rel.get('assets') or []:
            if asset.get('name', '').lower().endswith('.exe'):
                exe_mb = asset.get('size', 0) / 1048576.0
                break
        if exe_mb:
            break

    # "Updated 3 days ago" is a maintenance signal; an abandoned project is the
    # first thing a careful downloader checks for.
    updated = ''
    try:
        from datetime import datetime, timezone
        pushed = datetime.strptime(repo['pushed_at'], '%Y-%m-%dT%H:%M:%SZ')
        days = (datetime.now(timezone.utc).replace(tzinfo=None) - pushed).days
        updated = ('today' if days <= 0 else
                   'yesterday' if days == 1 else
                   '%d days ago' % days if days < 31 else
                   '%d months ago' % (days // 30))
    except Exception:
        pass

    repo_url = 'https://github.com/icyhoty2k/QuickImageViewer'
    stats = [
        ('⬇',  human(downloads), 'downloads', repo_url + '/releases', downloads, 1),
        ('⭐', human(repo.get('stargazers_count', 0)), 'stars',
         repo_url + '/stargazers', repo.get('stargazers_count', 0), 1),
        ('🔀', human(repo.get('forks_count', 0)), 'forks',
         repo_url + '/forks', repo.get('forks_count', 0), 1),
        ('👀', human(repo.get('subscribers_count', 0)), 'watching',
         repo_url + '/watchers', repo.get('subscribers_count', 0), 1),
        ('🚀', str(len(releases)), 'releases', repo_url + '/releases', len(releases), 2),
        ('🏷',  latest or '', 'latest release', repo_url + '/releases/latest', None, None),
        ('🕒', updated, 'last updated', repo_url + '/commits', 1 if updated else 0, 1),
        ('📦', '%.1f MB' % exe_mb, 'single .exe', repo_url + '/releases/latest',
         exe_mb, 0.1),
        ('🖼', '%d' % formats, 'formats', './#formats', formats, 5),
        ('⌨', '%d' % shortcuts, 'shortcuts', 'shortcuts.html', shortcuts, 5),
        ('📐', human(loc), 'lines of C++', repo_url, loc, 1000),
        ('📄', 'AGPLv3', 'licence', repo_url + '/blob/main/LICENSE', None, None),
        ('🖥',  'Windows', '10 &amp; 11', './', None, None),
    ]

    rows = ['<!-- Generated by build-site.py from the GitHub API at build time.',
            '     Do not edit: it is overwritten on every build. -->',
            '<div class="statstrip">']
    for glyph, value, label, href, number, floor in stats:
        if floor is not None and (number or 0) < floor:
            continue
        rows.append('    <a class="stat" href="%s">' % href)
        rows.append('        <span class="stat-glyph">%s</span>' % glyph)
        rows.append('        <span class="stat-value">%s</span>' % value)
        rows.append('        <span class="stat-label">%s</span>' % label)
        rows.append('    </a>')
    rows.append('</div>')
    # --- the version line that sits under the download button ------------
    #
    # A script used to build this in the browser from api.github.com on every
    # page view. Same three facts, fetched once here instead: the visitor's
    # browser no longer talks to GitHub to find out what it is downloading.
    #
    # SIZE IN KB WITH A THOUSANDS SEPARATOR, matching what the line has always
    # said. "9,792.00 KB" is oddly precise for a download button and that is the
    # point — it is the exact size of the file, not a rounded reassurance, on a
    # page whose whole pitch is that the thing is small.
    ver_html = ''
    for rel in releases:
        if rel.get('prerelease'):
            continue
        asset = next((a for a in rel.get('assets') or []
                      if a.get('name', '').lower().endswith('.exe')), None)
        if not asset:
            continue
        ver_html = (
            '<!-- Generated by build-site.py from the GitHub API at build time.\n'
            '     Do not edit: it is overwritten on every build. -->\n'
            'Version %s &bull; Size: %s KB &bull; Build Date: %s\n'
            % (rel.get('tag_name', '').lstrip('vV'),
               format(asset.get('size', 0) / 1024.0, ',.2f'),
               (rel.get('published_at') or '')[:10]))
        break

    return ('\n'.join(rows) + '\n', ver_html,
            '%s downloads, %s stars' % (human(downloads),
                                        human(repo.get('stargazers_count', 0))))

# =====================================================================

def main():
    check = '--check' in sys.argv
    print('QuickImageViewer site build%s' % ('  [check only]' if check else ''))
    print('  repo: %s' % REPO)

    print('\n== shortcut reference ==')
    page, total, nsec, unresolved = build_shortcuts_page()
    if unresolved:
        print('  %d constant(s) could not be resolved and are shown as "?":'
              % len(unresolved))
        for u in unresolved[:10]:
            print('     %s' % u)
    existing = read(OUT_PAGE) if os.path.isfile(OUT_PAGE) else None

    def comparable(t):
        """What is worth comparing between a fresh render and the file on disk.

        Two things must be ignored or the page is stale on every run:

        THE DATE, obviously — it changes daily whether or not a shortcut did.

        THE SHARED FENCES, less obviously. This function renders the page with
        EMPTY fences and job 2 fills them a moment later, so the file on disk
        always carries a header and a footer the fresh render does not. Blanking
        both sides is what makes --check answer the question actually being
        asked: has HelpWnd.cpp changed since this page was written?
        """
        t = t or ''
        t = re.sub(r'Last generated: \d{4}-\d{2}-\d{2}', '', t)
        t = re.sub(r'(<!-- SHARED-\w+-START.*?-->).*?(<!-- SHARED-\w+-END -->)',
                   r'\1\2', t, flags=re.S)
        return t

    shortcuts_stale = comparable(existing) != comparable(page)
    if shortcuts_stale and not check:
        write(OUT_PAGE, page)
        print('  wrote docs/shortcuts.html — %d shortcuts, %d sections' % (total, nsec))
    elif shortcuts_stale:
        print('  STALE: docs/shortcuts.html does not match HelpWnd.cpp')
    else:
        print('  docs/shortcuts.html is current — %d shortcuts, %d sections' % (total, nsec))

    print('\n== repository statistics ==')
    stats_html, version_html, note = build_stats()
    stats_stale = False
    if stats_html is None:
        print('  %s - keeping the numbers already in _partials/' % note)
    else:
        # The version line rides along: same fetch, same freshness, its own
        # partial so a page can take one without the other.
        if version_html and not check:
            write(VERSION_PARTIAL, version_html)
        existing = read(STATS_PARTIAL) if os.path.isfile(STATS_PARTIAL) else None
        if existing == stats_html:
            print('  unchanged - %s' % note)
        elif check:
            # NOT a failure. These numbers change because other people starred
            # or downloaded the project, which is not drift and not something a
            # commit hook should block. Reported, never fatal.
            stats_stale = True
            print('  out of date - %s (run without --check to refresh)' % note)
        else:
            write(STATS_PARTIAL, stats_html)
            print('  wrote _partials/stats.html - %s' % note)

    print('\n== shared blocks ==')
    stale, written = inject_partials(check)
    for p in written:
        print('  rewrote %s' % p)
    for p in stale:
        print('  STALE: %s' % p)
    if not written and not stale:
        print('  every page already matches _partials/')

    if check and (stale or shortcuts_stale):
        print('\nFAIL - run:  python build-site.py')
        return 1
    print('\nOK.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
