#!/usr/bin/env python3
"""Build PS5 sce_sys artwork (icon0.png + pic0/pic1.dds) from game key art.

The console's pic0.dds / pic1.dds are 3840x2160 DXGI_FORMAT_BC7_UNORM (98),
16 bytes per 4x4 block. Pillow/ImageMagick cannot write BC7, so the blocks are
encoded by the MIT/public-domain richgel999/bc7enc16 encoder through a tiny
ctypes wrapper; the DX10 header is copied verbatim from the boilerplate DDS so
the replacement is byte-format-compatible with the original package.

Upstream encoder (fetched once onto the build host):
  https://raw.githubusercontent.com/richgel999/bc7enc16/master/bc7enc16.c
  https://raw.githubusercontent.com/richgel999/bc7enc16/master/bc7enc16.h
  gcc -O3 -fPIC -shared -fopenmp -o libbc7enc16.so bc7enc16.c bc7enc16_wrapper.c -lm

    python3 tools/ps5/ps5_assets.py --sources-dir /data/switchrecomp/assets/bananza \
        --out-dir /data/switchrecomp/assets/ps5_sce_sys \
        --header-from /data/code/ps5-native-app-boilerplate/sce_sys/pic0.dds \
        --lib /data/switchrecomp/tools/bc7enc16/libbc7enc16.so \
        --icon-style A --cover-style A --run-dir /data/switchrecomp/runs/<id>
"""
import argparse
import ctypes
import hashlib
import json
import os
import struct
import time

from PIL import Image, ImageFilter

SOURCES = {
    'keyart_square': ('DK_Bananza_Final_Key_artwork_with_logo.png',
                      'https://mario.wiki.gallery/images/c/c4/DK_Bananza_Final_Key_artwork_with_logo.png'),
    'keyart_wide': ('DK_Bananza_Key_artwork_with_logo_alt.png',
                    'https://mario.wiki.gallery/images/0/0d/DK_Bananza_Key_artwork_with_logo_alt.png'),
    'box_na': ('DK_Bananza_Box_NA.png',
               'https://mario.wiki.gallery/images/2/2d/DK_Bananza_Box_NA.png'),
    'official_icon': ('DKB_Kong_Bananza_icon.png',
                      'https://mario.wiki.gallery/images/e/e4/DKB_Kong_Bananza_icon.png'),
}
DDS_HEADER_LEN = 148          # 128-byte DDS_HEADER + 20-byte DDS_HEADER_DXT10
DXGI_BC7_UNORM = 98
WIDTH, HEIGHT = 3840, 2160


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def log(run, msg):
    line = '[%s] %s' % (time.strftime('%H:%M:%S'), msg)
    print(line, flush=True)
    if run:
        with open(os.path.join(run, 'assets.log'), 'a', encoding='utf-8') as f:
            f.write(line + '\n')


def make_icon(sd, out, style):
    path = os.path.join(out, 'icon0.png')
    if style == 'A':
        im = Image.open(os.path.join(sd, SOURCES['keyart_square'][0])).convert('RGB')
        im = im.resize((512, 512), Image.LANCZOS)
        note = 'full 1920x1920 key art with logo, scaled'
    elif style == 'B':
        im = Image.open(os.path.join(sd, SOURCES['keyart_square'][0])).convert('RGB')
        w, h = im.size
        im = im.crop((int(w * .17), int(h * .12), int(w * .83), int(h * .88)))
        im = im.resize((512, 512), Image.LANCZOS)
        note = 'centre crop of key art, DK larger'
    else:
        ic = Image.open(os.path.join(sd, SOURCES['official_icon'][0])).convert('RGBA')
        ic = ic.resize((460, 460), Image.LANCZOS)
        im = Image.new('RGB', (512, 512), (24, 20, 18))
        im.paste(ic, (26, 26), ic)
        note = 'official 230px icon upscaled on dark background'
    im.save(path, 'PNG', optimize=True)
    return path, note


