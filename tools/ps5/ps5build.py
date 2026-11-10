#!/usr/bin/env python3
"""PS5 native-title build driver for SwitchRecomp (--target ps5).

Turns the static archive produced by switchrecomp.py + the C runtime into a
single statically linked PIE, converts it to an OrbisOS ELF with the boilerplate
ps5-native-tool, signs it into eboot.bin and assembles a PPSAxxxxx folder.

Everything is log-driven: every command, its rc and its stderr/stdout go into
ps5build.log / ps5build_report.json. Stages never fail fast -- all problems are
collected into one report so one run fixes as many issues as possible.

Modes:
  --manifest FILE   full build from switchrecomp.py's ps5_build_manifest.json
  --smoke           toolchain-only smoke (tiny main + strong swr_module_static)
  --runtime-only    build libswr_runtime_ps5.a and exit
"""
import argparse, glob, json, os, re, shutil, subprocess, sys, time, traceback
from concurrent.futures import ThreadPoolExecutor

RUNTIME_SRC = ['plat', 'mem', 'kernel', 'ipc', 'svc_core', 'audio', 'svc_hid', 'svc_fs', 'svc_vi',
               'nvdrv', 'gpu', 'gpu_engines', 'gpu_3d', 'loader', 'main', 'nup_ps5']
# glibc_compat.c is Linux-only: it defines fmod/fmodf, which would become
# application exports in .dynsym and the FSELF converter rejects those.
EXTRA = ['gpu_nogl', 'shader_proc', 'host1x', 'snxvk_glue',
         'backend_ps5', 'entry', 'ps5_crt']
# --probe replaces entry.c (which owns main()) with a tiny main that writes a
# durable boot log and exits. It links the same CRT / stubs / converter / signer
# as the real game, so a passing probe isolates a crash to payload size/content.
EXTRA_PROBE = [n for n in EXTRA if n != 'entry']

PROBE_C = r'''/* Minimal PS5 boot probe. Proves _start -> init arrays -> main actually runs,
   and leaves a durable log even though stdout/stderr are not capturable. */
#include <stdio.h>
#include <stdlib.h>

const void* swr_module(void){ return (void*)0; }
const void* swr_module_static(void){ return (void*)0; }

static int g_argc;
static const char* g_argv0;

static void probe_log(const char* tag){
  static const char* paths[] = {
    "/data/ps5_probe.log", "/data/switchrecomp/ps5_probe.log",
    "/mnt/usb1/ps5_probe.log", "/mnt/usb0/ps5_probe.log", NULL
  };
  for(int i=0; paths[i]; ++i){
    FILE* f = fopen(paths[i], "a");
    if(!f) continue;
    fprintf(f, "%s argc=%d argv0=%s\n", tag, g_argc, g_argv0 ? g_argv0 : "(none)");
    fclose(f);
  }
}

int main(int argc, char** argv, char** envp){
  (void)envp;
  g_argc = argc;
  g_argv0 = (argc > 0 && argv && argv[0]) ? argv[0] : "(none)";
  probe_log("SWR_PS5_PROBE_OK");
  exit(0);
  return 0;
}
'''
DEFAULT_SDK = '/data/code/EglantinePS5-build56/.deps/lapy-ps5-payload-sdk-v0.42'
DEFAULT_LLVM = '/data/toolchains/llvm-23.1.4/bin/llvm-config'
DEFAULT_BOILER = '/data/code/ps5-native-app-boilerplate'

# zstd (used by the NUP romfs reader) from the PS5 payload SDK bundled with EglantinePS5.
ZSTD_INC = '/data/code/EglantinePS5-build56/.deps/pacbrew-0.40.2/opt/ps5-payload-sdk/target/user/homebrew/include'
ZSTD_LIB = '/data/code/EglantinePS5-build56/.deps/pacbrew-0.40.2/opt/ps5-payload-sdk/target/user/homebrew/lib/libzstd.a'
DEFAULT_SIGNER = '/data/switchrecomp/tools/native-tool/ps5-native-tool'

STAGES = ['env', 'runtime', 'link', 'convert', 'sign', 'package', 'verify']


