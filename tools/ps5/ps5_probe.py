#!/usr/bin/env python3
"""Probe a PS5 homebrew console and collect SwitchRecomp startup evidence.

Transport: PS5 Web File Manager on :8888 (preferred, /api/list + /api/download),
with plain FTP on :2121 as a fallback.  Never fail fast: every port, every
candidate directory and every reachable log is tried, everything is recorded in
a structured report, and downloaded log text is kept on disk so conclusions can
be re-checked without the console.

    python3 tools/ps5/ps5_probe.py --run-dir /data/switchrecomp/runs/<id>
"""
import argparse, ftplib, io, json, os, re, socket, time, urllib.parse, urllib.request

PORTS = (2121, 8888, 8084, 9295, 9999, 41800, 9081, 21, 80, 8080)
# Observed layout: the USB drive mounts at /mnt/usb1, homebrew titles installed
# internally live in /data/homebrew/PPSAxxxxx, Eglantine keeps klog-ish text in
# /data/eglantineps5/user/log, and /app0 only exists while a title is running.
WFM_DIRS = (
    '/', '/mnt/usb1', '/mnt/usb1/PPSA99201', '/mnt/usb1/PPSA99201/swr_data',
    '/mnt/usb1/eglantine_logs', '/mnt/usb0', '/mnt/usb0/eglantine_logs',
    '/data', '/data/homebrew', '/data/homebrew/PPSA99201',
    '/data/eglantineps5/user/log', '/devlog', '/user', '/user/log',
    '/app0', '/app0/swr_data',
    # crash forensics: appmeta proves the title was launched, coredumps.0 and
    # shadowmount debug logs record what happened immediately afterwards.
    '/data/shadowmount', '/devlog/system', '/devlog/system/sce_coredumps.0',
    '/devlog/system/sce_logs', '/devlog/app', '/user/temp', '/user/data',
    '/user/appmeta', '/user/appmeta/PPSA99201', '/data/cheatrunner',
    '/data/eglantineps5/logs', '/data/blackbox-v2',
    # SwitchRecomp's logf_ writes <g_exedir><g_exename>.log; on PS5 that is
    # /data/switchrecomp/swr.log (see runtime/plat.c:24 and main.c PS5 branch).
    '/data/switchrecomp',
)
FTP_DIRS = ('/', '/data', '/data/homebrew', '/data/homebrew/PPSA99201',
            '/data/eglantineps5/user/log', '/data/switchrecomp', '/mnt/usb1/PPSA99201')
LOG_EXT = ('.log', '.txt', '.out', '.json', '.ini', '.1', '.dat')
KEYS = ('SwitchRecomp', 'statically linked module', 'no dlopen', 'dlopen', 'RomFS',
        'PPSA99201', 'swr_bananza', 'fatal', 'error', 'warn', 'code:', 'eboot',
        'crash', 'coredump', 'panic', 'exception', 'abort', 'signal', 'launch',
        'mount', '99201', 'swr_', 'app0', 'sce_err', 'SWR_PS5_PROBE')
# Lines matching these are the ones that actually explain a launch failure;
# generic RomFS indexing chatter from Eglantine must not drown them out.
CRITICAL = ('99201', 'switchrecomp', 'swr_', 'statically linked', 'no dlopen',
            'crash', 'coredump', 'panic', 'exception', 'abort', 'fatal',
            'app0', 'launch', 'shadowmount', 'core dump', 'swr_ps5_probe')
MAX_FILE = 8 << 20
MAX_TOTAL = 96 << 20
MAX_HITS = 6000
# Eglantine's old RomFS indexing chatter fills the hit buffer otherwise.
NOISE = ('romfs indexing:', 'finalizing image structure')
APP_EBOOT = '/mnt/usb1/PPSA99201/eboot.bin'
APP_EBOOT_SIZE = 571874598


