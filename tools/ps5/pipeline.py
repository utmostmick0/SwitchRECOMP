#!/usr/bin/env python3
# SwitchRecomp - PS5 port: one-shot, log-driven feasibility pipeline.
# Principle: never fail fast. Every stage runs, every problem is recorded, and one
# conversion/compile pass reports as many distinct issues as possible.
#
# Stages:
#   env      host tools, SDK, dumps and disk inventory
#   inspect  every .nup -> packages, exefs/romfs inventory (no extraction)
#   extract  exefs of every package -> work/<game>/<pkg>/exefs, with magic checks
#   probe    nxloader.load_program per game + FULL AArch64 decode coverage of .text
#   recomp   switchrecomp.py --no-build per game (C generation only)
#   compile  host C compiler over generated chunks, all errors grouped by pattern
#   ps5      PS5 toolchain: xcompile probe + link/sign smoke + optional --target ps5 build
#   report   report.json + summary.txt
#
# All output goes to <work-root>/runs/<run-id>/. Nothing is deleted between runs.
import argparse, collections, hashlib, json, os, re, shutil, struct, subprocess, sys, time, traceback

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
sys.path.insert(0, ROOT)

DEFAULT_DUMPS = '/data/dumps'
DEFAULT_WORK = '/data/switchrecomp'
DEFAULT_SDK = '/data/code/EglantinePS5-build56/.deps/lapy-ps5-payload-sdk-v0.42'

STAGES = ['env', 'inspect', 'extract', 'probe', 'recomp', 'compile', 'ps5']


def now():
    return time.strftime('%Y-%m-%d %H:%M:%S')


class Run:
    def __init__(self, work_root, run_id, root):
        self.root = root
        self.work_root = work_root
        self.run_id = run_id
        self.dir = os.path.join(work_root, 'runs', run_id)
        self.work = os.path.join(work_root, 'work')
        os.makedirs(self.dir, exist_ok=True)
        os.makedirs(self.work, exist_ok=True)
        self.events = []
        self.issues = []
        self.stage_result = {}
        self.data = {}
        self._fh = {}
        self.pipeline_log = open(os.path.join(self.dir, 'pipeline.log'), 'a', encoding='utf-8')

    def log(self, stage, level, msg):
        line = '[%s] [%s] %-7s %s' % (now(), stage, level, msg)
        self.pipeline_log.write(line + '\n')
        self.pipeline_log.flush()
        if stage not in self._fh:
            self._fh[stage] = open(os.path.join(self.dir, '%02d-%s.log' % (STAGES.index(stage) + 1, stage)), 'a', encoding='utf-8')
        self._fh[stage].write(line + '\n')
        self._fh[stage].flush()
        self.events.append(dict(t=now(), stage=stage, level=level, msg=msg))
        if level in ('ERROR', 'WARN'):
            print(line, flush=True)

    def issue(self, stage, code, detail, severity='ERROR', **extra):
        rec = dict(stage=stage, code=code, detail=detail, severity=severity, **extra)
        self.issues.append(rec)
        self.log(stage, severity, '%s: %s' % (code, detail))

    def dump_json(self, name, obj):
        with open(os.path.join(self.dir, name), 'w', encoding='utf-8') as f:
            json.dump(obj, f, indent=2, ensure_ascii=False, default=str)

    def close(self):
        for f in self._fh.values():
            f.close()
        self.pipeline_log.close()


def cmd_exists(c):
    return shutil.which(c) is not None


def run_cmd(argv, cwd=None, timeout=1800, env=None):
    """-> dict(rc, out, err, seconds). Never raises on a non-zero exit."""
    t0 = time.time()
    try:
        r = subprocess.run(argv, cwd=cwd, capture_output=True, text=True,
                           errors='replace', timeout=timeout, env=env)
        return dict(rc=r.returncode, out=r.stdout or '', err=r.stderr or '', seconds=round(time.time() - t0, 1))
    except subprocess.TimeoutExpired as e:
        return dict(rc=-9, out=(e.stdout or b'').decode('utf-8', 'replace') if isinstance(e.stdout, bytes) else (e.stdout or ''),
                    err='TIMEOUT after %ss' % timeout, seconds=round(time.time() - t0, 1))
    except Exception as e:
        return dict(rc=-1, out='', err='%s: %s' % (type(e).__name__, e), seconds=round(time.time() - t0, 1))