class Run:
    def __init__(self, d):
        self.dir = d
        os.makedirs(d, exist_ok=True)
        self.fh = open(os.path.join(d, 'ps5build.log'), 'a', encoding='utf-8')
        self.issues = []
        self.stage = {}
        self.data = {}
        self.cmds = []

    def log(self, level, msg):
        line = '[%s] %-7s %s' % (time.strftime('%H:%M:%S'), level, msg)
        self.fh.write(line + '\n'); self.fh.flush()
        if level in ('ERROR', 'WARN', 'INFO'):
            print(line, flush=True)

    def issue(self, code, detail, severity='ERROR', **extra):
        rec = dict(code=code, detail=detail, severity=severity, **extra)
        self.issues.append(rec)
        self.log(severity, '%s: %s' % (code, detail))

    def cmd(self, argv, cwd=None, env=None, timeout=1800, label=''):
        t0 = time.time()
        self.log('INFO', '$ %s%s' % (label + ': ' if label else '', ' '.join(str(x) for x in argv)))
        try:
            r = subprocess.run([str(x) for x in argv], cwd=cwd, env=env, capture_output=True,
                               text=True, errors='replace', timeout=timeout)
            out = dict(rc=r.returncode, out=r.stdout or '', err=r.stderr or '')
        except subprocess.TimeoutExpired:
            out = dict(rc=-9, out='', err='TIMEOUT after %ss' % timeout)
        except Exception as e:
            out = dict(rc=-1, out='', err='%s: %s' % (type(e).__name__, e))
        out['seconds'] = round(time.time() - t0, 1)
        out['argv'] = [str(x) for x in argv]
        out['label'] = label
        self.cmds.append(out)
        if out['rc']:
            self.log('ERROR', '  rc=%d  %s' % (out['rc'], (out['err'] or out['out'])[-1500:].replace('\n', ' | ')))
        else:
            self.log('INFO', '  rc=0 in %.1fs' % out['seconds'])
        return out

    def close(self):
        self.fh.close()


def build_env(sdk, llvm):
    e = dict(os.environ)
    e['LLVM_CONFIG'] = llvm
    e['PS5_PAYLOAD_SDK'] = sdk
    e['PATH'] = os.path.join(sdk, 'bin') + os.pathsep + e.get('PATH', '')
    return e


def normalise(line):
    x = re.sub(r'[^/\s]+\.(c|h|cpp)', '<file>', line)
    x = re.sub(r':\d+:\d+', ':<line>:<col>', x)
    x = re.sub(r'\b0x[0-9a-fA-F]+\b', '<hex>', x)
    x = re.sub(r'\b\d+\b', '<n>', x)
    return x.strip()


def rebuild_signer(run, boiler, env):
    """Rebuild ps5-native-tool with the HOST g++ (the shipped prebuilt needs a
    newer libstdc++ than this machine has). Never touches the boilerplate tree."""
    out = DEFAULT_SIGNER
    if os.path.isfile(out) and os.access(out, os.X_OK):
        run.log('INFO', 'signer reused: %s' % out)
        return out
    src = os.path.join(boiler, 'tooling', 'native')
    zroot = os.path.join(boiler, '.deps', 'native', 'zlib', 'root', 'usr')
    zlib = os.path.join(zroot, 'lib', 'libz.a')
    if not os.path.isfile(zlib):
        run.issue('SIGNER_ZLIB_MISSING', zlib)
        return None
    os.makedirs(os.path.dirname(out), exist_ok=True)
    objs = []
    for n in ('native_app_builder', 'self_container', 'elf_object', 'sce_module_writer'):
        o = os.path.join(os.path.dirname(out), n + '.o')
        r = run.cmd(['g++', '-std=c++20', '-O2', '-I', os.path.join(zroot, 'include'),
                     '-c', os.path.join(src, n + '.cpp'), '-o', o], env=env, label='signer-cc-' + n)
        if r['rc']:
            run.issue('SIGNER_COMPILE_FAILED', '%s: %s' % (n, (r['err'] or r['out'])[-500:]))
            return None
        objs.append(o)
    r = run.cmd(['g++'] + objs + [zlib, '-o', out], env=env, label='signer-link')
    if r['rc']:
        run.issue('SIGNER_LINK_FAILED', (r['err'] or r['out'])[-500:])
        return None
    return out


