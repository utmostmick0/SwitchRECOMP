#!/usr/bin/env python3
"""Replace a PS5 homebrew title's artwork over FTP, with backup and verification.

The console caches artwork in /user/appmeta/<title>, which is why simply
swapping the USB sce_sys files does not change what the menu shows. Both the
USB source and the appmeta cache are updated (when writable); every old file is
saved locally first and every upload is re-downloaded and sha256-compared.

    python3 tools/ps5/ps5_deploy_assets.py \
        --local-dir /data/switchrecomp/assets/ps5_sce_sys \
        --run-dir /data/switchrecomp/runs/<id>
"""
import argparse
import ftplib
import hashlib
import io
import json
import os
import time

FILES = ('icon0.png', 'pic0.dds', 'pic1.dds')
DIRS = ('/mnt/usb1/PPSA99201/sce_sys', '/user/appmeta/PPSA99201')


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--host', default='192.168.88.33')
    ap.add_argument('--port', type=int, default=2121)
    ap.add_argument('--local-dir', required=True)
    ap.add_argument('--param-json', default='',
                    help='optional param.json to also push into sce_sys (home-screen titleName)')
    ap.add_argument('--run-dir', required=True)
    ap.add_argument('--title', default='PPSA99201')
    a = ap.parse_args()
    os.makedirs(a.run_dir, exist_ok=True)
    backups = os.path.join(a.run_dir, 'backup')
    os.makedirs(backups, exist_ok=True)
    rep = dict(host=a.host, port=a.port, local_dir=a.local_dir,
               dirs=os.listdir(a.local_dir), steps=[], issues=[])
    log_path = os.path.join(a.run_dir, 'deploy_assets.log')

    def log(msg):
        line = '[%s] %s' % (time.strftime('%H:%M:%S'), msg)
        print(line, flush=True)
        with open(log_path, 'a', encoding='utf-8') as f:
            f.write(line + '\n')
        rep['steps'].append(msg)

    local = {}
    for f in FILES:
        p = os.path.join(a.local_dir, f)
        if not os.path.isfile(p):
            raise SystemExit('missing local asset: %s' % p)
        local[f] = open(p, 'rb').read()
    log('local assets: %s' % {f: (len(local[f]), sha(local[f])[:12]) for f in FILES})

    ftp = ftplib.FTP()
    ftp.connect(a.host, a.port, timeout=15)
    ftp.login()
    ftp.set_pasv(True)
    log('connected %s:%d' % (a.host, a.port))
    try:
        for d in DIRS:
            d = d.replace('PPSA99201', a.title)
            try:
                ftp.cwd(d)
            except Exception as ex:
                msg = '%s unavailable: %s' % (d, ex)
                rep['issues'].append(msg)
                log(msg)
                continue
            for f in FILES:
                old = io.BytesIO()
                try:
                    ftp.retrbinary('RETR ' + f, old.write)
                    with open(os.path.join(backups, d.strip('/').replace('/', '__') + '__' + f), 'wb') as fo:
                        fo.write(old.getvalue())
                    log('%s/%s backup %d bytes' % (d, f, len(old.getvalue())))
                except Exception as ex:
                    log('%s/%s no original to back up (%s)' % (d, f, type(ex).__name__))
                try:
                    ftp.storbinary('STOR ' + f, io.BytesIO(local[f]))
                except Exception as ex:
                    msg = '%s/%s STOR failed: %s' % (d, f, ex)
                    rep['issues'].append(msg)
                    log(msg)
                    continue
                got = io.BytesIO()
                ftp.retrbinary('RETR ' + f, got.write)
                ok = got.getvalue() == local[f]
                log('%s/%s uploaded %d bytes verify_sha256=%s' % (d, f, len(local[f]), 'OK' if ok else 'MISMATCH'))
                if not ok:
                    rep['issues'].append('%s/%s sha256 mismatch after upload' % (d, f))
        if a.param_json:
            with open(a.param_json, 'rb') as f:
                pdata = f.read()
            try:
                pj = json.loads(pdata.decode('utf-8'))
                names = {lang: loc.get('titleName') for lang, loc in
                         pj.get('localizedParameters', {}).items() if isinstance(loc, dict)}
            except Exception as ex:
                raise SystemExit('invalid --param-json: %s' % ex)
            log('param.json titleName: %s' % names)
            d = '/mnt/usb1/%s/sce_sys' % a.title
            ftp.cwd(d)
            old = io.BytesIO()
            try:
                ftp.retrbinary('RETR param.json', old.write)
                with open(os.path.join(backups, 'param.json.original'), 'wb') as fo:
                    fo.write(old.getvalue())
                log('%s/param.json backup %d bytes' % (d, len(old.getvalue())))
            except Exception as ex:
                log('%s/param.json no original to back up (%s)' % (d, type(ex).__name__))
            ftp.storbinary('STOR param.json', io.BytesIO(pdata))
            got = io.BytesIO()
            ftp.retrbinary('RETR param.json', got.write)
            ok = got.getvalue() == pdata
            log('%s/param.json uploaded %d bytes verify=%s' % (d, len(pdata), 'OK' if ok else 'MISMATCH'))
            if not ok:
                rep['issues'].append('%s/param.json sha256 mismatch after upload' % d)
    finally:
        try:
            ftp.quit()
        except Exception:
            pass
    with open(os.path.join(a.run_dir, 'deploy_assets.json'), 'w', encoding='utf-8') as f:
        json.dump(rep, f, indent=2, ensure_ascii=False)
    log('issues: %d -> %s' % (len(rep['issues']), rep['issues'] or 'none'))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