# --------------------------------------------------------------------- env
def stage_env(run, a):
    s = 'env'
    run.log(s, 'INFO', 'repo=%s  work=%s  run=%s' % (run.root, run.dir, run.run_id))
    env = {'python': sys.version.split()[0], 'cwd': os.getcwd(), 'platform': sys.platform}
    mods = {}
    for m in ('zstandard', 'lz4', 'yaml'):
        try:
            __import__(m)
            mods[m] = 'ok'
        except Exception as e:
            mods[m] = 'MISSING (%s)' % type(e).__name__
            if m == 'zstandard':
                run.issue(s, 'PY_MOD_ZSTD', 'zstandard is required to read compressed .nup exefs', severity='ERROR')
            else:
                run.issue(s, 'PY_MOD_' + m.upper(), 'optional module %s missing: %s' % (m, e), severity='WARN')
    env['python_modules'] = mods

    tools = {}
    for c in ('python3', 'gcc', 'clang', 'clang-18', 'llvm-ar-18', 'make', 'rsync'):
        tools[c] = shutil.which(c) or 'MISSING'
    for c in ('gcc', 'python3'):
        if tools[c] == 'MISSING':
            run.issue(s, 'TOOL_' + c.upper(), '%s not in PATH' % c)
    env['host_tools'] = tools

    sdk = a.sdk
    sdkbin = os.path.join(sdk, 'bin')
    ps5 = {}
    for c in ('prospero-clang', 'prospero-clang++', 'prospero-lld', 'prospero-ar', 'prospero-objcopy', 'prospero-nm'):
        p = os.path.join(sdkbin, c)
        ps5[c] = p if os.path.isfile(p) and os.access(p, os.X_OK) else 'MISSING'
        if ps5[c] == 'MISSING':
            run.issue(s, 'SDK_TOOL_' + c.upper(), '%s missing under %s' % (c, sdkbin), severity='WARN')
    env['ps5_sdk'] = sdk
    env['ps5_tools'] = ps5
    run.log(s, 'INFO', 'PS5 SDK: %s (%d/%d tools present)' % (sdk, sum(1 for v in ps5.values() if v != 'MISSING'), len(ps5)))

    dumps = []
    for dp, _, fn in os.walk(a.dumps):
        for f in sorted(fn):
            if f.lower().endswith('.nup'):
                p = os.path.join(dp, f)
                dumps.append(dict(path=p, size=os.path.getsize(p)))
    dumps.sort(key=lambda d: d['path'])
    env['dumps'] = dumps
    for d in dumps:
        run.log(s, 'INFO', 'dump %s  %.2f GiB' % (d['path'], d['size'] / (1 << 30)))
    if not dumps:
        run.issue(s, 'NO_DUMPS', 'no .nup found under %s' % a.dumps)

    try:
        st = os.statvfs(a.work_root)
        env['work_free_gib'] = round(st.f_bavail * st.f_frsize / (1 << 30), 1)
        run.log(s, 'INFO', 'free space at work root: %.1f GiB' % env['work_free_gib'])
        if env['work_free_gib'] < 20:
            run.issue(s, 'LOW_DISK', 'only %.1f GiB free; C generation needs several GiB' % env['work_free_gib'], severity='WARN')
    except Exception as e:
        run.issue(s, 'STATVFS', str(e), severity='WARN')

    for host, port, label in ((a.ps5_host, a.ps5_port, 'PS5 FTP'),):
        import socket
        sock = socket.socket()
        sock.settimeout(1.5)
        try:
            sock.connect((host, port))
            env[label] = 'reachable'
            run.log(s, 'INFO', '%s %s:%d reachable' % (label, host, port))
        except Exception as e:
            env[label] = 'offline (%s)' % type(e).__name__
            run.log(s, 'INFO', '%s %s:%d offline (console may be powered off)' % (label, host, port))
        finally:
            sock.close()

    run.data['env'] = env
    return env


# ----------------------------------------------------------------- inspect
def stage_inspect(run, a):
    s = 'inspect'
    import nupread
    inv = []
    for d in run.data['env']['dumps']:
        rec = dict(path=d['path'], size=d['size'], packages=[])
        try:
            with nupread.Nup(d['path']) as n:
                rec['toc_packages'] = len(n.packages)
                rec['nfiles'] = len(n.files)
                rec['ndirs'] = len(n.dirs)
                for i, pkg in enumerate(n.packages):
                    p = dict(index=i, title_id='%016X' % pkg['title_id'], kind=pkg['kind'],
                             version=pkg['version'], standalone=pkg['standalone'])
                    if pkg['exefs'] is not None:
                        ef = list(n.walk(pkg['exefs']))
                        p['exefs_count'] = len(ef)
                        p['exefs_files'] = [dict(name=nm, size=n.files[fi][2], codec=n.files[fi][6],
                                                 file_index=fi) for nm, fi in ef]
                        p['exefs_bytes'] = sum(x['size'] for x in p['exefs_files'])
                    else:
                        p['exefs_count'] = 0
                        p['exefs_files'] = []
                        p['exefs_bytes'] = 0
                    if pkg['romfs'] is not None:
                        rf = list(n.walk(pkg['romfs']))
                        p['romfs_count'] = len(rf)
                        p['romfs_bytes'] = sum(n.files[fi][2] for _, fi in rf)
                    else:
                        p['romfs_count'] = 0
                        p['romfs_bytes'] = 0
                    rec['packages'].append(p)
                    run.log(s, 'INFO', '%s pkg%d %s %s v%d exefs=%d files/%.1f MiB romfs=%d files/%.2f GiB%s' % (
                        os.path.basename(d['path']), i, p['title_id'], p['kind'], p['version'],
                        p['exefs_count'], p['exefs_bytes'] / (1 << 20), p['romfs_count'],
                        p['romfs_bytes'] / (1 << 30), ' standalone' if p['standalone'] else ''))
                    names = {x['name'] for x in p['exefs_files']}
                    if p['exefs_count'] and 'main' not in names:
                        run.issue(s, 'EXEFS_NO_MAIN', '%s pkg%d has exefs but no `main`' % (d['path'], i), severity='WARN')
                    if p['exefs_count'] and 'main.npdm' not in names:
                        run.issue(s, 'EXEFS_NO_NPDM', '%s pkg%d has no main.npdm' % (d['path'], i), severity='WARN')
        except Exception as e:
            rec['error'] = '%s: %s' % (type(e).__name__, e)
            run.issue(s, 'NUP_READ_FAILED', '%s: %s' % (d['path'], rec['error']))
            run.log(s, 'ERROR', traceback.format_exc())
        inv.append(rec)
    run.data['inventory'] = inv
    run.dump_json('inventory.json', inv)
    return inv


def game_key(path):
    return os.path.basename(os.path.dirname(path))


