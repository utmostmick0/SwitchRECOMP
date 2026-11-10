#!/usr/bin/env python3
"""Swap a PS5 title's eboot.bin over FTP without ever losing the original.

The game-sized eboot (hundreds of MiB) stays on the console: it is renamed to
eboot.bin.game, and a small diagnostic eboot is uploaded under the original
name. --mode restore puts it back in one FTP rename. Everything is logged as
text + JSON so the state can be audited without touching the console again.

    python3 tools/ps5/ps5_deploy_eboot.py --mode probe  --local <probe.bin> --run-dir <dir>
    python3 tools/ps5/ps5_deploy_eboot.py --mode restore --run-dir <dir>
"""
import argparse, ftplib, hashlib, io, json, os, time

DEFAULT_HOST = '192.168.88.33'
DEFAULT_PORT = 2121
DEFAULT_DIR = '/mnt/usb1/PPSA99201'
GAME = 'eboot.bin.game'


class Run:
    def __init__(self, d):
        self.dir = os.path.abspath(d)
        os.makedirs(self.dir, exist_ok=True)
        self.fh = open(os.path.join(self.dir, 'deploy_eboot.log'), 'a', encoding='utf-8')
        self.data = dict(steps=[])
        self.issues = []

    def log(self, level, msg):
        line = '[%s] %-5s %s' % (time.strftime('%H:%M:%S'), level, msg)
        print(line, flush=True)
        self.fh.write(line + '\n')
        self.fh.flush()
        self.data['steps'].append(dict(level=level, msg=msg))

    def issue(self, code, detail):
        self.issues.append(dict(code=code, detail=detail))
        self.log('ERROR', '%s: %s' % (code, detail))

    def dump(self):
        with open(os.path.join(self.dir, 'deploy_eboot.json'), 'w', encoding='utf-8') as f:
            json.dump(dict(data=self.data, issues=self.issues), f, indent=2, ensure_ascii=False)
        lines = ['PS5 eboot deploy %s' % time.strftime('%Y-%m-%d %H:%M:%S'),
                 'mode   : %s' % self.data.get('mode'),
                 'remote : %s' % self.data.get('remote_dir'), '',
                 'before : %s' % self.data.get('before'),
                 'after  : %s' % self.data.get('after'),
                 'sha256 : %s' % self.data.get('sha256'), '',
                 'issues : %s' % (self.issues or 'none')]
        with open(os.path.join(self.dir, 'deploy_eboot_summary.txt'), 'w', encoding='utf-8') as f:
            f.write('\n'.join(lines) + '\n')


def sha256_bytes(b):
    return hashlib.sha256(b).hexdigest()


def listing(ftp):
    ftp.retrlines  # noqa: keep ftplib API explicit
    out = []
    for name, facts in ftp.mlsd():
        if name not in ('.', '..'):
            out.append(dict(name=name, size=int(facts.get('size', 0) or 0),
                            type=facts.get('type', 'file'), modify=facts.get('modify')))
    return out


def read_back(ftp, name):
    buf = io.BytesIO()
    ftp.retrbinary('RETR ' + name, buf.write)
    return buf.getvalue()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--host', default=DEFAULT_HOST)
    ap.add_argument('--port', type=int, default=DEFAULT_PORT)
    ap.add_argument('--remote-dir', default=DEFAULT_DIR)
    ap.add_argument('--local', default='')
    ap.add_argument('--mode', choices=['probe', 'replace', 'restore', 'status'], default='status')
    ap.add_argument('--run-dir', required=True)
    a = ap.parse_args()
    run = Run(a.run_dir)
    run.data.update(mode=a.mode, remote_dir=a.remote_dir, host=a.host, port=a.port)
    ftp = ftplib.FTP()
    try:
        ftp.connect(a.host, a.port, timeout=20)
        ftp.login()
        ftp.set_pasv(True)
        ftp.cwd(a.remote_dir)
        run.log('INFO', 'connected %s:%d cwd=%s' % (a.host, a.port, a.remote_dir))
        before = listing(ftp)
        run.data['before'] = [e for e in before if e['name'].startswith('eboot')]
        run.log('INFO', 'before: %s' % run.data['before'])

        if a.mode == 'status':
            run.dump()
            return 0

        if a.mode in ('probe', 'replace'):
            if not a.local or not os.path.isfile(a.local):
                run.issue('LOCAL_MISSING', a.local or '(none)')
                run.dump()
                return 1
            data = open(a.local, 'rb').read()
            want = sha256_bytes(data)
            run.data['local_size'] = len(data)
            run.data['sha256'] = want
            has_game = any(e['name'] == GAME for e in before)
            if a.mode == 'probe' and has_game:
                run.issue('ALREADY_SWAPPED', '%s exists; use --mode replace (or restore first)' % GAME)
                run.dump()
                return 1
            if not any(e['name'] == 'eboot.bin' for e in before):
                run.issue('NO_EBOOT', 'eboot.bin missing on console')
                run.dump()
                return 1
            if a.mode == 'probe':
                # Keep the original on the console: an FTP rename is instant.
                ftp.rename('eboot.bin', GAME)
                run.log('INFO', 'renamed eboot.bin -> %s' % GAME)
            else:
                # Original game eboot is already parked in GAME. The current
                # eboot.bin is a small diagnostic build; drop it and keep GAME.
                if not has_game:
                    ftp.rename('eboot.bin', GAME)
                    run.log('INFO', 'parked current eboot.bin as %s' % GAME)
                else:
                    ftp.delete('eboot.bin')
                    run.log('INFO', 'removed previous diagnostic eboot.bin (backup %s intact)' % GAME)
            ftp.storbinary('STOR eboot.bin', io.BytesIO(data))
            run.log('INFO', 'uploaded %d bytes as eboot.bin' % len(data))
            size = ftp.size('eboot.bin')
            back = read_back(ftp, 'eboot.bin')
            if size != len(data) or sha256_bytes(back) != want:
                run.issue('VERIFY_FAILED', 'size=%s sha=%s' % (size, sha256_bytes(back)))
                run.dump()
                return 1
            run.log('INFO', 'verified size + sha256 over the wire')

        else:  # restore
            if not any(e['name'] == GAME for e in before):
                run.issue('NO_BACKUP', '%s not found' % GAME)
                run.dump()
                return 1
            if any(e['name'] == 'eboot.bin' for e in before):
                ftp.delete('eboot.bin')
                run.log('INFO', 'removed probe eboot.bin')
            ftp.rename(GAME, 'eboot.bin')
            run.log('INFO', 'restored %s -> eboot.bin' % GAME)
            size = ftp.size('eboot.bin')
            run.log('INFO', 'restored size=%s' % size)

        run.data['after'] = [e for e in listing(ftp) if e['name'].startswith('eboot')]
        run.log('INFO', 'after: %s' % run.data['after'])
        run.dump()
        return 0
    except Exception as e:
        run.issue('FTP_FAILED', '%s: %s' % (type(e).__name__, e))
        run.dump()
        return 1
    finally:
        try:
            ftp.quit()
        except Exception:
            pass
        run.fh.close()


if __name__ == '__main__':
    raise SystemExit(main())
