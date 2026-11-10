#!/usr/bin/env python3
"""Cross-compile every runtime/generated C source with the PS5 toolchain in ONE pass.

The point is to collect *all* distinct portability problems at once (never fail
fast), normalise them into error shapes and write machine-readable logs. This is
the "scripts + logs, one conversion solves as many problems as possible" probe
for the --target ps5 work.

Usage:
  python3 tools/ps5/xcompile_check.py --run-dir /data/switchrecomp/runs/<id> \
      --repo /data/code/switchrecomp [--code-dir <dir> [--max-code-files 0]] \
      [--sdk <sdk>] [--llvm-config <path>]
"""
import argparse, collections, json, os, re, subprocess, sys, time
from concurrent.futures import ThreadPoolExecutor

RUNTIME_SRC = ['plat', 'mem', 'kernel', 'ipc', 'svc_core', 'audio', 'svc_hid', 'svc_fs', 'svc_vi',
               'nvdrv', 'gpu', 'gpu_engines', 'gpu_3d', 'loader', 'main', 'nup_ps5']
# build_runtime()'s PS5 set (no GL) + the always-present glue sources
# glibc_compat.c is Linux-only (defines fmod/fmodf); the PS5 build omits it.
EXTRA = ['gpu_nogl', 'shader_proc', 'host1x', 'snxvk_glue',
         'backend_ps5', 'entry', 'ps5_crt']

# zstd headers for runtime/nup_ps5.c, from the pacbrew payload SDK (same path
# ps5build.py uses).
ZSTD_INC = ('/data/code/EglantinePS5-build56/.deps/pacbrew-0.40.2/opt/'
            'ps5-payload-sdk/target/user/homebrew/include')


def normalise(line):
    x = re.sub(r'[^/\s]+\.(c|h|cpp)', '<file>', line)
    x = re.sub(r':\d+:\d+', ':<line>:<col>', x)
    x = re.sub(r'\b0x[0-9a-fA-F]+\b', '<hex>', x)
    x = re.sub(r'\b\d+\b', '<n>', x)
    return x.strip()


def build_env(a):
    env = dict(os.environ)
    env['LLVM_CONFIG'] = a.llvm_config
    env['PS5_PAYLOAD_SDK'] = a.sdk
    # the wrapper resolves lld/ar/nm through PATH
    env['PATH'] = os.path.join(a.sdk, 'bin') + os.pathsep + env.get('PATH', '')
    return env


def cc_cmd(a, src, syntax_only=True):
    """prospero-clang already injects -target/-isysroot/-isystem/-femulated-tls."""
    cc = os.path.join(a.sdk, 'bin', 'prospero-clang')
    cmd = [cc, '-std=gnu17', '-w', '-fno-strict-aliasing', '-fwrapv',
           '-fno-jump-tables', '-fPIC',
           '-I', os.path.join(a.repo, 'runtime'),
           '-I', os.path.join(a.repo, 'runtime', 'bcn'),
           '-I', os.path.join(a.repo, 'runtime', 'spvc'),
           '-I', os.path.join(a.repo, 'runtime', 'oh264', 'include'),
           '-I', os.path.join(a.repo, 'runtime', 'opus', 'include')]
    # zstd ships with the pacbrew payload SDK; ps5build.py adds the same path for
    # nup_ps5.c. Keep the sweep authoritative: without it that one file always
    # "fails" for a reason the real build does not have.
    if os.path.isdir(ZSTD_INC):
        cmd += ['-I', ZSTD_INC]
    cmd += ['-DSWR_NO_H264', '-DSWR_NO_OPUS', '-DSWR_PS5']
    cmd += ['-fsyntax-only'] if syntax_only else ['-c']
    cmd += [src]
    return cmd