class Run:
    def __init__(self, d):
        self.dir = os.path.abspath(d)
        self.logs = os.path.join(self.dir, 'console_logs')
        os.makedirs(self.logs, exist_ok=True)
        self.f = open(os.path.join(self.dir, 'ps5_probe.log'), 'a', encoding='utf-8')
        self.issues = []
        self.data = dict(ports={}, dirs={}, downloads=[], hits=[], critical=[])
        self.total = 0

    def log(self, stage, msg):
        line = '%s [%s] %s' % (time.strftime('%H:%M:%S'), stage, msg)
        print(line, flush=True)
        self.f.write(line + '\n')
        self.f.flush()

    def issue(self, code, detail, severity='WARN'):
        self.issues.append(dict(code=code, detail=detail, severity=severity))
        self.log(severity, '%s: %s' % (code, detail))

    def dump(self):
        with open(os.path.join(self.dir, 'ps5_probe.json'), 'w', encoding='utf-8') as fo:
            json.dump(dict(data=self.data, issues=self.issues), fo, indent=2, ensure_ascii=False)


def opener():
    # Never let an HTTP_PROXY hijack LAN requests to the console.
    return urllib.request.build_opener(urllib.request.ProxyHandler({}))


def port_open(host, port):
    s = socket.socket()
    s.settimeout(1.2)
    try:
        s.connect((host, port))
        try:
            s.settimeout(0.8)
            b = s.recv(128)
        except Exception:
            b = b''
        return True, b.decode('utf-8', 'replace').strip()[:100]
    except Exception:
        return False, ''
    finally:
        s.close()


def scan_text(run, name, text):
    for i, line in enumerate(text.splitlines(), 1):
        low = line.lower()
        if any(n in low for n in NOISE):
            continue
        if not any(k.lower() in low for k in KEYS):
            continue
        rec = dict(file=name, line=i, text=line[:400])
        if len(run.data['hits']) < MAX_HITS:
            run.data['hits'].append(rec)
        if any(k in low for k in CRITICAL) and len(run.data['critical']) < 2000:
            run.data['critical'].append(rec)


def save_log(run, name, raw):
    rel = name.strip('/').replace('/', '__')
    local = os.path.join(run.logs, rel)
    with open(local, 'wb') as fo:
        fo.write(raw)
    txt = raw.decode('utf-8', 'replace')
    scan_text(run, name, txt)
    run.data['downloads'].append(dict(path=name, size=len(raw), local=local))
    run.log('download', '%s (%d bytes)' % (name, len(raw)))


def wfm_list(op, base, path):
    u = base + '/api/list?path=' + urllib.parse.quote(path)
    with op.open(u, timeout=8) as r:
        return json.load(r)


def wfm_download(op, base, path):
    data = urllib.parse.urlencode({'paths': path}).encode('utf-8')
    req = urllib.request.Request(base + '/api/download/prepare', data=data)
    with op.open(req, timeout=15) as r:
        j = json.load(r)
    tid = j.get('task_id')
    if not tid:
        raise RuntimeError('no task_id in %s' % j)
    with op.open(base + '/api/download?id=' + urllib.parse.quote(str(tid)), timeout=60) as r:
        return r.read()


def probe_wfm(run, host, port):
    op = opener()
    base = 'http://%s:%d' % (host, port)
    run.log('wfm', 'base=%s' % base)
    for d in WFM_DIRS:
        try:
            j = wfm_list(op, base, d)
            es = j.get('entries', [])
            run.data['dirs'][d] = [dict(name=e.get('name'), path=e.get('path'),
                                        type=e.get('type'), size=e.get('size'),
                                        mtime=e.get('mtime')) for e in es]
            run.log('wfm', '%-38s %d entries' % (d, len(es)))
            for e in es:
                name = e.get('name') or ''
                if e.get('type') == 'd' or not name.lower().endswith(LOG_EXT):
                    continue
                size = int(e.get('size') or 0)
                full = e.get('path') or (d.rstrip('/') + '/' + name)
                if not (0 < size <= MAX_FILE) or run.total + size > MAX_TOTAL:
                    continue
                try:
                    raw = wfm_download(op, base, full)
                    run.total += len(raw)
                    save_log(run, full, raw)
                except Exception as ex:
                    run.issue('WFM_DOWNLOAD_FAILED', '%s: %s' % (full, ex))
        except Exception as ex:
            run.data['dirs'][d] = 'unavailable: %s: %s' % (type(ex).__name__, str(ex)[:120])
            run.log('wfm', '%-38s unavailable (%s)' % (d, type(ex).__name__))
    # Facts that do not depend on log text
    entries = {d: v for d, v in run.data['dirs'].items() if isinstance(v, list)}
    app = entries.get('/mnt/usb1/PPSA99201', [])
    eb = next((e for e in app if e.get('name') == 'eboot.bin'), None)
    app0 = run.data['dirs'].get('/app0')
    run.data['layout'] = dict(
        usb_app_present=bool(eb),
        usb_eboot_size=eb.get('size') if eb else None,
        usb_eboot_size_expected=APP_EBOOT_SIZE,
        swr_data_present=any(e.get('name') == 'swr_data' for e in app),
        app0_listable=isinstance(app0, list),
        internal_install=isinstance(entries.get('/data/homebrew/PPSA99201'), list),
        title_likely_running=isinstance(app0, list),
    )