# ----------------------------------------------------------------- extract
def stage_extract(run, a, inv):
    s = 'extract'
    import nupread
    out = []
    for rec in inv:
        if rec.get('error'):
            continue
        gkey = game_key(rec['path'])
        try:
            with nupread.Nup(rec['path']) as n:
                for pkg in rec['packages']:
                    if not pkg['exefs_count']:
                        continue
                    tgt = os.path.join(run.work, gkey, 'pkg%d_%s' % (pkg['index'], pkg['kind']))
                    ex = os.path.join(tgt, 'exefs')
                    os.makedirs(ex, exist_ok=True)
                    # a placeholder romfs keeps switchrecomp.py's copy_data happy without
                    # materialising 26 GiB of assets during a code-generation pass
                    os.makedirs(os.path.join(tgt, 'romfs'), exist_ok=True)
                    files = []
                    for f in pkg['exefs_files']:
                        dest = os.path.join(ex, f['name'])
                        if os.path.isfile(dest) and os.path.getsize(dest) == f['size']:
                            files.append(dict(name=f['name'], status='cached', size=f['size']))
                        else:
                            n.extract_file(f['file_index'], dest)
                            files.append(dict(name=f['name'], status='extracted', size=os.path.getsize(dest)))
                        with open(dest, 'rb') as fh:
                            magic = fh.read(4)
                        files[-1]['magic'] = magic.hex()
                        files[-1]['magic_ascii'] = magic.decode('latin1')
                        if f['name'] in ('main', 'rtld', 'sdk', 'subsdk0') or f['name'].startswith('subsdk'):
                            if magic != b'NSO0':
                                run.issue(s, 'NSO_MAGIC', '%s/%s expected NSO0, got %r' % (gkey, f['name'], magic))
                        elif f['name'] == 'main.npdm' and magic != b'META':
                            run.issue(s, 'NPDM_MAGIC', '%s/main.npdm expected META, got %r' % (gkey, magic))
                    sha = hashlib.sha256()
                    for f in sorted(files, key=lambda x: x['name']):
                        with open(os.path.join(ex, f['name']), 'rb') as fh:
                            for chunk in iter(lambda: fh.read(1 << 20), b''):
                                sha.update(chunk)
                    entry = dict(game=gkey, package_index=pkg['index'], kind=pkg['kind'],
                                 title_id=pkg['title_id'], version=pkg['version'],
                                 dir=tgt, exefs=ex, files=files, sha256=sha.hexdigest()[:16])
                    out.append(entry)
                    run.log(s, 'INFO', '%s pkg%d -> %s (%d files, %.1f MiB, sha %s)' % (
                        gkey, pkg['index'], tgt, len(files),
                        sum(f['size'] for f in files) / (1 << 20), entry['sha256']))
        except Exception as e:
            run.issue(s, 'EXTRACT_FAILED', '%s: %s: %s' % (rec['path'], type(e).__name__, e))
            run.log(s, 'ERROR', traceback.format_exc())
    run.data['extracted'] = out
    run.dump_json('extracted.json', out)
    return out


# -------------------------------------------------------------------- probe
def load_modules_resilient(exefs_dir, run, stage, tag):
    """Load NSOs one by one: a broken module is recorded, not allowed to hide
    the rest. Bases stay deterministic for the modules that did load.
    """
    import nxloader
    mods, errors = [], []
    npdm = None
    p = os.path.join(exefs_dir, 'main.npdm')
    if os.path.isfile(p):
        with open(p, 'rb') as f:
            npdm = f.read()
    base = nxloader.CODE_BASE
    for n in nxloader.MODULE_ORDER:
        p = os.path.join(exefs_dir, n)
        if not os.path.isfile(p):
            continue
        try:
            m = nxloader.load_nso(p, n)
            m.base = base
            base += m.size + nxloader.PAGE
            mods.append(m)
        except Exception as ex:
            msg = '%s: %s' % (type(ex).__name__, ex)
            errors.append(dict(module=n, error=msg))
            run.issue(stage, 'MODULE_LOAD_FAILED', '%s/%s: %s' % (tag, n, msg))
    return mods, npdm, errors


def make_disassembler():
    try:
        from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN
        md = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN)

        def dis(w):
            ins = list(md.disasm(w.to_bytes(4, 'little'), 0x1000))
            if not ins:
                return '<unrecognised>', '<unrecognised>'
            return ins[0].mnemonic, (ins[0].mnemonic + ' ' + ins[0].op_str).strip()
        return dis
    except ImportError:
        return lambda w: ('<no-capstone>', '<no-capstone>')


