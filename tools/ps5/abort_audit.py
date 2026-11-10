#!/usr/bin/env python3
"""Static audit of every nn::diag Abort branch in a SwitchRecomp codegen tree.

Usage:
    python3 tools/ps5/abort_audit.py <generated_c_dir> [out_dir]

Produces summary.txt + abort_audit.json.  The point is to collect *all* HLE
reachable abort sites in one pass instead of discovering them one crash / one
rebuild / one console boot at a time.

What it extracts:
  * all calls into the nn::diag abort entry points (AbortImpl / Abort(Result))
  * the branch conditions in the 24 lines preceding each call
  * categories: ipc_result, null_handle_field, zero_value, result_const,
    sdk_session (the ones a host HLE can actually influence)
  * every TIPC command constant written to a TLS header (type = 16 + cmd),
    which gives the complete GSV UserSession command set a game may issue
  * Result constant constructions (0x30000 / 0x20000 masks)
"""
import os, re, json, glob, collections, sys

ROOT = sys.argv[1] if len(sys.argv) > 1 else '.'
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, 'abort_audit')
os.makedirs(OUT, exist_ok=True)

fun_re = re.compile(r'^void (F_[0-9A-F]+)\(CPU\* RESTRICT c, u64 e\)\{')
abort_re = re.compile(r'F_(ABC3350|ABC5C90|ABC7EC0|ABC69C0)\(c,')
tipc_send_re = re.compile(r'F_A5ED408\(c,')
const_re = re.compile(r'c->x\[\d+\]=\(u64\)\(\(u32\)\((\d+)\)\);')
result_mask_re = re.compile(r'\|(196608|131072|99942400|0x30000|0x20000)')

records = []
tipc_cmds = collections.Counter()
result_consts = collections.Counter()
func_hits = collections.Counter()
files_scanned = lines_scanned = 0

for path in sorted(glob.glob(os.path.join(ROOT, 'code_*.c'))):
    files_scanned += 1
    with open(path, errors='replace') as f:
        lines = f.readlines()
    lines_scanned += len(lines)
    fn = None
    for i, line in enumerate(lines):
        m = fun_re.match(line)
        if m:
            fn = m.group(1)
        if abort_re.search(line):
            w = lines[max(0, i - 24):i + 2]
            txt = ''.join(w)
            cats = []
            if tipc_send_re.search(txt) or re.search(r'if\(\(u32\)c->x\[0\]!=0\)', txt):
                cats.append('ipc_result')
            if re.search(r'RD32\(\(c->x\[\d+\]\+4\)\)', txt) and re.search(r'==0', txt):
                cats.append('null_handle_field')
            if result_mask_re.search(txt):
                cats.append('result_const')
            if 'F_ABD16B0' in txt or 'F_ABCD4E0' in txt or 'F_ABCD5A0' in txt:
                cats.append('sdk_session')
            if re.search(r'RD32\(\(c->x\[\d+\]\)\)', txt) and re.search(r'==0', txt):
                cats.append('zero_value')
            if not cats:
                cats.append('other')
            conds = [x.strip() for x in w if 'goto L_' in x or 'if(' in x][-8:]
            records.append({'file': os.path.basename(path), 'line': i + 1,
                            'func': fn, 'abort': abort_re.search(line).group(0).split('(')[0],
                            'cats': cats, 'conds': conds})
            func_hits[fn] += 1
        m2 = const_re.search(line)
        if m2:
            n = int(m2.group(1))
            if 16 <= n <= 2000:
                w = ''.join(lines[max(0, i - 6):i + 7])
                if 'tpidrro' in w and 'WR64' in w:
                    tipc_cmds[n - 16] += 1
        if result_mask_re.search(line):
            result_consts[line.strip()] += 1

cat_count = collections.Counter()
for r in records:
    for c in r['cats']:
        cat_count[c] += 1

summary = ['SwitchRecomp Abort 分支全量静态审查', '=' * 60,
           'root=%s' % ROOT,
           'files=%d lines=%d abort_calls=%d distinct_funcs=%d' %
           (files_scanned, lines_scanned, len(records), len(func_hits)), '',
           '== 分类计数（一个调用可属多类） ==']
for c, n in cat_count.most_common():
    summary.append('  %-20s %d' % (c, n))
summary += ['', '== Abort 调用最多的函数 top 25 ==']
for fn, n in func_hits.most_common(25):
    summary.append('  %-16s %d' % (str(fn), n))
summary += ['', '== 所有 TIPC 命令（cmd = type - 16） ==',
            '  count=%d  cmd_list=%s' %
            (len(tipc_cmds), ','.join(str(n) for n in sorted(tipc_cmds)))]
for n in sorted(tipc_cmds):
    summary.append('    cmd=%-5d occurrences=%d' % (n, tipc_cmds[n]))
summary += ['', '== Result 常量构造点 ==']
for k, v in result_consts.most_common(40):
    summary.append('  x%d  %s' % (v, k[:150]))
hi = [r for r in records if any(c in r['cats'] for c in
      ('ipc_result', 'null_handle_field', 'result_const', 'sdk_session'))]
summary += ['', '== 高价值分支 ==', '  count=%d' % len(hi)]
for r in hi[:200]:
    summary.append('  %s:%d %s [%s] %s' %
                   (r['file'], r['line'], r['func'], ','.join(r['cats']),
                    ' | '.join(r['conds'][-4:])[:220]))

with open(os.path.join(OUT, 'summary.txt'), 'w') as f:
    f.write('\n'.join(summary) + '\n')
with open(os.path.join(OUT, 'abort_audit.json'), 'w') as f:
    json.dump({'records': records,
               'tipc_cmds': {str(k): v for k, v in tipc_cmds.items()},
               'result_consts': {str(k): v for k, v in result_consts.items()},
               'cat_count': dict(cat_count)}, f, indent=1)
print('\n'.join(summary[:45]))
print('WROTE', os.path.join(OUT, 'summary.txt'), os.path.join(OUT, 'abort_audit.json'))