def probe_ftp(run, host, port):
    ftp = ftplib.FTP()
    try:
        ftp.connect(host, port, timeout=10)
        ftp.login()
        ftp.set_pasv(True)
    except Exception as e:
        run.issue('FTP_LOGIN_FAILED', '%s:%d %s: %s' % (host, port, type(e).__name__, e))
        return
    run.log('ftp', 'connected %s:%d welcome=%s' % (host, port, ftp.welcome[:100]))
    try:
        for d in FTP_DIRS:
            try:
                ftp.cwd(d)
                names = list(ftp.mlsd())
            except Exception:
                run.data['dirs']['ftp:' + d] = 'unavailable'
                continue
            rec = [dict(name=n, type=f.get('type', 'file'), size=int(f.get('size', 0) or 0))
                   for n, f in names if n not in ('.', '..')]
            run.data['dirs']['ftp:' + d] = rec
            run.log('ftp', '%s -> %d entries' % (d, len(rec)))
            for e in rec:
                if e['type'] == 'file' and e['name'].lower().endswith(LOG_EXT) \
                        and 0 < e['size'] <= MAX_FILE and run.total + e['size'] <= MAX_TOTAL:
                    full = d.rstrip('/') + '/' + e['name']
                    buf = io.BytesIO()
                    try:
                        ftp.retrbinary('RETR ' + full, buf.write)
                        raw = buf.getvalue()
                        run.total += len(raw)
                        save_log(run, 'ftp:' + full, raw)
                    except Exception as ex:
                        run.issue('FTP_RETR_FAILED', '%s: %s' % (full, ex))
    finally:
        try:
            ftp.quit()
        except Exception:
            pass


