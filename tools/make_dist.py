#!/usr/bin/env python3
"""make_dist.py -- package primeLua for transfer to a calculator.

Two archives:

  dist/primeLua.hpappdir.zip   the flat app folder, exactly as it must end up
                               in C:\\DATA\\ (lua.elf, main.py, examples, doc)
  dist/primeLua-src.zip        the sources needed to rebuild it (the port
                               layer, the test suite, the build glue) -- the
                               upstream Lua tree is included as the src/ files
                               actually used, not the whole tarball

The deployment folder's top-level name inside the zip is what the calculator
sees, so both are built with the directory name as the archive prefix (the
same convention finalize.sh uses for the other projects in this workspace).
"""
import os
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.dirname(HERE)
DIST = os.path.join(TOP, 'dist')
APPNAME = 'primeLua.hpappdir'
APPDIR = os.path.join(os.path.dirname(TOP), APPNAME)
LUASRC = os.path.join(TOP, 'src', 'lua-5.4.7')
PRIMETCC = os.path.join(os.path.dirname(TOP), 'primetcc')

# The bits of primetcc's runtime primeLua actually compiles (heap allocator,
# string/memory core, the openlibm subset, and now the hardware libraries the
# Lua bindings wrap: gfx/input/sys/random/codec/fixmath).  Copied into the
# source archive so that it rebuilds outside this workspace.
THIRD_PARTY = [
    'rt/hp_rt.c', 'rt/hp_string.c', 'rt/hp_icall.h', 'rt/hp_string.h',
    'rt/hp_gfx.c', 'rt/hp_gfx.h',
    'rt/hp_input.c', 'rt/hp_input.h',
    'rt/hp_input_svc.c', 'rt/hp_input_svc.h',
    'rt/hp_sys.c', 'rt/hp_sys.h',
    'rt/hp_random.c', 'rt/hp_random.h',
    'rt/hp_codec.c', 'rt/hp_codec.h',
    'rt/hp_fixmath.c', 'rt/hp_fixmath.h',
    'rt/hp_fonts.c', 'rt/hp_fonts.h',
    # the firmware client library those .c files include
    'hp/hp_libc.c', 'hp/hp_libc.h', 'hp/hp_svc.s',
]
THIRD_PARTY_GLOB = [('rt/openlibm', ('.c', '.h'))]

SKIP_DIRS = {'__pycache__', 'build', 'dist', '.git'}


def add_tree(zf, root, prefix, filter_fn=None):
    n = 0
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for name in sorted(filenames):
            path = os.path.join(dirpath, name)
            rel = os.path.relpath(path, root)
            if filter_fn and not filter_fn(rel):
                continue
            zf.write(path, os.path.join(prefix, rel))
            n += 1
    return n


def main():
    os.makedirs(DIST, exist_ok=True)

    app_zip = os.path.join(DIST, APPNAME + '.zip')
    with zipfile.ZipFile(app_zip, 'w', zipfile.ZIP_DEFLATED) as zf:
        n = add_tree(zf, APPDIR, APPNAME)
    print('  %-28s %7d B  (%d files)'
          % (os.path.basename(app_zip), os.path.getsize(app_zip), n))

    src_zip = os.path.join(DIST, 'primeLua-src.zip')
    with zipfile.ZipFile(src_zip, 'w', zipfile.ZIP_DEFLATED) as zf:
        n = 0
        for rel in ('Makefile', 'README.md', 'api.md'):
            zf.write(os.path.join(TOP, rel), os.path.join('primeLua', rel))
            n += 1
        for sub in ('port', 'calc', 'examples', 'tests', 'tools'):
            n += add_tree(zf, os.path.join(TOP, sub), os.path.join('primeLua', sub))

        # the upstream tree: build glue + the sources primeLua compiles, so the
        # archive rebuilds without downloading anything.  src/Makefile is part
        # of that glue: `make hostref` -- the reference interpreter every
        # differential test compares against -- shells out to upstream's own
        # Makefile, so an archive without it can build lua.elf but cannot run
        # `make test`.
        zf.write(os.path.join(LUASRC, 'Makefile'),
                 os.path.join('primeLua', 'src/lua-5.4.7/Makefile'))
        zf.write(os.path.join(LUASRC, 'src', 'Makefile'),
                 os.path.join('primeLua', 'src/lua-5.4.7/src/Makefile'))
        zf.write(os.path.join(LUASRC, 'README'),
                 os.path.join('primeLua', 'src/lua-5.4.7/README'))
        n += 3
        n += add_tree(zf, os.path.join(LUASRC, 'src'),
                      os.path.join('primeLua', 'src/lua-5.4.7/src'),
                      lambda rel: rel.endswith(('.c', '.h', '.hpp')))

        # primetcc runtime files -> third_party/primetcc/ (the Makefile falls
        # back to that location when ../primetcc is not there)
        if os.path.isdir(PRIMETCC):
            for rel in THIRD_PARTY:
                src = os.path.join(PRIMETCC, rel)
                if os.path.exists(src):
                    zf.write(src, os.path.join('primeLua', 'third_party',
                                               'primetcc', rel))
                    n += 1
            for sub, exts in THIRD_PARTY_GLOB:
                n += add_tree(
                    zf, os.path.join(PRIMETCC, sub),
                    os.path.join('primeLua', 'third_party', 'primetcc', sub),
                    lambda rel, e=exts: rel.endswith(e))
        else:
            print('  note: %s not found, third_party/ left out' % PRIMETCC)
    print('  %-28s %7d B  (%d files)'
          % (os.path.basename(src_zip), os.path.getsize(src_zip), n))
    return 0


if __name__ == '__main__':
    sys.exit(main())
