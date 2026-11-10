#!/usr/bin/env python3
"""Read SwitchRecomp evidence from a PS5 after one icon tap.

The console's Web File Manager (:8888) is the only transport; every directory
and file is optional so a crashed title still yields a usable report. Output is
a plain log, a JSON timeline and a short verdict summary in --run-dir.

    python3 tools/ps5/ps5_read_log.py --run-dir /data/switchrecomp/runs/<id>
"""
import argparse
import datetime
import json
import os
import urllib.parse
import urllib.request

DIRS = (
    '/data/switchrecomp', '/devlog/system/sce_coredumps.0',
    '/user/appmeta/PPSA99201', '/mnt/usb1/PPSA99201',
    '/devlog/system', '/data',
)
FILES = (
    '/data/switchrecomp/swr.log', '/data/switchrecomp/threads.txt',
    '/data/switchrecomp/va_probe.log',
    '/data/ps5_probe.log', '/data/switchrecomp/swr.ini',
)
VERDICT_KEYS = ('FATAL', 'window OK', 'window FAILED', 'segment ', 'reserve ',
                'commit selftest', 'decommit selftest', 'statically linked',
                'RomFS', 'npdm', 'error', 'failed', 'SWR_PS5_PROBE',
                'thread: ', 'hle: [S2]', 'badcall: ',
                'nup: pkg', 'aliases', 'NO-FILE', 'PADDING',
                'nup: read', 'nup: zstd', 'nup: alloc',
                'romfs: data', 'romfs: read', 'READ-FAIL',
                'NEVER WROTE OUTPUT', 'unresolved in',
                'kernel service census', 'LoadResult',
                'nvn lookup', 'nvn slot', 'nvn export', 'nvn bootstrap',
                'nvn call', 'nvn GetInteger', 'nvn WindowAcquireTexture',
                'NVN takeover', 'NV family', 'nv export', 'nvdrv call',
                'nn::Result', 'svcBreak', 'primitive_drawer',
                'without a new image', 'looping on SleepThread', 'past on SleepThread',
                'live guest state', 'live code around pc',
                'applet: queued message', 'applet: message', 'applet: IApplicationFunctions',
                'wait: thread', 'kernel event census', 'UNSIGNALED',
                'wait-cond:')


def opener():
    # Never let HTTP_PROXY hijack LAN requests to the console.
    return urllib.request.build_opener(urllib.request.ProxyHandler({}))


def stamp(value):
    if not isinstance(value, (int, float)):
        return str(value)
    return datetime.datetime.fromtimestamp(value).strftime('%Y-%m-%d %H:%M:%S')


def wfm_list(op, base, path):
    url = base + '/api/list?path=' + urllib.parse.quote(path)
    with op.open(url, timeout=8) as r:
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--host', default='192.168.88.33')
    ap.add_argument('--port', type=int, default=8888)
    ap.add_argument('--run-dir', required=True)
    a = ap.parse_args()
    os.makedirs(a.run_dir, exist_ok=True)
    base = 'http://%s:%d' % (a.host, a.port)
    op = opener()
    out = dict(host=a.host, base=base, dirs={}, files={}, issues=[])

    for d in DIRS:
        try:
            j = wfm_list(op, base, d)
            out['dirs'][d] = [dict(name=e.get('name'), size=e.get('size'),
                                   mtime=stamp(e.get('mtime')), type=e.get('type'))
                              for e in j.get('entries', [])]
        except Exception as ex:
            out['dirs'][d] = 'unavailable: %s: %s' % (type(ex).__name__, str(ex)[:120])
            out['issues'].append('%s: %s' % (d, out['dirs'][d]))

    text = ''
    va_text = ''
    for f in FILES:
        try:
            raw = wfm_download(op, base, f)
        except Exception as ex:
            out['files'][f] = 'unavailable: %s: %s' % (type(ex).__name__, str(ex)[:120])
            out['issues'].append('%s: %s' % (f, out['files'][f]))
            continue
        local = os.path.join(a.run_dir, f.strip('/').replace('/', '__'))
        with open(local, 'wb') as fo:
            fo.write(raw)
        out['files'][f] = dict(local=local, size=len(raw))
        if f.endswith('swr.log'):
            text = raw.decode('utf-8', 'replace')
        if f.endswith('va_probe.log'):
            va_text = raw.decode('utf-8', 'replace')

    with open(os.path.join(a.run_dir, 'timeline.json'), 'w', encoding='utf-8') as fo:
        json.dump(out, fo, indent=2, ensure_ascii=False)

    app = out['dirs'].get('/mnt/usb1/PPSA99201')
    eboot = None
    if isinstance(app, list):
        eboot = next((e for e in app if e.get('name') == 'eboot.bin'), None)
    lines = ['PS5 SwitchRecomp run %s' % datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S'),
             'host    : %s' % base,
             'eboot   : %s' % (eboot or 'not found'),
             'swr.log : %d bytes' % len(text.encode('utf-8', 'replace')),
             '']
    lines += ['--- swr.log ---'] + (text.splitlines() or ['(missing)'])
    if va_text:
        lines += ['', '--- va_probe.log (%d bytes) ---' % len(va_text.encode('utf-8', 'replace'))]
        lines += va_text.splitlines()
    lines += ['', '--- verdict lines ---']
    lines += [l for l in text.splitlines() if any(k.lower() in l.lower() for k in VERDICT_KEYS)] or ['(none)']
    if out['issues']:
        lines += ['', '--- issues ---'] + out['issues']
    summary = '\n'.join(lines) + '\n'
    with open(os.path.join(a.run_dir, 'summary.txt'), 'w', encoding='utf-8') as fo:
        fo.write(summary)
    print(summary, end='')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
