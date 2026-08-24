#!/usr/bin/env python3
"""stage-webm2.py — stage the embedded single-exe set as DUAL-STREAM packs.

For each manifest animation:
  * main  = the ORIGINAL upstream thumb WebM copied byte-for-byte (its video
            plane decodes fine with libvpx; only its BlockAdditional alpha is
            not independently decodable — which is why we supply a second stream)
  * alpha = a fresh ordinary VP9 stream whose gray Y plane IS the alpha mask,
            extracted ONCE with ffmpeg from the same source (proven decodable
            per frame). Optional --alpha-width downscales the mask (halved data,
            bilinear-upsampled to full canvas at runtime).

At runtime the two streams are decoded in memory by two independent libvpx
decoders and composited per frame, so every frame carries a FRESH alpha plane —
the "held first silhouette" dark-shadow bug (v7) is structurally impossible.

ffmpeg discovery order: --ffmpeg -> $FFMPEG env -> imageio_ffmpeg wheel ->
ffmpeg on PATH.

Usage:
  python tools/stage-webm2.py --src <thumb dir (91 *.webm)>
      --manifest <embedded-manifest.json>
      --out <assets/embedded>
      --config <default-config.jsonc>
      [--ffmpeg <path>] [--crf-alpha 34] [--alpha-width 320]
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # standalone/


def find_ffmpeg(explicit):
    if explicit and os.path.isfile(explicit):
        return explicit
    env = os.environ.get('FFMPEG')
    if env and os.path.isfile(env):
        return env
    for cand in (
        os.path.join(ROOT, 'ffmpegdl', 'ext', 'imageio_ffmpeg', 'binaries', 'ffmpeg-win-x86_64-v7.1.exe'),
        os.path.join(ROOT, '..', 'ffmpegdl', 'ext', 'imageio_ffmpeg', 'binaries', 'ffmpeg-win-x86_64-v7.1.exe'),
    ):
        if os.path.isfile(cand):
            return cand
    if shutil.which('ffmpeg'):
        return shutil.which('ffmpeg')
    sys.exit('ERR: ffmpeg not found (set FFMPEG=<ffmpeg.exe> or install imageio-ffmpeg in the ffmpegdl/ dir)')


def main():
    # Windows CI runners may default stdout to a charmap codepage; the logs here
    # carry Chinese animation names, so force UTF-8 output up front.
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
        sys.stderr.reconfigure(encoding='utf-8', errors='replace')
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--src', required=True, help='dir with the upstream thumb *.webm')
    ap.add_argument('--manifest', required=True, help='canonical embedded-manifest.json')
    ap.add_argument('--out', required=True, help='embedded staging dir')
    ap.add_argument('--config', default='', help='default-config.jsonc to stage')
    ap.add_argument('--ffmpeg', default='', help='explicit ffmpeg.exe path')
    ap.add_argument('--crf-alpha', type=int, default=34)
    ap.add_argument('--alpha-width', type=int, default=320,
                    help='alpha mask width (default 320, half of 640). 0 = full '
                         '640. The mask is bilinear-upscaled to the canvas at runtime.')
    args = ap.parse_args()

    ff = find_ffmpeg(args.ffmpeg)
    print(f'[stage2] ffmpeg: {ff}')

    with open(args.manifest, encoding='utf-8') as f:
        j = json.load(f)
    entries = j.get('animations', [])
    if not entries:
        sys.exit('ERR: manifest has no animations')

    by_name = {}
    by_index = {}
    for fn in sorted(os.listdir(args.src)):
        if fn.lower().endswith('.webm'):
            base = os.path.splitext(fn)[0]
            by_name[base] = os.path.join(args.src, fn)
            m = re.fullmatch(r'pack(\d{3})', base)
            if m:
                by_index[int(m.group(1))] = os.path.join(args.src, fn)

    os.makedirs(os.path.join(args.out, 'alpha'), exist_ok=True)
    out_man = {'version': 1, 'backend': 'webm2', 'animations': []}
    missing = []
    checked_src = None
    for i, e in enumerate(entries):
        name = e.get('name', '')
        src = by_name.get(name) or by_index.get(i)
        if not src:
            missing.append(name)
            continue
        if checked_src is None:
            checked_src = src
            probe = subprocess.run(
                [ff, '-y', '-hide_banner', '-loglevel', 'error',
                 '-c:v', 'libvpx-vp9', '-i', src, '-frames:v', '1',
                 '-f', 'rawvideo', '-pix_fmt', 'yuva420p', '-'],
                capture_output=True)
            if probe.returncode != 0 or len(probe.stdout) < 640 * 360:
                sys.exit(f'ERR: cannot decode first source ({src}). '
                         'Is this a real upstream thumb dir (VP9-alpha)?')
            aoff = 640 * 360 + 2 * (320 * 180)
            alpha = probe.stdout[aoff:aoff + 640 * 360]
            opa = sum(1 for b in alpha if b > 128)
            if opa > 640 * 360 * 95 // 100:
                sys.exit(f'ERR: {src} has no decodable alpha plane ({opa}/230400 '
                         'pixels opaque>128). Use REAL upstream thumbs.')
            print(f'[stage2] source alpha sanity OK: {opa} opaque>128 px on frame 0')
        # 1) main = ORIGINAL bytes, copied unchanged (no re-encode at all)
        main_dst = os.path.join(args.out, f'pack{i:03d}.webm')
        if os.path.abspath(src) != os.path.abspath(main_dst):
            shutil.copyfile(src, main_dst)
        # 2) alpha = fresh ordinary gray VP9 stream from the SAME source
        alpha_work = os.path.join(args.out, '.work-alpha', f'pack{i:03d}.webm')
        os.makedirs(os.path.dirname(alpha_work), exist_ok=True)
        vf = 'format=yuva420p,extractplanes=a'
        if args.alpha_width:
            ah = args.alpha_width * 9 // 16
            vf += f',scale={args.alpha_width}:{ah}:flags=area'
            pix = 'gray'
        else:
            pix = 'gray'
        cmd_alpha = [ff, '-y', '-hide_banner', '-loglevel', 'error',
                     '-c:v', 'libvpx-vp9', '-i', src,
                     '-an', '-vf', vf,
                     '-pix_fmt', pix,
                     '-c:v', 'libvpx-vp9', '-b:v', '0', '-crf', str(args.crf_alpha),
                     '-g', '60', '-row-mt', '1', '-threads', '4', alpha_work]
        r = subprocess.run(cmd_alpha, capture_output=True)
        if r.returncode != 0 or not os.path.isfile(alpha_work) or os.path.getsize(alpha_work) == 0:
            sys.exit(f'ERR alpha encode failed [{name}]: {r.stderr.decode(errors="replace")[:400]}')
        os.replace(alpha_work, os.path.join(args.out, 'alpha', f'pack{i:03d}.webm'))
        out_man['animations'].append({'name': name})
        if (i + 1) % 10 == 0:
            print(f'[stage2] {i + 1}/{len(entries)}: {name}')

    with open(os.path.join(args.out, 'embedded-manifest.json'), 'w', encoding='utf-8') as f:
        json.dump(out_man, f, ensure_ascii=False, indent=1)
    if args.config and os.path.isfile(args.config):
        shutil.copyfile(args.config, os.path.join(args.out, 'config.jsonc'))
    mb = sum(os.path.getsize(os.path.join(args.out, f'pack{i:03d}.webm'))
             for i in range(len(out_man['animations']))) / (1024 * 1024)
    ab = sum(os.path.getsize(os.path.join(args.out, 'alpha', f'pack{i:03d}.webm'))
             for i in range(len(out_man['animations']))) / (1024 * 1024)
    print(f'[stage2] staged {len(out_man["animations"])} animations '
          f'(original main {mb:.1f} MB + alpha {ab:.1f} MB) to {args.out}')
    if missing:
        print(f'[stage2] WARN {len(missing)} manifest names missing in --src: {missing}')
    if not out_man['animations']:
        sys.exit('ERR: nothing staged')


if __name__ == '__main__':
    main()