#!/usr/bin/env python3
"""Generate (and optionally upload) the PS5 RomFS directory manifest.

OrbisOS refuses opendir() on /app0 (measured: errno=1), so runtime/plat.c
falls back to this manifest to enumerate the deployed RomFS tree.  The NUP
package that was deployed to USB is the source of truth: no need to mount the
drive again, and no need to trust a directory listing from the console.

    python3 tools/ps5/romfs_manifest.py \
        --nup "/data/dumps/bananza/Donkey Kong BananzaUpdate3.2.0DLC.nup" \
        --package-index 0 --run-dir /data/switchrecomp/runs/<id> --upload

Every claim lands in romfs_manifest.log / romfs_manifest_report.json /
romfs_manifest_summary.txt.  --upload verifies size + SHA256 over FTP.
"""
import argparse
import hashlib
import io
import json
import os
import sys
import time
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

GUEST_ROOT = '/app0/swr_data/romfs'
# /data is read-only to the console's FTP server (STOR -> 550).  The package
# root is writable (that is how eboot.bin gets replaced), and /app0 maps to it.
REMOTE_DIR = '/mnt/usb1/PPSA99201'
REMOTE_NAME = 'romfs_manifest.txt'
DEFAULT_NUP = '/data/dumps/bananza/Donkey Kong BananzaUpdate3.2.0DLC.nup'


class Run:
    def __init__(self, d):
        self.dir = os.path.abspath(d)
        os.makedirs(self.dir, exist_ok=True)
        self.fh = open(os.path.join(self.dir, 'romfs_manifest.log'), 'a', encoding='utf-8')
        self.data = {'steps': []}
        self.issues = []

    def log(self, level, msg):
        line = '[%s] %-5s %s' % (time.strftime('%H:%M:%S'), level, msg)
        print(line, flush=True)
        self.fh.write(line + '\n')
        self.fh.flush()
        self.data['steps'].append({'level': level, 'msg': msg})

    def issue(self, code, detail):
        self.issues.append({'code': code, 'detail': detail})
        self.log('ERROR', '%s: %s' % (code, detail))

    def dump(self, path, obj):
        with open(os.path.join(self.dir, path), 'w', encoding='utf-8') as f:
            json.dump(obj, f, indent=2, ensure_ascii=False)


def sha256_bytes(b):
    return hashlib.sha256(b).hexdigest()


def build_manifest(nup_path, package_index, run):
    """Return (manifest bytes, report dict). Derives directories from paths."""
    from nupread import Nup
    n = Nup(nup_path)
    pkg = n.packages[package_index]
    root = pkg['romfs']
    run.log('INFO', 'nup=%s package=%d romfs_dir=%s' % (nup_path, package_index, root))
    if root is None:
        raise ValueError('package %d has no RomFS' % package_index)

    dirs = set()
    lines = []
    nfile = 0
    total = 0
    for rel, fi in n.walk(root):
        size = n.files[fi][2]
        parts = rel.split('/')
        name = parts[-1]
        parent = '/'.join(parts[:-1])
        if parent:
            # every ancestor directory becomes one D entry in its own parent
            acc = []
            for part in parts[:-1]:
                acc.append(part)
                dirs.add('/'.join(acc))
        gdir = GUEST_ROOT + (('/' + parent) if parent else '')
        lines.append('F\t%s\t%s\t%d' % (gdir, name, size))
        nfile += 1
        total += size

    for d in sorted(dirs):
        parts = d.split('/')
        parent = '/'.join(parts[:-1])
        gdir = GUEST_ROOT + (('/' + parent) if parent else '')
        lines.append('D\t%s\t%s\t0' % (gdir, parts[-1]))

    # Stable order: directories before files, alphabetical inside a directory.
    def key(line):
        kind, gdir, name, _ = line.split('\t', 3)
        return (gdir, 0 if kind == 'D' else 1, name)
    lines.sort(key=key)
    blob = ('\n'.join(lines) + '\n').encode('utf-8')

    per_dir = {}
    for line in lines:
        kind, gdir, name, sz = line.split('\t', 3)
        rec = per_dir.setdefault(gdir, {'dirs': 0, 'files': 0, 'bytes': 0})
        if kind == 'D':
            rec['dirs'] += 1
        else:
            rec['files'] += 1
            rec['bytes'] += int(sz)

    report = {
        'nup': nup_path,
        'package_index': package_index,
        'guest_root': GUEST_ROOT,
        'files': nfile,
        'directories': len(dirs),
        'entries': len(lines),
        'total_file_bytes': total,
        'total_file_gib': round(total / 2**30, 3),
        'manifest_bytes': len(blob),
        'manifest_sha256': sha256_bytes(blob),
        'top_directories': sorted(
            ({'dir': k, **v} for k, v in per_dir.items()),
            key=lambda x: -x['files'])[:10],
        'largest_files': [],
    }
    for rel, fi in n.walk(root):
        sz = n.files[fi][2]
        report['largest_files'].append({'path': rel, 'bytes': sz})
    report['largest_files'] = sorted(report['largest_files'],
                                     key=lambda x: -x['bytes'])[:10]
    run.log('INFO', 'entries=%d (files=%d dirs=%d) bytes=%.2f GiB manifest=%d bytes sha=%s' % (
        len(lines), nfile, len(dirs), total / 2**30, len(blob), report['manifest_sha256'][:16]))
    return blob, report