def stage_probe(run, a, extracted):
    """Full-architecture probe: load every module and decode EVERY .text word.

    One pass reports the complete instruction gap: all distinct unknown encodings
    (not just the first N), zero-fill padding separated from real instructions,
    and every encoding labelled with its AArch64 mnemonic when capstone is around.
    """
    s = 'probe'
    import nxloader, a64
    dis = make_disassembler()
    cov, gap_words, gap_mn = [], collections.Counter(), collections.Counter()
    selected = [e for e in extracted if not a.games or e['game'] in a.games]
    if not selected:
        run.issue(s, 'NO_GAME_SELECTED', 'no extracted game matched --games=%s' % ','.join(a.games or []), severity='WARN')
    for e in selected:
        tag = '%s/p%d' % (e['game'], e['package_index'])
        rec = dict(game=e['game'], package_index=e['package_index'], dir=e['dir'], modules=[], decode={})
        try:
            try:
                std, _, _ = nxloader.load_program(e['dir'])
                rec['load_program'] = 'OK (%d modules)' % len(std)
            except Exception as ex:
                rec['load_program'] = 'FAIL: %s: %s' % (type(ex).__name__, ex)
                run.issue(s, 'STANDARD_LOAD_FAILED', '%s: %s' % (tag, rec['load_program']))

            mods, npdm, errors = load_modules_resilient(e['exefs'], run, s, tag)
            rec['module_errors'] = errors
            rec['npdm'] = nxloader.parse_npdm(npdm)
            rec['npdm']['title_id'] = '%016X' % rec['npdm']['title_id']
            run.log(s, 'INFO', '%s: %d modules loaded, %d failed, npdm aspace=%s' % (
                tag, len(mods), len(errors), rec['npdm']['aspace']))

            for m in mods:
                mi = dict(name=m.name, base='%010X' % m.base, size=m.size,
                          text_off=m.text[0], text_size=m.text[1], ro_size=m.ro[1],
                          data_size=m.data[1], bss=m.bss, syms=len(m.syms),
                          funcs=len(m.funcs), fde=len(m.fde), relptrs=len(m.relptrs),
                          imports=len(m.imports), mod0=m.mod0 is not None,
                          build_id=m.build_id.hex()[:16])
                rec['modules'].append(mi)
                run.log(s, 'INFO', '  %-8s base=%s text=%.2f MiB syms=%d funcs=%d fde=%d mod0=%s' % (
                    m.name, mi['base'], m.text[1] / (1 << 20), mi['syms'], mi['funcs'], mi['fde'], mi['mod0']))
                if not mi['mod0']:
                    run.issue(s, 'NO_MOD0', '%s/%s has no MOD0 (symbols/FDE unavailable)' % (tag, m.name), severity='WARN')

                # ---- full .text decode coverage --------------------------------
                off, size = m.text
                mv = memoryview(m.image)[off:off + (size & ~3)].cast('I')
                total = min(a.limit_words or len(mv), len(mv))
                cache, unknown, zero, samples = {}, collections.Counter(), 0, {}
                first_pc = m.base + off
                for i in range(total):
                    w = int(mv[i])
                    d = cache.get(w)
                    if d is None:
                        d = a64.decode(w)
                        cache[w] = d
                    if d < 0:
                        unknown[w] += 1
                        if w not in samples:
                            samples[w] = []
                        if len(samples[w]) < 3:
                            samples[w].append('%X' % (first_pc + i * 4))
                    if w == 0:
                        zero += 1
                # .text contains literal pools and alignment padding. UDF and
                # encodings capstone cannot parse are inline data, not real
                # instructions, so they must not inflate the instruction gap.
                # stg = MTE store allocation tag: the host emulates no tags, so
                # it is a semantic no-op; it also only ever shows up inside .text
                # literal pools in these dumps.
                data_mn = ('udf', '<unrecognised>', '<no-capstone>', 'stg')
                all_unk, real_words, real_unknown, real_distinct, inline_data = [], set(), 0, 0, 0
                mn_count, bytec = collections.Counter(), collections.Counter()
                for w, c in unknown.most_common():
                    mn, text = dis(w)
                    all_unk.append(dict(word='%08X' % w, count=c, mnemonic=mn, disasm=text,
                                        sample_pcs=samples[w]))
                    mn_count[mn] += c
                    bytec['%02X' % (w >> 24)] += c
                    if w == 0:
                        continue                      # zero fill is counted separately
                    if mn in data_mn:
                        inline_data += c
                        continue                      # literal pool / inline data
                    real_unknown += c
                    real_distinct += 1
                    real_words.add(w)
                    gap_words[w] += c
                    gap_mn[mn] += c
                dr = dict(module=m.name, words_scanned=total, text_words=size // 4,
                          unique_encodings=len(cache), unique_unknown=len(unknown),
                          unknown_words=sum(unknown.values()), zero_fill_words=zero,
                          inline_data_words=inline_data,
                          real_unknown_words=real_unknown,
                          real_unknown_distinct=real_distinct,
                          unknown_pct=round(100.0 * sum(unknown.values()) / total, 4) if total else 0.0,
                          real_coverage_pct=round(100.0 * (total - zero - inline_data - real_unknown) / total, 4) if total else 0.0,
                          top_unknown=all_unk[:a.top_unknown],
                          by_mnemonic=dict(mn_count.most_common()),
                          by_top_byte=dict(bytec.most_common()))
                rec['decode'].setdefault('modules', []).append(dr)
                run.log(s, 'INFO', '  decode %-8s %d words, real unknown %d (%.4f%%), zero-fill %d, real coverage %.4f%%' % (
                    m.name, total, real_unknown, 100.0 * real_unknown / total if total else 0,
                    zero, dr['real_coverage_pct']))
                run.log(s, 'INFO', '  top mnemonics: %s' % ', '.join(
                    '%s x%d' % (k, v) for k, v in mn_count.most_common(8)))
                if real_unknown:
                    run.issue(s, 'INSTRUCTION_GAP', '%s/%s: %d words in %d encodings unsupported; top: %s' % (
                        tag, m.name, real_unknown, dr['real_unknown_distinct'],
                        ', '.join('%s x%d' % (k, v) for k, v in mn_count.most_common(5) if k not in ('udf',))),
                        severity='WARN')
        except Exception as ex:
            rec['error'] = '%s: %s' % (type(ex).__name__, ex)
            run.issue(s, 'PROBE_FAILED', '%s: %s' % (tag, rec['error']))
            run.log(s, 'ERROR', traceback.format_exc())
        cov.append(rec)
    run.data['probe'] = cov
    run.dump_json('decode_coverage.json', cov)
    gaps = dict(by_mnemonic=dict(gap_mn.most_common()),
                distinct_encodings=len(gap_words),
                encodings=[dict(word='%08X' % w, count=c, disasm=dis(w)[1])
                           for w, c in gap_words.most_common()])
    run.dump_json('opcode_gaps.json', gaps)
    run.data['opcode_gaps'] = dict(by_mnemonic=gaps['by_mnemonic'], distinct_encodings=gaps['distinct_encodings'])
    run.log(s, 'INFO', 'opcode gaps: %d distinct real encodings, %d mnemonic groups -> opcode_gaps.json' % (
        len(gap_words), len(gap_mn)))
    return cov