def stage_env(run, a, env):
    s = 'env'
    sdk = a.sdk
    tools = {}
    for c in ('prospero-clang', 'prospero-clang++', 'prospero-lld', 'prospero-ar', 'prospero-nm'):
        p = os.path.join(sdk, 'bin', c)
        tools[c] = p if os.path.isfile(p) else 'MISSING'
        if tools[c] == 'MISSING':
            run.issue('SDK_TOOL_MISSING', c)
    if not os.path.isfile(a.llvm_config):
        run.issue('LLVM_CONFIG_MISSING', a.llvm_config)
    v = run.cmd([os.path.join(sdk, 'bin', 'prospero-clang'), '--version'], env=env, label='clang-version')
    ver = (v['out'] + v['err']).strip().splitlines()[:2]
    ld = os.path.join(a.boilerplate, 'tooling', 'native', 'ps5-pie.ld')
    vmap = os.path.join(a.boilerplate, 'tooling', 'native', 'app-symbols.map')
    for name, p in (('ps5-pie.ld', ld), ('app-symbols.map', vmap), ('libc.prx', os.path.join(a.boilerplate, 'runtime', 'libc.prx'))):
        if not os.path.isfile(p):
            run.issue('BOILERPLATE_ASSET_MISSING', '%s: %s' % (name, p))
    signer = rebuild_signer(run, a.boilerplate, env)
    stubs = sorted(glob.glob(os.path.join(sdk, 'target', 'lib', '*.so')))
    if not stubs:
        run.issue('NO_SDK_STUBS', 'no .so stubs under %s/target/lib' % sdk)
    run.data['env'] = dict(tools=tools, clang=ver, linker_script=ld, symbol_map=vmap,
                           signer=signer, stubs=len(stubs), llvm_config=a.llvm_config, sdk=sdk)
    run.log('INFO', 'sdk=%s llvm=%s stubs=%d signer=%s' % (sdk, a.llvm_config, len(stubs), signer))
    return run.data['env']


def compile_one(job):
    src, obj, sdk, repo, opt, env = job
    cmd = [os.path.join(sdk, 'bin', 'prospero-clang'), '-std=gnu17', '-O' + opt, '-w',
           '-fno-strict-aliasing', '-fwrapv', '-fPIC', '-fno-jump-tables',
           '-ffunction-sections', '-fdata-sections',
           '-DSWR_PS5', '-DSWR_NO_H264', '-DSWR_NO_OPUS',
           '-I', os.path.join(repo, 'runtime'),
           '-I', os.path.join(repo, 'runtime', 'bcn'),
           '-I', os.path.join(repo, 'runtime', 'spvc'),
           '-I', os.path.join(repo, 'runtime', 'oh264', 'include'),
           '-I', os.path.join(repo, 'runtime', 'opus', 'include'),
           '-I', ZSTD_INC,
           '-c', src, '-o', obj]
    t0 = time.time()
    try:
        r = subprocess.run(cmd, env=env, capture_output=True, text=True, errors='replace', timeout=900)
        rc, err = r.returncode, (r.stderr or r.stdout or '')
    except subprocess.TimeoutExpired:
        rc, err = -9, 'TIMEOUT'
    except Exception as e:
        rc, err = -1, '%s: %s' % (type(e).__name__, e)
    return dict(src=os.path.relpath(src, repo), obj=obj, rc=rc, seconds=round(time.time() - t0, 2),
                cmd=cmd, errors=[ln.strip() for ln in err.splitlines()
                                 if 'error:' in ln or 'fatal error:' in ln][:12],
                tail=err[-600:])


