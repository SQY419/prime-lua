#!/usr/bin/env python3
"""strip_main.py -- move main.py's prose out of main.py.

WHY
    The calculator's Python environment compiles the whole source of an imported
    module into its own memory, and it does so generously: on this machine
    `import main` (156 KB of source) costs about 6 MB of heap before a single
    script runs.  The comments and docstrings -- 40% of those bytes -- are not
    worth 2.4 MB of a calculator's RAM.

    So the prose lives in calc/main_notes.md, ordered the way it appeared, and
    calc/main.py keeps only a short header pointing at it.  Nothing is thrown
    away: this tool EXTRACTS, it does not delete.

WHAT IT DOES
    * every `#` comment is copied into the notes (with the definition it sat in)
      and removed from the code;
    * every module/class/function docstring except the module's first paragraph
      is copied and removed;
    * runs of blank lines are collapsed (they cost bytes and say nothing);
    * the result is re-parsed before it is written -- a broken main.py would be
      a calculator that cannot start.

USAGE
    python3 tools/strip_main.py            # rewrite calc/main.py + main_notes.md
    python3 tools/strip_main.py --check    # exit 1 if main.py is not stripped
"""
import ast
import io
import os
import sys
import tokenize

TOP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAIN = os.path.join(TOP, 'calc', 'main.py')
NOTES = os.path.join(TOP, 'calc', 'main_notes.md')

# The header that stays in the code: enough to explain what the file is and
# where its documentation went, and no more.
HEADER = '''"""
main.py -- primeLua: the Lua 5.4 interpreter on the HP Prime G1.

    >>> import main

Run it from the calculator's Python app: a dictionary-style list of the .lua
files in the app folder, ENTER runs the selected one, and its output is printed
while it runs (ON or ESC interrupts it).

This file is kept DELIBERATELY SHORT: the calculator compiles a module's whole
source into RAM (156 KB of source cost ~6 MB of heap), so the comments and
docstrings live in main_notes.md next to it, in the same order.  See that file
for how the loader, the input hook, the streaming console and the screen UI
work, and README.md for the project as a whole.
"""
'''


def owner_map(tree):
    """{line: "Class.method"|"function"|"module"} for every line of the tree."""
    owners = {}

    def walk(node, name):
        body = getattr(node, 'body', [])
        for child in body:
            if isinstance(child, (ast.FunctionDef, ast.AsyncFunctionDef)):
                here = '%s.%s' % (name, child.name) if name else child.name
                for i in range(child.lineno,
                               getattr(child, 'end_lineno', child.lineno) + 1):
                    owners.setdefault(i, here)
                walk(child, here)
            elif isinstance(child, ast.ClassDef):
                for i in range(child.lineno,
                               getattr(child, 'end_lineno', child.lineno) + 1):
                    owners.setdefault(i, child.name)
                walk(child, child.name)
            elif isinstance(child, (ast.If, ast.Try, ast.With, ast.For,
                                    ast.While)):
                walk(child, name)

    walk(tree, '')
    return owners


def docstring_nodes(tree):
    """Every docstring statement: (node, owner) for module/class/function."""
    out = []
    for node in ast.walk(tree):
        if not isinstance(node, (ast.Module, ast.ClassDef, ast.FunctionDef,
                                 ast.AsyncFunctionDef)):
            continue
        body = getattr(node, 'body', None)
        if not body:
            continue
        first = body[0]
        if (isinstance(first, ast.Expr) and isinstance(first.value, ast.Constant)
                and isinstance(first.value.value, str)):
            if isinstance(node, ast.Module):
                continue                    # the header stays (see HEADER)
            name = getattr(node, 'name', '<module>')
            out.append((first, name))
    return out


def main(argv):
    src = open(MAIN).read()
    tree = ast.parse(src)
    owners = owner_map(tree)
    lines = src.split('\n')

    # ---- 1. collect and blank the comments ---------------------------------
    comments = []                            # (line, owner, text)
    drop = set()                             # line indexes to drop entirely
    keep_prefix = {}                         # line index -> code before the #
    for tok in tokenize.generate_tokens(io.StringIO(src).readline):
        if tok.type != tokenize.COMMENT:
            continue
        row, col = tok.start
        text = tok.string.lstrip('#').strip()
        before = lines[row - 1][:col]
        if before.strip():
            keep_prefix[row] = before.rstrip()
        else:
            drop.add(row)
        if text:
            comments.append((row, owners.get(row, '<module>'), text))

    # ---- 2. collect and blank the docstrings -------------------------------
    doc_lines = set()
    for node, name in docstring_nodes(tree):
        first, last = node.lineno, getattr(node, 'end_lineno', node.lineno)
        text = node.value.value.strip('\n')
        comments.append((first, name, text))
        for i in range(first, last + 1):
            doc_lines.add(i)

    # ---- 3. rebuild the source --------------------------------------------
    out = []
    for i, line in enumerate(lines, start=1):
        if i in doc_lines:
            continue
        if i in drop:
            continue
        if i in keep_prefix:
            line = keep_prefix[i]
        stripped = line.rstrip()
        if not stripped and out and not out[-1].strip():
            continue                        # collapse runs of blank lines
        out.append(stripped)
    while out and not out[0].strip():
        out.pop(0)
    while out and not out[-1].strip():
        out.pop()
    body = '\n'.join(out)
    header_end = body.index('"""', body.index('"""') + 3) + 3
    new_src = HEADER + body[header_end:].lstrip('\n') + '\n'

    # ---- 4. it must still be Python ---------------------------------------
    ast.parse(new_src)
    compile(new_src, MAIN, 'exec')

    if '--check' in argv:
        if new_src == src:
            print('calc/main.py is already stripped')
            return 0
        print('calc/main.py still carries %d comment(s)/docstring(s); '
              'run tools/strip_main.py' % len(comments))
        return 1

    # ---- 5. the notes, in file order --------------------------------------
    grouped = []
    for row, owner, text in sorted(comments, key=lambda c: c[0]):
        if grouped and grouped[-1][0] == owner:
            grouped[-1][1].append((row, text))
        else:
            grouped.append((owner, [(row, text)]))

    with open(NOTES, 'w') as fh:
        fh.write('# main.py -- the prose, moved out of the code\n\n')
        fh.write('`calc/main.py` is the launcher: it loads lua.elf, starts a run\n'
                 'on a firmware thread, streams the program\'s output, and draws\n'
                 'the program list.  Its comments and docstrings live here, in\n'
                 'the order they appeared, because the calculator compiles a\n'
                 'module\'s source into RAM (~6 MB for the 156 KB it was) and none\n'
                 'of this prose has to be in there.\n\n')
        fh.write('Regenerate with `python3 tools/strip_main.py` -- it extracts, it\n'
                 'does not delete: write the prose in main.py as usual, run the\n'
                 'tool, and it moves across.\n\n')
        for owner, items in grouped:
            fh.write('## `%s`\n\n' % owner)
            for row, text in items:
                body = '\n'.join('    ' + ln if ln.strip() else ''
                                 for ln in text.split('\n'))
                fh.write(body.rstrip() + '\n\n')
    open(MAIN, 'w').write(new_src)

    print('calc/main.py: %d -> %d bytes (-%.0f%%), %d note block(s) -> %s'
          % (len(src), len(new_src), 100.0 * (1 - len(new_src) / len(src)),
             len(grouped), os.path.basename(NOTES)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