# ------------------------------------------------------------------- recomp
def stage_recomp(run, a, extracted):
    s = 'recomp'
    results = []
    selected = [e for e in extracted if not a.games or e['game'] in a.games]
    outroot = os.path.join(run.work, 'out')
    os.makedirs(outroot, exist_ok=True)
    for e in selected:
        title = 'swr_%s_p%d' % (e['game'], e['package_index'])
        argv = [sys.executable, os.path.join(run.root, 'switchrecomp.py'), e['dir'],
                '-o', title, '--outdir', outroot, '--no-build', '--chunk', str(a.chunk)]
        run.log(s, 'INFO', 'RUN %s' % ' '.join(argv))
        r = run_cmd(argv, cwd=run.root, timeout=a.recomp_timeout)
        logname = 'recomp_%s_p%d.log' % (e['game'], e['package_index'])
        with open(os.path.join(run.dir, logname), 'w', encoding='utf-8') as f:
            f.write('$ %s\n\n' % ' '.join(argv))
            f.write('--- stdout ---\n' + r['out'] + '\n--- stderr ---\n' + r['err'])
        bdir = os.path.join(outroot, title + '_build')
        cfiles = sorted(f for f in os.listdir(bdir) if f.endswith('.c')) if os.path.isdir(bdir) else []
        total = sum(os.path.getsize(os.path.join(bdir, f)) for f in cfiles) if cfiles else 0
        rec = dict(game=e['game'], package_index=e['package_index'], title=title,
                   rc=r['rc'], seconds=r['seconds'], build_dir=bdir,
                   c_files=len(cfiles), c_bytes=total, log=logname,
                   stdout_tail=r['out'][-2000:], stderr_tail=r['err'][-4000:])
        results.append(rec)
        run.log(s, 'INFO', '%s -> rc=%d in %.1fs, %d C files / %.1f MiB' % (
            title, r['rc'], r['seconds'], len(cfiles), total / (1 << 20)))
        if r['rc'] != 0:
            run.issue(s, 'RECOMP_FAILED', '%s rc=%d: %s' % (title, r['rc'], (r['err'] or r['out'])[-500:]),
                      log=logname)
        elif not cfiles:
            run.issue(s, 'RECOMP_NO_C', '%s succeeded but produced no C files' % title, severity='WARN')
        for pat, code, sev in ((r'Traceback', 'RECOMP_TRACEBACK', 'ERROR'),
                               (r'Cannot load the game', 'RECOMP_LOAD', 'ERROR'),
                               (r'error translating', 'RECOMP_TRANSLATE', 'ERROR')):
            if re.search(pat, r['out'] + r['err']):
                run.issue(s, code, '%s: %s found in %s' % (title, pat, logname), severity=sev)
    run.data['recomp'] = results
    return results


# ------------------------------------------------------------------ compile
def normalise_error(line):
    """Group compiler diagnostics by their shape, not by file/line/number."""
    x = re.sub(r'code_\d+\.c|module\.c|[^/\s]+\.c', '<file>', line)
    x = re.sub(r':\d+:\d+', ':<line>:<col>', x)
    x = re.sub(r'\b0x[0-9a-fA-F]+\b', '<hex>', x)
    x = re.sub(r'\b\d+\b', '<n>', x)
    return x.strip()


def stage_compile(run, a, recomp_results):
    s = 'compile'
    from concurrent.futures import ThreadPoolExecutor
    cc = a.cc or shutil.which('gcc') or 'gcc'
    report = []
    for rr in recomp_results:
        if rr['rc'] != 0 or not rr['c_files']:
            continue
        bdir = rr['build_dir']
        files = sorted(f for f in os.listdir(bdir) if f.endswith('.c'))
        if a.max_compile_files and len(files) > a.max_compile_files:
            step = len(files) / float(a.max_compile_files)
            picked = [files[min(len(files) - 1, int(i * step))] for i in range(a.max_compile_files)]
            if 'module.c' not in picked and 'module.c' in files:
                picked.append('module.c')
            run.log(s, 'INFO', '%s: sampling %d of %d C files evenly' % (rr['title'], len(picked), len(files)))
        else:
            picked = files
        objdir = os.path.join(run.dir, 'objs', rr['title'])
        os.makedirs(objdir, exist_ok=True)

        def one(name):
            src = os.path.join(bdir, name)
            argv = [cc, '-std=gnu17', '-fsyntax-only', '-w', '-I', os.path.join(run.root, 'runtime'),
                    '-fno-strict-aliasing', src]
            r = run_cmd(argv, timeout=a.compile_timeout)
            return name, r

        t0 = time.time()
        err_lines = []
        with ThreadPoolExecutor(max_workers=a.jobs) as pool:
            for name, r in pool.map(one, picked):
                if r['rc'] == 0:
                    continue
                text = (r['err'] or r['out']).strip().splitlines()
                found = [dict(file=name, raw=ln.strip()) for ln in text
                         if 'error:' in ln or 'fatal error:' in ln]
                if not found:
                    found.append(dict(file=name, raw='rc=%d %s' % (r['rc'], (r['err'] or r['out'])[:300])))
                err_lines.extend(found)
        groups = collections.Counter(normalise_error(x['raw']) for x in err_lines)
        rec = dict(title=rr['title'], compiler=cc, files_total=len(files), files_checked=len(picked),
                   seconds=round(time.time() - t0, 1), error_lines=len(err_lines),
                   ok_files=len(picked) - len({x['file'] for x in err_lines}),
                   failed_files=len({x['file'] for x in err_lines}),
                   error_groups=[dict(pattern=p, count=c) for p, c in groups.most_common(40)],
                   samples=err_lines[:40])
        report.append(rec)
        run.log(s, 'INFO', '%s: %d/%d files compiled clean in %.1fs, %d distinct error shapes' % (
            rr['title'], rec['ok_files'], rec['files_checked'], rec['seconds'], len(groups)))
        if err_lines:
            run.issue(s, 'C_COMPILE_ERRORS', '%s: %d/%d files failed, %d distinct error shapes (see compile_errors.json)' % (
                rr['title'], rec['failed_files'], rec['files_checked'], len(groups)), severity='WARN')
    run.data['compile'] = report
    run.dump_json('compile_errors.json', report)
    return report