def stage_runtime(run, a, env, objdir):
    s = 'runtime'
    os.makedirs(objdir, exist_ok=True)
    extras = EXTRA_PROBE if getattr(a, 'probe', False) else EXTRA
    srcs = [os.path.join(a.repo, 'runtime', n + '.c') for n in RUNTIME_SRC + extras]
    missing = [p for p in srcs if not os.path.isfile(p)]
    for p in missing:
        run.issue('RUNTIME_SRC_MISSING', p)
    srcs = [p for p in srcs if os.path.isfile(p)]
    jobs = [(p, os.path.join(objdir, os.path.basename(p).replace('.c', '.o')), a.sdk, a.repo, a.opt, env)
            for p in srcs]
    results = []
    with ThreadPoolExecutor(max_workers=a.jobs) as pool:
        for r in pool.map(compile_one, jobs):
            results.append(r)
            if r['rc']:
                run.issue('RUNTIME_COMPILE_ERROR', '%s rc=%d: %s' % (
                    r['src'], r['rc'], ' | '.join(r['errors'][:2]) or r['tail'][-300:]))
    groups = {}
    for r in results:
        for ln in r['errors']:
            groups[normalise(ln)] = groups.get(normalise(ln), 0) + 1
    ok = [r['obj'] for r in results if r['rc'] == 0]
    run.data['runtime'] = dict(sources=len(srcs), ok=len(ok), failed=len(srcs) - len(ok),
                               error_groups=sorted(groups.items(), key=lambda x: -x[1]),
                               per_file=[r for r in results if r['rc']])
    run.log('INFO', 'runtime: %d/%d objects compiled, %d error shapes' % (len(ok), len(srcs), len(groups)))
    if not ok:
        run.issue('RUNTIME_NO_OBJECTS', 'no runtime object compiled', severity='ERROR')
        return []
    ar = os.path.join(a.sdk, 'bin', 'prospero-ar')
    arc = os.path.join(objdir, 'libswr_runtime_ps5.a')
    if os.path.isfile(arc): os.remove(arc)
    r = run.cmd([ar, 'rcs', arc] + ok, env=env, label='runtime-ar')
    if r['rc']:
        run.issue('RUNTIME_AR_FAILED', (r['err'] or r['out'])[-500:])
    return ok


def link_pie(run, a, env, envinfo, rt_objs, extra_objs, archive, outpath):
    s = 'link'
    lld = os.path.join(a.sdk, 'bin', 'prospero-lld')
    stubs = sorted(glob.glob(os.path.join(a.sdk, 'target', 'lib', '*.so')))
    cmd = [lld, '-T', envinfo['linker_script'], '--eh-frame-hdr',
           '--version-script', envinfo['symbol_map'], '-e', '_start', '-o', outpath,
           '--exclude-libs', 'ALL', ZSTD_LIB]
    cmd += ['-u', 'swr_module_static']   # force the static hook even if archive-only
    cmd += list(extra_objs) + list(rt_objs)
    if archive:
        cmd += ['--start-group', archive, '--end-group']
    cmd += ['--as-needed'] + stubs
    r = run.cmd(cmd, env=env, label='link-pie')
    if r['rc']:
        # Retry with the game objects passed directly (no archive extraction).
        rsp = a.objects_rsp
        if rsp and os.path.isfile(rsp):
            run.log('WARN', 'retrying link with direct object list from %s' % rsp)
            cmd2 = [lld, '-T', envinfo['linker_script'], '--eh-frame-hdr',
                    '--version-script', envinfo['symbol_map'], '-e', '_start', '-o', outpath]
            # objects.rsp already contains every game object (including
            # module.o), so extra_objs must not be repeated here. Its entries
            # are relative to the build dir -> run lld with that cwd.
            cmd2 += ['--exclude-libs', 'ALL', ZSTD_LIB] + list(rt_objs) + ['@' + os.path.basename(rsp), '--as-needed'] + stubs
            r = run.cmd(cmd2, env=env, cwd=os.path.dirname(rsp), label='link-pie-direct')
    if r['rc']:
        run.issue('LINK_FAILED', (r['err'] or r['out'])[-2000:])
        return None
    if not os.path.isfile(outpath):
        run.issue('LINK_NO_OUTPUT', outpath)
        return None
    run.data['link'] = dict(elf=outpath, size=os.path.getsize(outpath),
                            runtime_objects=len(rt_objs), extra_objects=len(extra_objs),
                            archive=archive, stub_count=len(stubs))
    return outpath