def run_one(args):
    src, a, env, outdir = args
    cmd = cc_cmd(a, src)
    t0 = time.time()
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, errors='replace',
                           env=env, timeout=a.timeout)
        rc, out, err = r.returncode, r.stdout or '', r.stderr or ''
    except subprocess.TimeoutExpired:
        rc, out, err = -9, '', 'TIMEOUT'
    except Exception as e:
        rc, out, err = -1, '', '%s: %s' % (type(e).__name__, e)
    lines = [ln.strip() for ln in (err + out).splitlines()
             if 'error:' in ln or 'fatal error:' in ln or 'unsupported' in ln.lower()]
    if rc != 0 and not lines:
        lines = ['rc=%d %s' % (rc, (err or out)[:400].replace('\n', ' '))]
    return dict(src=os.path.relpath(src, a.repo), rc=rc, seconds=round(time.time() - t0, 2),
                errors=lines[:a.max_errors_per_file], total_error_lines=len(lines),
                cmd=' '.join(cmd))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--repo', default=os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
    ap.add_argument('--run-dir', required=True)
    ap.add_argument('--sdk', default='/data/code/EglantinePS5-build56/.deps/lapy-ps5-payload-sdk-v0.42')
    ap.add_argument('--llvm-config', default='/data/toolchains/llvm-23.1.4/bin/llvm-config')
    ap.add_argument('--code-dir', default='', help='generated C directory (Game_build)')
    ap.add_argument('--max-code-files', type=int, default=0, help='0 = all files')
    ap.add_argument('--jobs', type=int, default=4)
    ap.add_argument('--timeout', type=int, default=300)
    ap.add_argument('--max-errors-per-file', type=int, default=12)
    a = ap.parse_args()
    os.makedirs(a.run_dir, exist_ok=True)
    env = build_env(a)

    sources = [os.path.join(a.repo, 'runtime', s + '.c') for s in RUNTIME_SRC + EXTRA]
    missing = [p for p in sources if not os.path.isfile(p)]
    sources = [p for p in sources if os.path.isfile(p)]
    code_files = []
    if a.code_dir:
        allc = sorted(f for f in os.listdir(a.code_dir) if f.endswith('.c'))
        if a.max_code_files and len(allc) > a.max_code_files:
            step = len(allc) / float(a.max_code_files)
            picked = [allc[min(len(allc) - 1, int(i * step))] for i in range(a.max_code_files)]
            if 'module.c' not in picked and 'module.c' in allc:
                picked.append('module.c')
            allc = picked
        code_files = [os.path.join(a.code_dir, f) for f in allc]

    todo = [(p, a, env, a.run_dir) for p in sources + code_files]
    print('xcompile: %d runtime + %d generated sources, jobs=%d' % (len(sources), len(code_files), a.jobs), flush=True)
    results = []
    with ThreadPoolExecutor(max_workers=a.jobs) as pool:
        for i, res in enumerate(pool.map(run_one, todo), 1):
            results.append(res)
            if i % 25 == 0 or i == len(todo):
                print('  %d/%d done' % (i, len(todo)), flush=True)

    groups = collections.Counter()
    samples = collections.defaultdict(list)
    failed = []
    for r in results:
        if r['rc'] == 0:
            continue
        failed.append(r)
        for ln in r['errors']:
            key = normalise(ln)
            groups[key] += 1
            if len(samples[key]) < 5:
                samples[key].append(dict(src=r['src'], raw=ln))
    out = dict(sdk=a.sdk, llvm_config=a.llvm_config,
               toolchain=subprocess.run([os.path.join(a.sdk, 'bin', 'prospero-clang'), '--version'],
                                        capture_output=True, text=True, env=env).stdout.strip().splitlines()[:2],
               runtime_sources=len(sources), missing_runtime_sources=missing,
               generated_sources=len(code_files), total=len(results),
               ok=len(results) - len(failed), failed=len(failed),
               error_groups=[dict(pattern=k, count=c, samples=samples[k])
                             for k, c in groups.most_common()],
               per_file=[r for r in results if r['rc'] != 0])
    path = os.path.join(a.run_dir, 'xcompile_errors.json')
    with open(path, 'w', encoding='utf-8') as f:
        json.dump(out, f, indent=2, ensure_ascii=False)
    with open(os.path.join(a.run_dir, 'xcompile.log'), 'w', encoding='utf-8') as f:
        f.write('total=%d ok=%d failed=%d distinct_error_shapes=%d\n\n' % (
            out['total'], out['ok'], out['failed'], len(groups)))
        for g in out['error_groups']:
            f.write('[%4d] %s\n' % (g['count'], g['pattern']))
            for s in g['samples']:
                f.write('        %s: %s\n' % (s['src'], s['raw']))
            f.write('\n')
        f.write('\n== failed files ==\n')
        for r in out['per_file']:
            f.write('%s rc=%d %s\n' % (r['src'], r['rc'], ' | '.join(r['errors'][:3])))
    print('\ntotal=%d ok=%d failed=%d distinct_error_shapes=%d -> %s' % (
        out['total'], out['ok'], out['failed'], len(groups), path), flush=True)
    for g in out['error_groups'][:40]:
        print('  [%4d] %s' % (g['count'], g['pattern']), flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