def upload(blob, want, host, port, run, remote_dir):
    import ftplib
    ftp = ftplib.FTP()
    ftp.connect(host, port, timeout=30)
    ftp.login()
    ftp.set_pasv(True)
    ftp.cwd(remote_dir)
    run.log('INFO', 'ftp %s:%d cwd=%s' % (host, port, remote_dir))
    ftp.storbinary('STOR ' + REMOTE_NAME, io.BytesIO(blob))
    size = ftp.size(REMOTE_NAME)
    buf = io.BytesIO()
    ftp.retrbinary('RETR ' + REMOTE_NAME, buf.write)
    back = buf.getvalue()
    got = sha256_bytes(back)
    rec = {'remote': remote_dir + '/' + REMOTE_NAME, 'size': size,
           'sha256': got, 'expected': want,
           'verified': size == len(blob) and got == want}
    run.log('INFO', 'upload: size=%s sha=%s verified=%s' % (size, got[:16], rec['verified']))
    if not rec['verified']:
        run.issue('MANIFEST_VERIFY_FAILED', json.dumps(rec))
    try:
        ftp.quit()
    except Exception:
        pass
    return rec


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--nup', default=DEFAULT_NUP)
    ap.add_argument('--package-index', type=int, default=0)
    ap.add_argument('--run-dir', required=True)
    ap.add_argument('--out', default='')
    ap.add_argument('--upload', action='store_true')
    ap.add_argument('--host', default='192.168.88.33')
    ap.add_argument('--port', type=int, default=2121)
    ap.add_argument('--remote-dir', default=REMOTE_DIR)
    a = ap.parse_args()
    run = Run(a.run_dir)
    run.data.update(nup=a.nup, package_index=a.package_index, upload=a.upload)
    try:
        blob, report = build_manifest(a.nup, a.package_index, run)
        local = a.out or os.path.join(run.dir, REMOTE_NAME)
        with open(local, 'wb') as f:
            f.write(blob)
        run.log('INFO', 'wrote %s (%d bytes)' % (local, len(blob)))
        run.data['report'] = report
        run.data['local_path'] = local
        if a.upload:
            run.data['ftp'] = upload(blob, report['manifest_sha256'], a.host, a.port, run,
                                     a.remote_dir)
        else:
            run.log('INFO', 'no --upload: manifest stays on this host')
    except Exception as e:
        run.issue('MANIFEST_FAILED', '%s: %s' % (type(e).__name__, e))
        run.log('ERROR', traceback.format_exc())

    run.data['issues'] = run.issues
    run.dump('romfs_manifest_report.json', run.data)
    lines = ['PS5 RomFS manifest %s' % time.strftime('%Y-%m-%d %H:%M:%S'),
             'nup      : %s (package %d)' % (a.nup, a.package_index),
             'root     : %s' % GUEST_ROOT,
             'entries  : %s' % run.data.get('report', {}).get('entries'),
             'files    : %s / dirs %s' % (run.data.get('report', {}).get('files'),
                                          run.data.get('report', {}).get('directories')),
             'bytes    : %s (%.2f GiB)' % (run.data.get('report', {}).get('total_file_bytes'),
                                           run.data.get('report', {}).get('total_file_gib', 0)),
             'manifest : %s bytes sha256=%s' % (run.data.get('report', {}).get('manifest_bytes'),
                                                run.data.get('report', {}).get('manifest_sha256')),
             'upload   : %s' % json.dumps(run.data.get('ftp', {}), ensure_ascii=False),
             'issues   : %s' % (run.issues or 'none')]
    with open(os.path.join(run.dir, 'romfs_manifest_summary.txt'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')
    print('\n'.join(lines))
    return 1 if run.issues else 0


if __name__ == '__main__':
    sys.exit(main())