def stage_convert_sign(run, a, env, envinfo, pie):
    signer = envinfo['signer']
    out = {}
    if not signer:
        run.issue('NO_SIGNER', 'ps5-native-tool unavailable')
        return None, None
    eboot_elf = os.path.join(run.dir, 'eboot.elf')
    r = run.cmd([signer, 'link', '--in', pie, '--out', eboot_elf,
                 '--stub-dir', os.path.join(a.sdk, 'target', 'lib'),
                 '--module-sdk', '0x02000009', '--companion-sdk', '0x08050001',
                 '--file-name', 'eboot.elf'], env=env, label='convert')
    if r['rc'] or not os.path.isfile(eboot_elf):
        run.issue('CONVERT_FAILED', (r['err'] or r['out'])[-1200:])
        return None, None
    out['converted_size'] = os.path.getsize(eboot_elf)
    appdir = a.appdir
    os.makedirs(appdir, exist_ok=True)
    fself = os.path.join(appdir, 'eboot.bin')
    r = run.cmd([signer, 'self', '--sign', '--in', eboot_elf, '--out', fself,
                 '--magic', '0x1D3D154F'], env=env, label='sign')
    if r['rc']:
        run.issue('SIGN_FAILED', (r['err'] or r['out'])[-1200:])
        return eboot_elf, None
    out['fself_size'] = os.path.getsize(fself)
    out['fself'] = fself
    run.data['convert'] = out
    return eboot_elf, fself


def stage_package(run, a, env, envinfo, fself):
    s = 'package'
    if not fself or not a.appdir:
        run.log('INFO', 'package skipped (no fself or no appdir)')
        return
    app = a.appdir
    os.makedirs(os.path.join(app, 'sce_sys'), exist_ok=True)
    os.makedirs(os.path.join(app, 'sce_module'), exist_ok=True)
    copied = []
    libc = os.path.join(a.boilerplate, 'runtime', 'libc.prx')
    if os.path.isfile(libc):
        shutil.copyfile(libc, os.path.join(app, 'sce_module', 'libc.prx')); copied.append('libc.prx')
    else:
        run.issue('LIBC_PRX_MISSING', libc)
    # param.json from the boilerplate template, re-stamped for this title
    tp = os.path.join(a.boilerplate, 'sce_sys', 'param.json')
    if os.path.isfile(tp):
        with open(tp, encoding='utf-8') as f: param = json.load(f)
        tid = a.ps5_title_id
        num = re.sub(r'[^0-9]', '', tid)[-5:] or '99201'
        param['titleId'] = tid
        param['conceptId'] = num
        param['contentId'] = 'UP9000-%s_00-SWITCHRECOMP001' % tid
        # A user-facing title may differ from the -o build name (which is used
        # for log files, ini and generated C). Prefer --display-title, else the
        # human-readable name already in the template, and only then the build
        # name -- the PS5 home screen must never show an internal "swr_*" name.
        enus = param.get('localizedParameters', {}).get('en-US', {})
        tmpl_name = enus.get('titleName', '') if isinstance(enus, dict) else ''
        display = (getattr(a, 'display_title', '') or tmpl_name
                   or a.title or 'SwitchRecomp')
        for lang, loc in param.get('localizedParameters', {}).items():
            if isinstance(loc, dict) and 'titleName' in loc:
                loc['titleName'] = display
        with open(os.path.join(app, 'sce_sys', 'param.json'), 'w', encoding='utf-8') as f:
            json.dump(param, f, indent=2)
        copied.append('param.json')
    for asset in ('icon0.png', 'pic0.dds', 'pic1.dds', 'snd0.at9'):
        p = os.path.join(a.boilerplate, 'sce_sys', asset)
        if os.path.isfile(p):
            shutil.copyfile(p, os.path.join(app, 'sce_sys', asset)); copied.append(asset)
    # game data expected by runtime/main.c under SWR_PS5: <app>/swr_data/
    if a.datadir and os.path.isdir(a.datadir):
        dst = os.path.join(app, 'swr_data')
        if os.path.abspath(dst) != os.path.abspath(a.datadir):
            if os.path.isdir(dst): shutil.rmtree(dst)
            shutil.copytree(a.datadir, dst, dirs_exist_ok=True)
        copied.append('swr_data/')
    else:
        run.issue('GAME_DATA_MISSING', a.datadir or '(none)', severity='WARN')
    run.data['package'] = dict(appdir=app, items=copied,
                               files=sorted(os.path.relpath(os.path.join(dp, f), app)
                                            for dp, _, fs in os.walk(app) for f in fs)[:200])