def make_cover(sd, out, style):
    path = os.path.join(out, 'cover_master_%s.png' % style)
    if style == 'A':
        im = Image.open(os.path.join(sd, SOURCES['keyart_wide'][0])).convert('RGB')
        im = im.resize((WIDTH, HEIGHT), Image.LANCZOS)
        note = '16:9 key art with logo, scaled to 3840x2160'
    else:
        box = Image.open(os.path.join(sd, SOURCES['box_na'][0])).convert('RGB')
        im = box.resize((WIDTH, HEIGHT), Image.LANCZOS).filter(ImageFilter.GaussianBlur(48))
        fit = box.copy()
        fit.thumbnail((1400, 2000), Image.LANCZOS)
        im.paste(fit, ((WIDTH - fit.width) // 2, (HEIGHT - fit.height) // 2))
        note = 'NA box art centred over a blurred full-bleed version'
    im.save(path, 'PNG')
    return path, note


def check_header(path):
    with open(path, 'rb') as f:
        hdr = f.read(DDS_HEADER_LEN)
    if len(hdr) != DDS_HEADER_LEN or hdr[:4] != b'DDS ':
        raise RuntimeError('%s is not a DX10 DDS (need %d header bytes)' % (path, DDS_HEADER_LEN))
    if hdr[84:88] != b'DX10':
        raise RuntimeError('%s has no DX10 extension' % path)
    # DDS_HEADER starts at byte 4: dwSize, dwFlags, dwHeight, dwWidth, ...
    h, w = struct.unpack('<II', hdr[12:20])
    fmt = struct.unpack('<I', hdr[128:132])[0]
    if (w, h) != (WIDTH, HEIGHT) or fmt != DXGI_BC7_UNORM:
        raise RuntimeError('%s expected %dx%d BC7(%d), got %dx%d format %d'
                           % (path, WIDTH, HEIGHT, DXGI_BC7_UNORM, w, h, fmt))
    return hdr


def encode_bc7(lib_path, png_path, header, dds_path):
    lib = ctypes.CDLL(lib_path)
    fn = lib.swr_bc7_encode
    fn.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
                   ctypes.c_void_p, ctypes.c_int]
    fn.restype = ctypes.c_int
    im = Image.open(png_path).convert('RGBA')
    if im.size != (WIDTH, HEIGHT):
        raise RuntimeError('cover must be %dx%d, got %s' % (WIDTH, HEIGHT, im.size))
    src = im.tobytes()
    n = WIDTH * HEIGHT                      # BC7: 1 byte per pixel
    dst = ctypes.create_string_buffer(n)
    srcbuf = ctypes.create_string_buffer(src, len(src))
    t0 = time.time()
    rc = fn(srcbuf, WIDTH, HEIGHT, dst, 0)
    if rc:
        raise RuntimeError('BC7 encoder returned %d' % rc)
    data = dst.raw
    if len(data) != n or data.count(0) == n:
        raise RuntimeError('BC7 output looks empty (%d bytes)' % len(data))
    with open(dds_path, 'wb') as f:
        f.write(header)
        f.write(data)
    return dict(bytes=os.path.getsize(dds_path),
                seconds=round(time.time() - t0, 1),
                nonzero_fraction=round(1.0 - data.count(0) / float(n), 4))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--sources-dir', required=True)
    ap.add_argument('--out-dir', required=True)
    ap.add_argument('--header-from', required=True, help='existing 3840x2160 BC7 pic0.dds')
    ap.add_argument('--lib', required=True, help='libbc7enc16.so')
    ap.add_argument('--icon-style', choices=['A', 'B', 'C'], default='A')
    ap.add_argument('--cover-style', choices=['A', 'B'], default='A')
    ap.add_argument('--run-dir', default='')
    a = ap.parse_args()
    os.makedirs(a.out_dir, exist_ok=True)
    if a.run_dir:
        os.makedirs(a.run_dir, exist_ok=True)
    rep = dict(sources={k: dict(file=v[0], url=v[1]) for k, v in SOURCES.items()},
               options=dict(icon_style=a.icon_style, cover_style=a.cover_style),
               outputs={})

    for k, (name, url) in SOURCES.items():
        p = os.path.join(a.sources_dir, name)
        if not os.path.isfile(p):
            raise SystemExit('missing source: %s (download from %s)' % (p, url))
        rep['sources'][k]['bytes'] = os.path.getsize(p)
        rep['sources'][k]['sha256'] = sha256(p)

    ip, inote = make_icon(a.sources_dir, a.out_dir, a.icon_style)
    rep['outputs']['icon0.png'] = dict(bytes=os.path.getsize(ip), sha256=sha256(ip), note=inote)
    log(a.run_dir, 'icon0.png: %s (%d bytes)' % (inote, os.path.getsize(ip)))

    cp, cnote = make_cover(a.sources_dir, a.out_dir, a.cover_style)
    log(a.run_dir, 'cover: %s' % cnote)
    header = check_header(a.header_from)
    log(a.run_dir, 'DX10 header from %s: %dx%d BC7_UNORM' % (a.header_from, WIDTH, HEIGHT))
    for name in ('pic0.dds', 'pic1.dds'):
        p = os.path.join(a.out_dir, name)
        r = encode_bc7(a.lib, cp, header, p)
        r['sha256'] = sha256(p)
        r['note'] = cnote
        rep['outputs'][name] = r
        log(a.run_dir, '%s: %d bytes, encoded in %.1fs, nonzero=%.3f'
            % (name, r['bytes'], r['seconds'], r['nonzero_fraction']))
    with open(os.path.join(a.out_dir, 'assets_build.json'), 'w', encoding='utf-8') as f:
        json.dump(rep, f, indent=2, ensure_ascii=False)
    if a.run_dir:
        with open(os.path.join(a.run_dir, 'assets_build.json'), 'w', encoding='utf-8') as f:
            json.dump(rep, f, indent=2, ensure_ascii=False)
    log(a.run_dir, 'done -> %s' % a.out_dir)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
