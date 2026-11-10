#!/usr/bin/env python3
"""Deploy a SwitchRecomp PS5 title to an exFAT USB drive, with logs and verification.

The PS5 homebrew workflow on this machine puts each title at the drive root as
PPSAxxxxx/ (like the existing PPSA98515).  A title also needs its RomFS: svc_fs.c
looks for <g_datadir>/romfs, which is /app0/swr_data/romfs on PS5, so the assets
are materialised into PPSAxxxxx/swr_data/romfs/.

Principle (same as pipeline.py): never fail fast.  Every problem is recorded, one
run reports as many distinct issues as possible, and every claim is verified by
a log file rather than by eyeballing.

    python3 tools/ps5/deploy_usb.py \
        --mount /mnt/swr-usb \
        --app-src /data/switchrecomp/ps5full/PPSA99201 \
        --nup /data/dumps/bananza/Donkey Kong BananzaUpdate3.2.0DLC.nup \
        --package-index 0 --with-romfs \
        --run-dir /data/switchrecomp/runs/<id> --jobs 8
"""
import argparse, collections, hashlib, json, os, re, shutil, subprocess, sys, threading, time, traceback
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

BAD_CHARS = re.compile(r'[\\/:*?"<>|\x00-\x1f]')
CHUNK = 1 << 20


class Run:
    def __init__(self, d):
        self.dir = os.path.abspath(d)
        os.makedirs(self.dir, exist_ok=True)
        self.logf = open(os.path.join(self.dir, 'deploy.log'), 'a', encoding='utf-8')
        self.issues = []
        self.data = {}

    def log(self, stage, level, msg):
        line = '%s [%s] %-5s %s' % (time.strftime('%H:%M:%S'), stage, level, msg)
        print(line, flush=True)
        self.logf.write(line + '\n')
        self.logf.flush()

    def issue(self, stage, code, detail, severity='ERROR', **extra):
        rec = dict(stage=stage, code=code, detail=detail, severity=severity, **extra)
        self.issues.append(rec)
        self.log(stage, severity, '%s: %s' % (code, detail))

    def dump(self, name, obj):
        with open(os.path.join(self.dir, name), 'w', encoding='utf-8') as fo:
            json.dump(obj, fo, indent=2, ensure_ascii=False)

    def summary(self, lines):
        with open(os.path.join(self.dir, 'deploy_summary.txt'), 'w', encoding='utf-8') as fo:
            fo.write('\n'.join(lines) + '\n')