def stage_verify(run, a, env, envinfo, pie, fself):
    s = 'verify'
    v = {}
    if pie and os.path.isfile(pie):
        nm = os.path.join(a.sdk, 'bin', 'prospero-nm')
        r = run.cmd([nm, '--defined-only', pie], env=env, label='nm-defined')
        syms = r['out'] + r['err']
        v['swr_module_static_defined'] = bool(re.search(r'\b[TDtB]\s+swr_module_static\b', syms))
        v['swr_module_defined'] = bool(re.search(r'\b[TDtB]\s+swr_module\b', syms))
        v['_start_defined'] = bool(re.search(r'\b[Tt]\s+_start\b', syms))
        v['defined_symbol_count'] = len([l for l in syms.splitlines() if l.strip()])
        r2 = run.cmd([nm, '-u', pie], env=env, label='nm-undefined')
        undef = r2['out'] + r2['err']
        v['undefined_sample'] = [l.strip() for l in undef.splitlines() if l.strip()][:40]
        v['dlopen_undefined'] = bool(re.search(r'\bU\s+dlopen\b', undef))
        if not v['swr_module_static_defined']:
            run.issue('STATIC_HOOK_NOT_DEFINED', 'swr_module_static is not a strong defined symbol in the PIE')
        if v['dlopen_undefined']:
            run.issue('DLOPEN_STILL_UNDEFINED', 'dlopen unresolved; static link path not taken', severity='WARN')
    if fself and os.path.isfile(fself) and envinfo['signer']:
        r = run.cmd([envinfo['signer'], 'self', '--inspect', '--file', fself], env=env, label='self-inspect')
        v['inspect_rc'] = r['rc']
        v['inspect'] = (r['out'] + r['err'])[:2000]
        with open(fself, 'rb') as f: head = f.read(16)
        v['fself_magic'] = head[:8].hex()
        if r['rc']:
            run.issue('SELF_INSPECT_FAILED', (r['err'] or r['out'])[-600:], severity='WARN')
    run.data['verify'] = v
    run.log('INFO', 'verify: static_hook=%s swr_module=%s start=%s dlopen_undef=%s' % (
        v.get('swr_module_static_defined'), v.get('swr_module_defined'),
        v.get('_start_defined'), v.get('dlopen_undefined')))
    return v


