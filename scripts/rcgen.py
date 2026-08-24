#!/usr/bin/env python3
"""rcgen.py — generate standalone/src/embedded.rc from the embedded staging dir.

# Staging layout (root assets/embedded/, produced by stage-webm2.py for the
# dual-stream path):
#   embedded-manifest.json   {"animations":[{"name": ...}, ...]}  (order == PACK000..)
#   config.jsonc             default config JSONC text (RCDATA "CFG")
#   pack000.webm + alpha/    one asset per entry (RCDATA "PACK000"..) + partner
#                            alpha stream (RCDATA "PACK000A"..)

Generated embedded.rc uses string resource names (no numeric-id collisions with
the app icon) and absolute paths, so both windres (MinGW) and rc.exe (MSVC) can
resolve the files regardless of the working directory.
"""
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # standalone/
STAGE = os.path.join(ROOT, 'assets', 'embedded')
OUT = os.path.join(ROOT, 'src', 'embedded.rc')


def rc_path(p):
    """Absolute path with forward slashes (accepted by windres and rc.exe)."""
    return os.path.abspath(p).replace('\\', '/')


def find_asset(i):
    for ext in ('.webm', '.pka'):
        p = os.path.join(STAGE, f'pack{i:03d}{ext}')
        if os.path.isfile(p):
            return p
    return None


def main():
    if not os.path.isdir(STAGE):
        sys.exit(f'ERR: staging dir missing: {STAGE}\n  run: python tools/stage-webm2.py --src <thumb dir> ...')
    man = os.path.join(STAGE, 'embedded-manifest.json')
    cfg = os.path.join(STAGE, 'config.jsonc')
    if not os.path.isfile(man):
        sys.exit(f'ERR: missing {man}')
    if not os.path.isfile(cfg):
        sys.exit(f'ERR: missing {cfg}')

    with open(man, encoding='utf-8') as f:
        j = json.load(f)
    entries = j.get('animations', [])
    backend = j.get('backend', 'webm' if os.path.exists(os.path.join(STAGE, 'pack000.webm')) else 'pka')

    lines = [
        f'CFG RCDATA "{rc_path(cfg)}"',
        f'EMB RCDATA "{rc_path(man)}"',
    ]
    for i, e in enumerate(entries):
        asset = find_asset(i)
        if not asset:
            sys.exit(f'ERR: pack{i:03d} (.pka/.webm) missing (manifest entry {i}: {e.get("name", "?")})')
        lines.append(f'PACK{i:03d} RCDATA "{rc_path(asset)}"')
        # dual-stream (v8): optional partner alpha stream pack%d03dA.webm
        alpha = os.path.join(STAGE, 'alpha', f'pack{i:03d}.webm')
        if os.path.isfile(alpha):
            lines.append(f'PACK{i:03d}A RCDATA "{rc_path(alpha)}"')

    with open(OUT, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')
    print(f'wrote {OUT}: {len(entries)} {backend} assets + CFG + EMB')


if __name__ == '__main__':
    main()