# ---------------------------------------------------------------------- ps5
def stage_ps5(run, a, recomp_results):
    """PS5 toolchain validation. Every sub-check runs even when an earlier one
    fails, and each machine-readable report is folded into the run's issues."""
    s = 'ps5'
    here = os.path.join(run.root, 'tools', 'ps5')
    results = []

    def collect(label, argv, timeout, report_name, cwd=None):
        r = run_cmd(argv, cwd=cwd or run.root, timeout=timeout)
        d = os.path.join(run.dir, label)
        os.makedirs(d, exist_ok=True)
        rec = dict(label=label, rc=r['rc'], seconds=r['seconds'], dir=d,
                   stdout_tail=(r['out'] or '')[-1500:], stderr_tail=(r['err'] or '')[-1500:])
        rp = os.path.join(d, report_name)
        if os.path.isfile(rp):
            try:
                with open(rp, encoding='utf-8') as f:
                    j = json.load(f)
                rec['issues'] = j.get('issues', [])
                rec['summary'] = {k: v for k, v in j.get('data', {}).items()
                                  if k in ('runtime', 'link', 'convert', 'package', 'verify')}
                if report_name == 'xcompile_errors.json':
                    rec['summary'] = dict(ok=j.get('ok'), total=j.get('total'),
                                          error_shapes=len(j.get('error_groups', [])))
                    if j.get('failed'):
                        run.issue(s, 'PS5_XCOMPILE_ERRORS',
                                  '%s: %d/%d sources failed, %d distinct error shapes: %s' % (
                                      label, j['failed'], j.get('total', 0),
                                      len(j.get('error_groups', [])),
                                      ' | '.join(g['pattern'] for g in j.get('error_groups', [])[:5])))
                for iss in rec['issues']:
                    run.issue(s, 'PS5_' + iss.get('code', 'UNKNOWN'),
                              '%s: %s' % (label, iss.get('detail', '')),
                              severity=iss.get('severity', 'ERROR'))
            except Exception as e:
                run.issue(s, 'PS5_REPORT_UNREADABLE', '%s: %s' % (rp, e), severity='WARN')
        else:
            rec['issues'] = []
            run.issue(s, 'PS5_REPORT_MISSING', '%s produced no %s (rc=%d)' % (label, report_name, r['rc']))
        if r['rc']:
            run.issue(s, 'PS5_CHECK_FAILED', '%s rc=%d: %s' % (label, r['rc'], (r['err'] or r['out'])[-400:]))
        run.log(s, 'INFO', '%s rc=%d in %.1fs, %d reported issues' % (label, r['rc'], r['seconds'], len(rec['issues'])))
        results.append(rec)
        return rec

    env_common = dict(repo=run.root, sdk=a.sdk, llvm=a.ps5_llvm_config, boiler=a.ps5_boilerplate,
                      jobs=a.jobs, opt=a.ps5_opt)
    # 1) the whole link -> convert -> sign -> inspect chain on a tiny program
    collect('ps5_smoke',
            [sys.executable, os.path.join(here, 'ps5build.py'), '--smoke',
             '--repo', env_common['repo'], '--sdk', env_common['sdk'],
             '--llvm-config', env_common['llvm'], '--boilerplate', env_common['boiler'],
             '--run-dir', os.path.join(run.dir, 'ps5_smoke'),
             '--outdir', os.path.join(run.dir, 'ps5_smoke'),
             '--jobs', str(env_common['jobs']), '--opt', env_common['opt']],
            a.ps5_timeout, 'ps5build_report.json')

    # 2) cross-compile every runtime source plus a sample of each generated tree
    for rr in recomp_results:
        if rr.get('rc') != 0 or not rr.get('c_files'):
            continue
        d = os.path.join(run.dir, 'xcompile_' + rr['title'])
        collect('xcompile_' + rr['title'],
                [sys.executable, os.path.join(here, 'xcompile_check.py'),
                 '--run-dir', d, '--repo', env_common['repo'], '--sdk', env_common['sdk'],
                 '--llvm-config', env_common['llvm'], '--code-dir', rr['build_dir'],
                 '--max-code-files', str(a.ps5_xcompile_files), '--jobs', str(env_common['jobs'])],
                a.ps5_timeout, 'xcompile_errors.json')

    # 3) optional real --target ps5 CLI build (generation + static archive + package)
    if a.ps5_game_dir:
        outdir = os.path.join(run.dir, 'ps5_app')
        os.makedirs(outdir, exist_ok=True)
        log = os.path.join(run.dir, 'ps5_cli.log')
        argv = [sys.executable, os.path.join(run.root, 'switchrecomp.py'), a.ps5_game_dir,
                '-o', a.ps5_title, '--outdir', outdir, '--target', 'ps5', '--backend', 'ps5',
                '--jobs', str(a.jobs), '-O', a.ps5_opt, '--ps5-sdk', a.sdk,
                '--llvm-config', a.ps5_llvm_config, '--ps5-boilerplate', a.ps5_boilerplate,
                '--ps5-title-id', a.ps5_title_id, '--interp-only' if a.ps5_interp_only else '--chunk',
                str(a.chunk) if not a.ps5_interp_only else '']
        argv = [x for x in argv if x]
        r = run_cmd(argv, cwd=run.root, timeout=a.ps5_timeout)
        with open(log, 'w', encoding='utf-8') as f:
            f.write('$ %s\n\n--- stdout ---\n%s\n--- stderr ---\n%s\n' % (' '.join(argv), r['out'], r['err']))
        rec = dict(label='ps5_cli', rc=r['rc'], seconds=r['seconds'], log=log,
                   stdout_tail=r['out'][-2000:], stderr_tail=r['err'][-2000:])
        run.log(s, 'INFO', 'ps5_cli rc=%d in %.1fs' % (r['rc'], r['seconds']))
        if r['rc']:
            run.issue(s, 'PS5_CLI_FAILED', 'switchrecomp.py --target ps5 rc=%d: %s' % (
                r['rc'], (r['err'] or r['out'])[-500:]))
        results.append(rec)

    run.data['ps5'] = results
    run.dump_json('ps5_checks.json', results)
    return results