def report(run, a):
    errs = [i for i in run.issues if i['severity'] == 'ERROR']
    warns = [i for i in run.issues if i['severity'] == 'WARN']
    lines = ['SwitchRecomp --target ps5 构建报告  %s' % time.strftime('%Y-%m-%d %H:%M:%S'),
             'run dir : %s' % run.dir, 'mode    : %s' % ('smoke' if a.smoke else 'runtime-only' if a.runtime_only else 'full'),
             'sdk     : %s' % a.sdk, '']
    lines.append('== 阶段 ==')
    for s in STAGES:
        lines.append('  %-9s %s' % (s, run.stage.get(s, 'SKIP')))
    lines.append('')
    lines.append('== 结果 ==')
    for k in ('env', 'runtime', 'link', 'convert', 'package', 'verify'):
        d = run.data.get(k)
        if not d: continue
        if k == 'runtime':
            lines.append('  runtime    : %d/%d objects, %d error shapes' % (d['ok'], d['sources'], len(d['error_groups'])))
        elif k == 'link':
            lines.append('  PIE        : %s (%.1f MiB)' % (d['elf'], d['size'] / (1 << 20)))
        elif k == 'convert':
            lines.append('  eboot.elf  : %d bytes' % d.get('converted_size', 0))
            lines.append('  eboot.bin  : %d bytes' % d.get('fself_size', 0))
        elif k == 'package':
            lines.append('  app        : %s (%d items)' % (d['appdir'], len(d['items'])))
        elif k == 'verify':
            lines.append('  symbols    : swr_module_static=%s swr_module=%s _start=%s dlopen_undef=%s' % (
                d.get('swr_module_static_defined'), d.get('swr_module_defined'),
                d.get('_start_defined'), d.get('dlopen_undefined')))
        elif k == 'env':
            lines.append('  clang      : %s' % ' / '.join(d['clang']))
    lines.append('')
    lines.append('== 问题 (%d ERROR / %d WARN) ==' % (len(errs), len(warns)))
    for i in errs + warns:
        lines.append('  [%-5s] %-26s %s' % (i['severity'], i['code'], i['detail'][:300]))
    if not errs and not warns:
        lines.append('  (无)')
    text = '\n'.join(lines) + '\n'
    with open(os.path.join(run.dir, 'ps5build_summary.txt'), 'w', encoding='utf-8') as f: f.write(text)
    with open(os.path.join(run.dir, 'ps5build_report.json'), 'w', encoding='utf-8') as f:
        json.dump(dict(issues=run.issues, stage=run.stage, data=run.data, commands=run.cmds),
                  f, indent=2, ensure_ascii=False, default=str)
    print(text, flush=True)
    return len(errs)