def sha256_file(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        while True:
            b = f.read(CHUNK)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def walk_files(root):
    out = []
    for dp, _, names in os.walk(root):
        for n in names:
            p = os.path.join(dp, n)
            out.append((os.path.relpath(p, root).replace(os.sep, '/'), p))
    return sorted(out)


def safe_rel(rel):
    """exFAT is case-insensitive and forbids a few characters; reject anything unsafe."""
    if not rel or rel.startswith('/') or '\\' in rel:
        return False
    parts = rel.split('/')
    if any(p in ('', '.', '..') or BAD_CHARS.search(p) for p in parts):
        return False
    return True


def stage_env(run, a):
    s = 'env'
    rec = dict(mount=a.mount, app_src=a.app_src, nup=a.nup, with_romfs=a.with_romfs)
    if not os.path.ismount(a.mount):
        run.issue(s, 'NOT_MOUNTED', a.mount)
        return rec
    df = subprocess.run(['df', '-B1', a.mount], capture_output=True, text=True)
    if df.returncode == 0:
        fields = df.stdout.splitlines()[-1].split()
        rec['free_bytes'] = int(fields[3])
        rec['total_bytes'] = int(fields[1])
        run.log(s, 'INFO', 'mount=%s free=%.1f GiB / total=%.1f GiB' % (
            a.mount, rec['free_bytes'] / 2**30, rec['total_bytes'] / 2**30))
    if not os.path.isdir(a.app_src):
        run.issue(s, 'APP_SRC_MISSING', a.app_src)
        return rec
    app_files = walk_files(a.app_src)
    rec['app_files'] = len(app_files)
    rec['app_bytes'] = sum(os.path.getsize(p) for _, p in app_files)
    eboot = os.path.join(a.app_src, 'eboot.bin')
    if not os.path.isfile(eboot):
        run.issue(s, 'EBOOT_MISSING', eboot)
    else:
        rec['eboot_size'] = os.path.getsize(eboot)
    run.log(s, 'INFO', 'app src: %d files / %.1f MiB, eboot.bin=%d bytes' % (
        rec['app_files'], rec['app_bytes'] / 2**20, rec.get('eboot_size', -1)))
    title = os.path.basename(os.path.normpath(a.app_src))
    rec['title'] = title
    rec['dest'] = os.path.join(a.mount, title)
    if os.path.exists(rec['dest']):
        run.log(s, 'WARN', 'destination already exists, will only fill/verify differences: %s' % rec['dest'])
    else:
        run.log(s, 'INFO', 'destination is new: %s' % rec['dest'])
    if a.with_romfs:
        if not os.path.isfile(a.nup):
            run.issue(s, 'NUP_MISSING', a.nup)
            return rec
        from nupread import Nup
        n = Nup(a.nup)
        if not (0 <= a.package_index < len(n.packages)):
            run.issue(s, 'BAD_PACKAGE_INDEX', '%d (0..%d)' % (a.package_index, len(n.packages) - 1))
            return rec
        pkg = n.packages[a.package_index]
        rec['package'] = dict(index=a.package_index, kind=pkg['kind'], version=pkg['version'],
                              title_id=pkg['title_id'])
        if pkg['romfs'] is None:
            run.issue(s, 'NO_ROMFS_IN_PACKAGE', str(a.package_index))
            return rec
        ents = list(n.walk(pkg['romfs']))
        rec['romfs_files'] = len(ents)
        rec['romfs_bytes'] = sum(n.files[fi][2] for _, fi in ents)
        run.log(s, 'INFO', 'romfs pkg%d (%s v%d): %d files / %.2f GiB' % (
            a.package_index, pkg['kind'], pkg['version'], len(ents), rec['romfs_bytes'] / 2**30))
        need = rec['romfs_bytes'] + rec['app_bytes']
        # exFAT clusters are 128 KiB; small files waste space. Allow generous headroom.
        if rec.get('free_bytes', 0) < need * 1.6 + (1 << 30):
            run.issue(s, 'DISK_SPACE_LOW', 'need ~%.1f GiB, free %.1f GiB' % (
                need * 1.6 / 2**30, rec.get('free_bytes', 0) / 2**30))
    run.data['env'] = rec
    return rec


def stage_app(run, a, env):
    s = 'app'
    src, dst = os.path.normpath(a.app_src) + os.sep, env['dest'] + os.sep
    os.makedirs(dst, exist_ok=True)
    # -rlt, not -a: exFAT has no POSIX owners/modes and chmod would fail.
    cmd = ['rsync', '-rlt', '--modify-window=2', '--itemize-changes', '--', src, dst]
    run.log(s, 'INFO', '$ ' + ' '.join(cmd))
    r = subprocess.run(cmd, capture_output=True, text=True, errors='replace')
    rec = dict(rc=r.returncode, copied=sum(1 for l in r.stdout.splitlines()
                                           if l.startswith(('>f', '<f', '>L', '<L'))))
    if r.returncode:
        run.issue(s, 'RSYNC_FAILED', (r.stderr or r.stdout)[-2000:])
    run.log(s, 'INFO', 'rsync rc=%d copied=%d files' % (r.returncode, rec['copied']))
    # independent verification: every source file must match size + sha256 at dest
    mism = []
    for rel, p in walk_files(a.app_src):
        q = os.path.join(env['dest'], rel.replace('/', os.sep))
        if not os.path.isfile(q):
            mism.append(dict(file=rel, why='missing'))
        elif os.path.getsize(q) != os.path.getsize(p):
            mism.append(dict(file=rel, why='size'))
        elif sha256_file(q) != sha256_file(p):
            mism.append(dict(file=rel, why='sha256'))
    rec['verified'] = len(walk_files(a.app_src)) - len(mism)
    rec['mismatches'] = mism[:50]
    if mism:
        run.issue(s, 'APP_VERIFY_MISMATCH', '%d files differ' % len(mism), count=len(mism))
    run.log(s, 'INFO', 'app verify: %d/%d files match size+sha256' % (
        rec['verified'], len(walk_files(a.app_src))))
    run.data['app'] = rec
    return rec


def stage_romfs(run, a, env):
    s = 'romfs'
    from nupread import Nup
    dst = os.path.join(env['dest'], 'swr_data', 'romfs')
    os.makedirs(dst, exist_ok=True)
    n = Nup(a.nup)
    ents = list(n.walk(n.packages[a.package_index]['romfs']))
    if a.limit:
        ents = ents[:a.limit]
    rec = dict(dest=dst, total=len(ents), extracted=0, cached=0, errors=[], conflict=0)
    # exFAT is case-insensitive: two names differing only by case would collide.
    seen = {}
    clean = []
    for rel, fi in ents:
        if not safe_rel(rel):
            rec['errors'].append(dict(file=rel, why='unsafe path'))
            continue
        k = rel.casefold()
        if k in seen:
            rec['conflict'] += 1
            rec['errors'].append(dict(file=rel, why='case-insensitive collision with %s' % seen[k]))
            continue
        seen[k] = rel
        clean.append((rel, fi))
    if rec['errors']:
        run.issue(s, 'ROMFS_BAD_PATHS', '%d unsafe/colliding names' % len(rec['errors']),
                  severity='WARN', count=len(rec['errors']))
    run.log(s, 'INFO', 'materialising %d files into %s (jobs=%d)' % (len(clean), dst, a.jobs))

    # Each worker needs its own Nup: extract_file() seeks the shared file handle.
    local = threading.local()

    def one(item):
        rel, fi = item
        out = os.path.join(dst, *rel.split('/'))
        size = n.files[fi][2]
        try:
            if os.path.isfile(out) and os.path.getsize(out) == size:
                return ('cached', rel, None)
            nn = getattr(local, 'nup', None)
            if nn is None:
                nn = local.nup = Nup(a.nup)
            nn.extract_file(fi, out)          # verifies size + SHA256 before rename
            return ('ok', rel, None)
        except Exception as e:
            return ('error', rel, '%s: %s' % (type(e).__name__, e))

    t0 = time.time()
    done = 0
    with ThreadPoolExecutor(max_workers=a.jobs) as ex:
        for status, rel, err in ex.map(one, clean):
            done += 1
            if status == 'ok':
                rec['extracted'] += 1
            elif status == 'cached':
                rec['cached'] += 1
            else:
                if len(rec['errors']) < 200:
                    rec['errors'].append(dict(file=rel, why=err))
            if done % 1000 == 0:
                run.log(s, 'INFO', '%d/%d  ok=%d cached=%d errors=%d  %.1fs' % (
                    done, len(clean), rec['extracted'], rec['cached'],
                    len(rec['errors']), time.time() - t0))
    rec['seconds'] = round(time.time() - t0, 1)
    bad = sum(1 for e in rec['errors'] if 'why' in e and e['why'] not in ('unsafe path',)
              and not str(e['why']).startswith('case-insensitive'))
    if bad:
        run.issue(s, 'ROMFS_EXTRACT_ERRORS', '%d files failed' % bad, count=bad)
    run.log(s, 'INFO', 'romfs done: ok=%d cached=%d errors=%d in %.1fs' % (
        rec['extracted'], rec['cached'], len(rec['errors']), rec['seconds']))
    run.data['romfs'] = rec
    return rec


def stage_verify(run, a, env):
    s = 'verify'
    from nupread import Nup
    rec = dict(app=run.data.get('app', {}).get('mismatches', []), romfs_errors=[], checked=0, matched=0)
    if a.with_romfs:
        dst = os.path.join(env['dest'], 'swr_data', 'romfs')
        n = Nup(a.nup)
        ents = list(n.walk(n.packages[a.package_index]['romfs']))
        if a.limit:
            ents = ents[:a.limit]
        t0 = time.time()
        for i, (rel, fi) in enumerate(ents):
            if not safe_rel(rel):
                continue
            p = os.path.join(dst, *rel.split('/'))
            rec['checked'] += 1
            want_size, want_sha = n.files[fi][2], n.files[fi][5]
            if not os.path.isfile(p):
                rec['romfs_errors'].append(dict(file=rel, why='missing'))
            elif os.path.getsize(p) != want_size:
                rec['romfs_errors'].append(dict(file=rel, why='size'))
            # NUP records store the raw 32-byte SHA-256 digest (see nupread.extract_file)
            elif a.verify_mode == 'sha' and want_sha and bytes.fromhex(sha256_file(p)) != bytes(want_sha):
                rec['romfs_errors'].append(dict(file=rel, why='sha256'))
            else:
                rec['matched'] += 1
            if (i + 1) % 5000 == 0:
                run.log(s, 'INFO', 'verified %d/%d ... %.1fs' % (i + 1, len(ents), time.time() - t0))
        if len(rec['romfs_errors']) > 200:
            rec['romfs_errors'] = rec['romfs_errors'][:200]
        if rec['romfs_errors']:
            run.issue(s, 'ROMFS_VERIFY_MISMATCH', '%d files differ' % len(rec['romfs_errors']),
                      count=len(rec['romfs_errors']))
        run.log(s, 'INFO', 'romfs verify (%s): %d/%d match' % (
            a.verify_mode, rec['matched'], rec['checked']))
    # FSELF sanity: the signed eboot must still be a SELF after the copy
    eb = os.path.join(env['dest'], 'eboot.bin')
    if os.path.isfile(eb):
        with open(eb, 'rb') as f:
            magic = f.read(16)
        rec['eboot_magic'] = magic.hex()
        run.log(s, 'INFO', 'eboot.bin size=%d magic=%s' % (os.path.getsize(eb), magic.hex()))
    else:
        run.issue(s, 'EBOOT_MISSING_AT_DEST', eb)
    run.data['verify'] = rec
    return rec


def stage_finalise(run, a, env):
    s = 'finalise'
    rec = {}
    r = subprocess.run(['sync'], capture_output=True, text=True)
    run.log(s, 'INFO', 'sync rc=%d' % r.returncode)
    rec['sync_rc'] = r.returncode
    if a.umount:
        r = subprocess.run(['sudo', '-n', 'umount', a.mount], capture_output=True, text=True)
        rec['umount_rc'] = r.returncode
        rec['umount_out'] = (r.stdout + r.stderr).strip()
        if r.returncode:
            run.issue(s, 'UMOUNT_FAILED', rec['umount_out'])
        else:
            run.log(s, 'INFO', 'unmounted %s - safe to unplug' % a.mount)
    else:
        run.log(s, 'INFO', 'mount left in place (--no-umount)')
    run.data['finalise'] = rec


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--mount', default='/mnt/swr-usb')
    ap.add_argument('--app-src', required=True)
    ap.add_argument('--nup', default='')
    ap.add_argument('--package-index', type=int, default=0)
    ap.add_argument('--with-romfs', action='store_true')
    ap.add_argument('--limit', type=int, default=0, help='only first N romfs files (sample run)')
    ap.add_argument('--jobs', type=int, default=8)
    ap.add_argument('--verify-mode', choices=('sha', 'size'), default='sha')
    ap.add_argument('--no-umount', dest='umount', action='store_false')
    ap.set_defaults(umount=True)
    ap.add_argument('--run-dir', required=True)
    a = ap.parse_args()
    run = Run(a.run_dir)
    run.log('main', 'INFO', 'deploy start: app=%s romfs=%s nup=%s' % (
        a.app_src, a.with_romfs, a.nup if a.with_romfs else '-'))
    env = stage_env(run, a)
    if run.issues and any(i['severity'] == 'ERROR' for i in run.issues):
        run.log('main', 'ERROR', 'env checks failed; continuing where possible')
    if os.path.isdir(a.app_src) and env.get('dest'):
        try:
            stage_app(run, a, env)
        except Exception as e:
            run.issue('app', 'STAGE_FAILED', '%s: %s' % (type(e).__name__, e))
            run.log('app', 'ERROR', traceback.format_exc())
    if a.with_romfs and env.get('romfs_files'):
        try:
            stage_romfs(run, a, env)
        except Exception as e:
            run.issue('romfs', 'STAGE_FAILED', '%s: %s' % (type(e).__name__, e))
            run.log('romfs', 'ERROR', traceback.format_exc())
        try:
            stage_verify(run, a, env)
        except Exception as e:
            run.issue('verify', 'STAGE_FAILED', '%s: %s' % (type(e).__name__, e))
    else:
        try:
            stage_verify(run, a, env)
        except Exception as e:
            run.issue('verify', 'STAGE_FAILED', '%s: %s' % (type(e).__name__, e))
    try:
        stage_finalise(run, a, env)
    except Exception as e:
        run.issue('finalise', 'STAGE_FAILED', '%s: %s' % (type(e).__name__, e))
    errs = collections.Counter(i['severity'] for i in run.issues)
    lines = ['USB deploy %s' % time.strftime('%Y-%m-%d %H:%M:%S'),
             'source      %s' % a.app_src,
             'destination %s' % env.get('dest', '?'),
             'ERROR %d / WARN %d' % (errs['ERROR'], errs['WARN']), '']
    e = run.data.get('env', {})
    lines.append('env      app %d files / %.1f MiB' % (e.get('app_files', 0), e.get('app_bytes', 0) / 2**20))
    if a.with_romfs:
        lines.append('env      romfs %d files / %.2f GiB (pkg%d %s v%s)' % (
            e.get('romfs_files', 0), e.get('romfs_bytes', 0) / 2**30,
            a.package_index, e.get('package', {}).get('kind', '?'), e.get('package', {}).get('version', '?')))
        r = run.data.get('romfs', {})
        lines.append('romfs    ok=%d cached=%d errors=%d %.1fs' % (
            r.get('extracted', 0), r.get('cached', 0), len(r.get('errors', [])), r.get('seconds', 0)))
    ap_ = run.data.get('app', {})
    lines.append('app      copied=%d verified=%d mismatches=%d' % (
        ap_.get('copied', 0), ap_.get('verified', 0), len(ap_.get('mismatches', []))))
    v = run.data.get('verify', {})
    lines.append('verify   romfs %d/%d match, eboot magic %s' % (
        v.get('matched', 0), v.get('checked', 0), v.get('eboot_magic', '?')))
    lines.append('finalise sync=%s umount=%s' % (
        run.data.get('finalise', {}).get('sync_rc', '?'),
        run.data.get('finalise', {}).get('umount_rc', 'skipped')))
    for i in run.issues:
        lines.append('  %-5s %-22s %s' % (i['severity'], i['code'], i['detail'][:200]))
    run.summary(lines)
    run.dump('deploy_report.json', dict(args=vars(a), data=run.data, issues=run.issues))
    run.log('main', 'INFO', 'deploy done: ERROR=%d WARN=%d -> %s' % (
        errs['ERROR'], errs['WARN'], os.path.join(run.dir, 'deploy_summary.txt')))
    return 1 if errs['ERROR'] else 0


if __name__ == '__main__':
    sys.exit(main())