# ------------------------------------------------------------------- report
def stage_report(run, a):
    errs = [i for i in run.issues if i['severity'] == 'ERROR']
    warns = [i for i in run.issues if i['severity'] == 'WARN']
    summary = []
    summary.append('SwitchRecomp PS5 管线报告  run=%s  %s' % (run.run_id, now()))
    summary.append('工作目录: %s' % run.dir)
    summary.append('仓库: %s' % run.root)
    summary.append('')
    summary.append('== 阶段状态 ==')
    for st in STAGES:
        r = run.stage_result.get(st, {})
        summary.append('  %-8s %-6s %s' % (st, r.get('status', 'SKIP'), r.get('note', '')))
    summary.append('')
    summary.append('== 关键指标 ==')
    inv = run.data.get('inventory', [])
    for rec in inv:
        summary.append('  dump %-55s %6.2f GiB  %d packages' % (
            os.path.basename(rec['path']), rec['size'] / (1 << 30), len(rec.get('packages', []))))
    for rec in run.data.get('probe', []):
        for dr in rec.get('decode', {}).get('modules', []):
            summary.append('  解码覆盖 %-22s %-8s %9d words  真未知 %7d (%d 编码)  零填充 %7d  有效覆盖率 %.4f%%' % (
                rec['game'] + '/p%d' % rec['package_index'], dr['module'], dr['words_scanned'],
                dr['real_unknown_words'], dr['real_unknown_distinct'], dr['zero_fill_words'],
                dr['real_coverage_pct']))
    gaps = run.data.get('opcode_gaps')
    if gaps:
        summary.append('  指令缺口 %d 种编码 / %d 类: %s' % (
            gaps['distinct_encodings'], len(gaps['by_mnemonic']),
            ', '.join('%s x%d' % (k, v) for k, v in list(gaps['by_mnemonic'].items())[:12])))
    for rr in run.data.get('recomp', []):
        summary.append('  代码生成 %-22s rc=%-3d %4d C files  %8.1f MiB  %6.1fs' % (
            rr['title'], rr['rc'], rr['c_files'], rr['c_bytes'] / (1 << 20), rr['seconds']))
    for cr in run.data.get('compile', []):
        summary.append('  C 编译   %-22s %d/%d clean  %d error shapes' % (
            cr['title'], cr['ok_files'], cr['files_checked'], len(cr['error_groups'])))
    for pr in run.data.get('ps5', []):
        summary.append('  PS5 检查 %-22s rc=%-3d %5.1fs  %d issues' % (
            pr['label'], pr['rc'], pr['seconds'], len(pr.get('issues', []))))
    summary.append('')
    summary.append('== 问题清单 (%d ERROR / %d WARN) ==' % (len(errs), len(warns)))
    for i in errs + warns:
        summary.append('  [%-5s] %-8s %-24s %s' % (i['severity'], i['stage'], i['code'], i['detail']))
    if not errs and not warns:
        summary.append('  (无)')
    summary.append('')
    summary.append('== 产物 ==')
    for f in sorted(os.listdir(run.dir)):
        summary.append('  %s' % f)
    text = '\n'.join(summary) + '\n'
    with open(os.path.join(run.dir, 'summary.txt'), 'w', encoding='utf-8') as f:
        f.write(text)
    run.dump_json('report.json', dict(run_id=run.run_id, repo=run.root, dir=run.dir,
                                      stage_result=run.stage_result, env=run.data.get('env', {}),
                                      inventory=run.data.get('inventory', []),
                                      extracted=run.data.get('extracted', []),
                                      probe=run.data.get('probe', []),
                                      recomp=run.data.get('recomp', []),
                                      compile=run.data.get('compile', []),
                                      ps5=run.data.get('ps5', []),
                                      issues=run.issues, events=run.events))
    latest = os.path.join(run.work_root, 'runs', 'latest')
    try:
        if os.path.islink(latest) or os.path.isfile(latest):
            os.unlink(latest)
        os.symlink(run.run_id, latest)
    except OSError:
        pass
    print(text)
    return text