class Cfg:
    pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--manifest', default='')
    ap.add_argument('--repo', default=os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
    ap.add_argument('--sdk', default=DEFAULT_SDK)
    ap.add_argument('--llvm-config', default=DEFAULT_LLVM)
    ap.add_argument('--boilerplate', default=DEFAULT_BOILER)
    ap.add_argument('--outdir', default='')
    ap.add_argument('--run-dir', default='')
    ap.add_argument('--opt', default='2')
    ap.add_argument('--jobs', type=int, default=4)
    ap.add_argument('--backend', default='ps5')
    ap.add_argument('--title-id', default='PPSA99201')
    ap.add_argument('--smoke', action='store_true')
    ap.add_argument('--probe', action='store_true',
                    help='build a tiny boot-probe title (same CRT/link/sign chain, ~MB eboot)')
    ap.add_argument('--va-probe', action='store_true',
                    help='build a tiny VA-capability probe from tools/ps5/ps5_va_probe.c')
    ap.add_argument('--display-title', default='',
                    help='param.json localizedParameters titleName (defaults to the build -o name)')
    ap.add_argument('--runtime-only', action='store_true')
    a = ap.parse_args()

    cfg = dict(repo=os.path.abspath(a.repo), sdk=a.sdk, llvm_config=a.llvm_config,
               boilerplate=a.boilerplate, opt=a.opt, jobs=a.jobs, title='', ps5_title_id=a.title_id,
               display_title=a.display_title or '',
               appdir='', datadir='', archive='', module_obj='', objects_rsp='', smoke=a.smoke,
               probe=a.probe or a.va_probe, va_probe=a.va_probe,
               runtime_only=a.runtime_only, backend=a.backend)
    if a.manifest:
        with open(a.manifest, encoding='utf-8') as f: m = json.load(f)
        for k in ('repo', 'sdk', 'llvm_config', 'boilerplate', 'opt', 'jobs', 'backend', 'title',
                  'ps5_title_id', 'display_title', 'appdir', 'datadir', 'archive',
                  'module_obj', 'objects_rsp'):
            if k in m and m[k]: cfg[k] = m[k]
        cfg['build'] = m.get('build', os.path.dirname(a.manifest))
    else:
        cfg['build'] = os.path.abspath(a.outdir or os.path.join('/data/switchrecomp', 'ps5build'))
    cfg['run_dir'] = a.run_dir or os.path.join(cfg['build'], 'ps5')
    if not cfg['appdir']:
        cfg['appdir'] = os.path.join(cfg['run_dir'], 'app', cfg['ps5_title_id'])
    c = Cfg()
    for k, v in cfg.items(): setattr(c, k, v)
    run = Run(c.run_dir)
    env = build_env(c.sdk, c.llvm_config)
    try:
        run.log('INFO', 'ps5build start mode=%s repo=%s' % (
            'probe' if c.probe else 'smoke' if c.smoke
            else 'runtime-only' if c.runtime_only else 'full', c.repo))
        envinfo = stage_env(run, c, env); run.stage['env'] = 'OK'
        objdir = os.path.join(c.run_dir, 'objects'); os.makedirs(objdir, exist_ok=True)
        rt_objs = stage_runtime(run, c, env, objdir)
        run.stage['runtime'] = '%d objects' % len(rt_objs)
        if c.runtime_only:
            run.log('INFO', 'runtime-only: %s' % os.path.join(objdir, 'libswr_runtime_ps5.a'))
            report(run, c); return 0
        if not rt_objs:
            run.stage['link'] = 'SKIP (no runtime objects)'
            report(run, c); return 1
        pie = os.path.join(c.run_dir, 'llvm-pie.elf')
        if c.probe:
            probe = os.path.join(c.run_dir, 'probe.c')
            if c.va_probe:
                va_src = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                      'ps5_va_probe.c')
                if not os.path.isfile(va_src):
                    run.issue('VA_PROBE_SRC_MISSING', va_src)
                    report(run, c); return 1
                with open(va_src, encoding='utf-8') as sf:
                    probe_c = sf.read()
            else:
                probe_c = PROBE_C
            with open(probe, 'w') as f:
                f.write(probe_c)
            r = compile_one((probe, os.path.join(objdir, 'probe.o'), c.sdk, c.repo, c.opt, env))
            if r['rc']:
                run.issue('PROBE_COMPILE_FAILED', r['tail'])
                report(run, c); return 1
            extra = [os.path.join(objdir, 'probe.o')]
            arc = None
        elif c.smoke:
            smoke = os.path.join(c.run_dir, 'smoke.c')
            with open(smoke, 'w') as f:
                # entry.c already provides main(); the smoke only needs the
                # strong static hook to prove the weak-symbol/PIE/FSELF chain.
                f.write('/* toolchain smoke: strong definitions of the static module hook */\n'
                        'const void* swr_module(void){ return (void*)0; }\n'
                        'const void* swr_module_static(void){ return (void*)0; }\n')
            r = compile_one((smoke, os.path.join(objdir, 'smoke.o'), c.sdk, c.repo, c.opt, env))
            if r['rc']:
                run.issue('SMOKE_COMPILE_FAILED', r['tail'])
                report(run, c); return 1
            extra = [os.path.join(objdir, 'smoke.o')]
            arc = None
        else:
            extra = [c.module_obj] if c.module_obj else []
            arc = c.archive
            if not os.path.isfile(arc or ''):
                run.issue('GAME_ARCHIVE_MISSING', arc or '(none)')
        pie = link_pie(run, c, env, envinfo, rt_objs, extra, arc, pie)
        run.stage['link'] = 'OK' if pie else 'FAIL'
        if not pie:
            report(run, c); return 1
        eboot_elf, fself = stage_convert_sign(run, c, env, envinfo, pie)
        run.stage['convert'] = 'OK' if eboot_elf else 'FAIL'
        run.stage['sign'] = 'OK' if fself else 'FAIL'
        if not c.smoke or c.probe:
            stage_package(run, c, env, envinfo, fself); run.stage['package'] = 'OK'
        stage_verify(run, c, env, envinfo, pie, fself); run.stage['verify'] = 'OK'
        rc = report(run, c)
        return 1 if rc else 0
    except Exception as e:
        run.issue('STAGE_EXCEPTION', '%s: %s' % (type(e).__name__, e))
        run.log('ERROR', traceback.format_exc())
        report(run, c)
        return 1
    finally:
        run.close()


if __name__ == '__main__':
    sys.exit(main())