def main():
    global APP_EBOOT_SIZE
    ap = argparse.ArgumentParser()
    ap.add_argument('--host', default='192.168.88.33')
    ap.add_argument('--ftp-port', type=int, default=2121)
    ap.add_argument('--http-port', type=int, default=8888)
    ap.add_argument('--expected-eboot-size', type=int, default=APP_EBOOT_SIZE,
                    help='expected eboot.bin byte count (change when a probe eboot is deployed)')
    ap.add_argument('--run-dir', required=True)
    a = ap.parse_args()
    APP_EBOOT_SIZE = a.expected_eboot_size
    run = Run(a.run_dir)
    run.log('main', 'PS5 probe start host=%s' % a.host)
    for p in PORTS:
        ok, banner = port_open(a.host, p)
        run.data['ports'][p] = dict(open=ok, banner=banner)
        run.log('port', '%d %s %s' % (p, 'OPEN' if ok else 'closed', banner))
    if run.data['ports'].get(a.http_port, {}).get('open'):
        try:
            probe_wfm(run, a.host, a.http_port)
        except Exception as e:
            run.issue('WFM_STAGE_FAILED', '%s: %s' % (type(e).__name__, e))
    else:
        run.issue('WFM_OFFLINE', 'port %d closed' % a.http_port, severity='ERROR')
    if run.data['ports'].get(a.ftp_port, {}).get('open'):
        try:
            probe_ftp(run, a.host, a.ftp_port)
        except Exception as e:
            run.issue('FTP_STAGE_FAILED', '%s: %s' % (type(e).__name__, e))
    hits = run.data['hits']
    critical = run.data['critical']
    blob = '\n'.join(h['text'] for h in hits).lower()
    lay = run.data.get('layout', {})
    dirs = run.data['dirs']
    core = dirs.get('/devlog/system/sce_coredumps.0')
    core_files = core if isinstance(core, list) else []
    rtlog = dirs.get('/data/switchrecomp')
    rtlog_files = rtlog if isinstance(rtlog, list) else []
    appmeta = dirs.get('/user/appmeta')
    appmeta_list = appmeta if isinstance(appmeta, list) else []
    registered = next((e for e in appmeta_list if 'PPSA99201' in str(e.get('name'))), None)
    verdict = dict(
        usb_app_present=lay.get('usb_app_present', False),
        usb_eboot_size_ok=lay.get('usb_eboot_size') == APP_EBOOT_SIZE,
        app0_listable=lay.get('app0_listable', False),
        title_likely_running=lay.get('title_likely_running', False),
        statically_linked=('statically linked module' in blob or 'no dlopen' in blob),
        romfs_seen=('romfs:' in blob or 'romfs,' in blob),
        switchrecomp_seen=('switchrecomp' in blob),
        probe_ok=('swr_ps5_probe_ok' in blob),
        title_seen=('ppsa99201' in blob or 'swr_bananza' in blob),
        fatal_seen=('fatal' in blob),
        hit_count=len(hits),
        critical_hits=len(critical),
        downloaded_logs=len(run.data['downloads']),
        title_registered=bool(registered),
        appmeta_mtime=registered.get('mtime') if registered else None,
        coredump_entries=len(core_files),
        coredump_files=[dict(name=e.get('name'), size=e.get('size'), mtime=e.get('mtime'))
                        for e in core_files[:20]],
        runtime_log_files=[dict(name=e.get('name'), size=e.get('size'), mtime=e.get('mtime'))
                           for e in rtlog_files[:20]],
    )
    run.data['verdict'] = verdict
    run.dump()
    lines = ['PS5 probe %s host=%s' % (time.strftime('%Y-%m-%d %H:%M:%S'), a.host),
             'open ports : %s' % (', '.join(str(p) for p, v in run.data['ports'].items() if v['open']) or 'none'),
             'logs saved : %d' % verdict['downloaded_logs'],
             'hits       : %d' % verdict['hit_count'], '',
             'USB app PPSA99201    : %s (eboot size ok: %s)' % (
                 verdict['usb_app_present'], verdict['usb_eboot_size_ok']),
             '/app0 mounted       : %s  (a title is running)' % verdict['app0_listable'],
             'static module log   : %s' % verdict['statically_linked'],
             'RomFS log           : %s' % verdict['romfs_seen'],
             'SwitchRecomp log    : %s' % verdict['switchrecomp_seen'],
             'boot probe OK       : %s' % verdict['probe_ok'],
             'runtime log files   : %s' % (verdict['runtime_log_files'] or 'none'),
             'PPSA99201 log       : %s' % verdict['title_seen'],
             'fatal in logs       : %s' % verdict['fatal_seen'], '',
             '--- crash forensics ---',
             'title appmeta registered : %s (mtime=%s)' % (
                 verdict['title_registered'], verdict['appmeta_mtime']),
             'coredump dir entries     : %d  %s' % (
                 verdict['coredump_entries'], verdict['coredump_files']),
             'critical keyword hits   : %d' % verdict['critical_hits'], '']
    for i in run.issues:
        lines.append('%-5s %-22s %s' % (i['severity'], i['code'], i['detail'][:180]))
    lines += ['', '--- critical hits (last 120) ---']
    lines += ['%s:%d %s' % (h['file'], h['line'], h['text'][:220]) for h in critical[-120:]]
    lines += ['', '--- other keyword hits (last 40) ---']
    lines += ['%s:%d %s' % (h['file'], h['line'], h['text'][:220]) for h in hits[-40:]]
    with open(os.path.join(run.dir, 'ps5_probe_summary.txt'), 'w', encoding='utf-8') as fo:
        fo.write('\n'.join(lines) + '\n')
    run.log('main', 'probe done -> %s' % os.path.join(run.dir, 'ps5_probe_summary.txt'))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