def main():
    ap = argparse.ArgumentParser(description='SwitchRecomp PS5 log-driven pipeline')
    ap.add_argument('--dumps', default=DEFAULT_DUMPS)
    ap.add_argument('--work-root', default=DEFAULT_WORK)
    ap.add_argument('--sdk', default=DEFAULT_SDK)
    ap.add_argument('--run-id', default=time.strftime('%Y%m%d-%H%M%S'))
    ap.add_argument('--stages', default='all', help='comma separated subset of %s, or all' % ','.join(STAGES))
    ap.add_argument('--games', default='', help='comma separated dump folder names (default: all)')
    ap.add_argument('--from-run', default='', help='reuse extracted.json / inventory.json from an earlier run id')
    ap.add_argument('--limit-words', type=int, default=0, help='max .text words per module (0=all)')
    ap.add_argument('--top-unknown', type=int, default=25)
    ap.add_argument('--chunk', type=int, default=30000)
    ap.add_argument('--recomp-timeout', type=int, default=3600)
    ap.add_argument('--compile-timeout', type=int, default=600)
    ap.add_argument('--max-compile-files', type=int, default=12, help='0 = all generated C files')
    ap.add_argument('--jobs', type=int, default=4)
    ap.add_argument('--cc', default='')
    ap.add_argument('--ps5-host', default='192.168.88.33')
    ap.add_argument('--ps5-port', type=int, default=2121)
    ap.add_argument('--ps5-llvm-config', default='/data/toolchains/llvm-23.1.4/bin/llvm-config')
    ap.add_argument('--ps5-boilerplate', default='/data/code/ps5-native-app-boilerplate')
    ap.add_argument('--ps5-opt', default='0', help='-O level for the PS5 cross build')
    ap.add_argument('--ps5-xcompile-files', type=int, default=40, help='generated C files per game (0=all)')
    ap.add_argument('--ps5-timeout', type=int, default=3600)
    ap.add_argument('--ps5-game-dir', default='', help='optional real --target ps5 CLI build on this game folder')
    ap.add_argument('--ps5-title', default='swr_ps5')
    ap.add_argument('--ps5-title-id', default='PPSA99201')
    ap.add_argument('--ps5-interp-only', action='store_true', help='fast CLI build without recompiling functions')
    a = ap.parse_args()
    a.games = [x for x in a.games.split(',') if x]
    stages = STAGES[:] if a.stages == 'all' else [x for x in a.stages.split(',') if x]
    for x in stages:
        if x not in STAGES:
            ap.error('unknown stage %s' % x)

    run = Run(a.work_root, a.run_id, ROOT)
    run.log('env', 'INFO', 'pipeline start: stages=%s games=%s' % (','.join(stages), ','.join(a.games) or 'all'))
    print('run dir: %s' % run.dir, flush=True)
    try:
        if 'env' in stages:
            t = time.time()
            try:
                stage_env(run, a)
                run.stage_result['env'] = dict(status='OK', note='%.1fs' % (time.time() - t))
            except Exception as e:
                run.issue('env', 'STAGE_FAILED', '%s: %s' % (type(e).__name__, e))
                run.log('env', 'ERROR', traceback.format_exc())
                run.stage_result['env'] = dict(status='FAIL', note=str(e))
        else:
            run.data['env'] = dict(dumps=[dict(path=p, size=os.path.getsize(p)) for p in
                                          [os.path.join(dp, f) for dp, _, fs in os.walk(a.dumps) for f in fs
                                           if f.lower().endswith('.nup')]])

        inv = []
        if 'inspect' in stages:
            t = time.time()
            try:
                inv = stage_inspect(run, a)
                run.stage_result['inspect'] = dict(status='OK', note='%d dumps' % len(inv))
            except Exception as e:
                run.issue('inspect', 'STAGE_FAILED', str(e))
                run.log('inspect', 'ERROR', traceback.format_exc())
                run.stage_result['inspect'] = dict(status='FAIL', note=str(e))

        extracted = []
        if 'extract' in stages:
            t = time.time()
            try:
                extracted = stage_extract(run, a, inv)
                run.stage_result['extract'] = dict(status='OK', note='%d exefs trees' % len(extracted))
            except Exception as e:
                run.issue('extract', 'STAGE_FAILED', str(e))
                run.log('extract', 'ERROR', traceback.format_exc())
                run.stage_result['extract'] = dict(status='FAIL', note=str(e))
        elif a.from_run:
            src = os.path.join(a.work_root, 'runs', a.from_run)
            try:
                with open(os.path.join(src, 'extracted.json'), encoding='utf-8') as f:
                    extracted = json.load(f)
                with open(os.path.join(src, 'inventory.json'), encoding='utf-8') as f:
                    inv = json.load(f)
                run.data['inventory'] = inv
                run.log('env', 'INFO', 'reused %d exefs trees from %s' % (len(extracted), src))
                run.stage_result['extract'] = dict(status='REUSED', note=src)
            except Exception as e:
                run.issue('env', 'REUSE_FAILED', '%s: %s' % (src, e))

        if 'probe' in stages:
            t = time.time()
            try:
                stage_probe(run, a, extracted)
                run.stage_result['probe'] = dict(status='OK', note='%.1fs' % (time.time() - t))
            except Exception as e:
                run.issue('probe', 'STAGE_FAILED', str(e))
                run.log('probe', 'ERROR', traceback.format_exc())
                run.stage_result['probe'] = dict(status='FAIL', note=str(e))

        recomp_results = []
        if 'recomp' in stages:
            t = time.time()
            try:
                recomp_results = stage_recomp(run, a, extracted)
                run.stage_result['recomp'] = dict(status='OK', note='%d games / %.1fs' % (len(recomp_results), time.time() - t))
            except Exception as e:
                run.issue('recomp', 'STAGE_FAILED', str(e))
                run.log('recomp', 'ERROR', traceback.format_exc())
                run.stage_result['recomp'] = dict(status='FAIL', note=str(e))
        elif a.from_run:
            src = os.path.join(a.work_root, 'runs', a.from_run)
            try:
                with open(os.path.join(src, 'report.json'), encoding='utf-8') as f:
                    recomp_results = json.load(f).get('recomp', [])
                run.data['recomp'] = recomp_results
                run.log('env', 'INFO', 'reused %d recomp results from %s' % (len(recomp_results), src))
                run.stage_result['recomp'] = dict(status='REUSED', note=src)
            except Exception as e:
                run.issue('env', 'RECOMP_REUSE_FAILED', '%s: %s' % (src, e))

        if 'compile' in stages:
            t = time.time()
            try:
                stage_compile(run, a, recomp_results)
                run.stage_result['compile'] = dict(status='OK', note='%.1fs' % (time.time() - t))
            except Exception as e:
                run.issue('compile', 'STAGE_FAILED', str(e))
                run.log('compile', 'ERROR', traceback.format_exc())
                run.stage_result['compile'] = dict(status='FAIL', note=str(e))

        if 'ps5' in stages:
            t = time.time()
            try:
                stage_ps5(run, a, recomp_results)
                run.stage_result['ps5'] = dict(status='OK', note='%.1fs' % (time.time() - t))
            except Exception as e:
                run.issue('ps5', 'STAGE_FAILED', str(e))
                run.log('ps5', 'ERROR', traceback.format_exc())
                run.stage_result['ps5'] = dict(status='FAIL', note=str(e))

        stage_report(run, a)
    finally:
        run.close()
    errs = sum(1 for i in run.issues if i['severity'] == 'ERROR')
    return 1 if errs else 0


if __name__ == '__main__':
    sys.exit(main())
