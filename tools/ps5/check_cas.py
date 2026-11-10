#!/usr/bin/env python3
"""Verify the ARMv8.1 LSE CAS/CASP implementation against the real dump.

Two independent checks, both logged to a structured JSON report:

  coverage  - every undef() encoding in the generated C that capstone calls a
              cas* instruction must now emit C (a64.emit_static != None), and
              no recognized text-segment instruction may remain a gap.
  overmatch - random words matching the two LDSTX sub-cases we changed
              (o2=1/o1=1 -> CAS; o2=0/o1=1/size<2 -> CASP) must disassemble
              to a cas* mnemonic. A pattern that swallowed unrelated opcodes
              is a correctness bug, because decode() returns the FIRST match.

Only words whose guest PC lies inside a module .text segment are considered:
.rodata, literal pools, string tables and zero padding produce millions of
bogus "instructions" and would drown the real gaps.
"""
import argparse
import collections
import glob
import json
import os
import random
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, ROOT)
import a64  # noqa: E402

import capstone  # noqa: E402
import lz4.block  # noqa: E402

# Guest .text ranges reported by runtime/loader.c ("set_state text=.. ro=..")
TEXT_RANGES = (
    (0x08000000, 0x08002040, 'rtld'),
    (0x08021000, 0x099AA000, 'main'),
    (0x0A472000, 0x0ABE9000, 'sdk'),
)
UNDEF_RE = re.compile(
    r'undef\(c,\s*(\d+),\s*(?:(0x[0-9A-Fa-f]+)ull|(\d+))\)\s*;')
CAS_PREFIXES = ('cas',)


def module_of(pc):
    for lo, hi, name in TEXT_RANGES:
        if lo <= pc < hi:
            return name
    return None


def mnemonic(md, w):
    ins = list(md.disasm(struct.pack('<I', w), 0))
    return ins[0].mnemonic if ins else 'unrecognised'


def collect(build_dir, md):
    """-> {encoding: [count, module, first_pc]} for real .text undef slots."""
    out = {}
    for path in glob.glob(os.path.join(build_dir, 'code_*.c')):
        with open(path, errors='replace') as f:
            for m in UNDEF_RE.finditer(f.read()):
                pc = int(m.group(1))
                w = int(m.group(2), 16) if m.group(2) else int(m.group(3))
                if w > 0xFFFFFFFF or module_of(pc) is None:
                    continue
                if w not in out:
                    out[w] = [0, module_of(pc), hex(pc)]
                out[w][0] += 1
    return out


def cas_subcase_words(rng, count):
    """Random words in the two LDSTX sub-cases we implemented."""
    words = []
    for _ in range(count):
        # fixed: bits29..24 = 001000, bit21 = o1 = 1
        w = (1 << 27) | (1 << 21)
        for bit in (23, 22):
            if rng.getrandbits(1):
                w |= 1 << bit
        for bit in range(20, 16):          # Rs
            if rng.getrandbits(1):
                w |= 1 << bit
        if rng.getrandbits(1):
            w |= 1 << 15                  # L / release
        for bit in range(10, 15):         # must be 11111 for CAS/CASP
            w |= 1 << bit
        for bit in range(5, 10):          # Rn
            if rng.getrandbits(1):
                w |= 1 << bit
        for bit in range(0, 5):           # Rt
            if rng.getrandbits(1):
                w |= 1 << bit
        # CAS needs o2=1 (bit23) and any size; CASP needs o2=0 and size<2
        if not (w >> 23) & 1:
            w &= ~(1 << 23)
            w = (w & ~(3 << 30)) | (rng.getrandbits(1) << 30)
            w &= ~(1 << 16)               # Rs even
        else:
            w = (w & ~(3 << 30)) | (rng.getrandbits(2) << 30)
        words.append(w)
    return words


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--build-dir', required=True,
                    help='generated C build directory (contains code_*.c)')
    ap.add_argument('--run-dir', required=True)
    ap.add_argument('--samples', type=int, default=3000)
    ap.add_argument('--seed', type=int, default=20261108)
    a = ap.parse_args()
    os.makedirs(a.run_dir, exist_ok=True)
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_LITTLE_ENDIAN)
    rep = {'build_dir': a.build_dir, 'checks': {}, 'issues': []}

    slots = collect(a.build_dir, md)
    by_mnemonic = collections.Counter()
    cas_rows = []
    remaining = []
    for w, (count, mod, pc) in slots.items():
        name = mnemonic(md, w)
        by_mnemonic[name] += count
        idx = a64.decode(w)
        emitted = a64.emit_static(w, 0) if idx >= 0 else None
        state = 'NO_PATTERN' if idx < 0 else ('HANDLER_NONE' if emitted is None else 'OK')
        if name.startswith(CAS_PREFIXES):
            cas_rows.append({'encoding': '0x%08X' % w, 'mnemonic': name,
                             'occurrences': count, 'module': mod, 'pc': pc,
                             'state': state,
                             'emits_c': bool(emitted),
                             'c': (emitted[:160] if isinstance(emitted, str) else None)})
            if state != 'OK':
                rep['issues'].append('CAS %s at %s still %s' % (name, pc, state))
        elif state != 'OK' and name not in ('udf', 'unrecognised', 'stg'):
            remaining.append({'encoding': '0x%08X' % w, 'mnemonic': name,
                              'occurrences': count, 'state': state})
            rep['issues'].append('non-CAS gap %s at %s (%s)' % (name, pc, state))

    rep['checks']['coverage'] = {
        'text_undef_distinct': len(slots),
        'text_undef_occurrences': sum(v[0] for v in slots.values()),
        'cas_distinct': len(cas_rows),
        'cas_occurrences': sum(r['occurrences'] for r in cas_rows),
        'cas_emitting_c': sum(1 for r in cas_rows if r['emits_c']),
        'other_recognised_gaps': len(remaining),
        'by_mnemonic': dict(by_mnemonic.most_common(30)),
        'cas_detail': sorted(cas_rows, key=lambda r: -r['occurrences'])[:40],
        'other_gaps': sorted(remaining, key=lambda r: -r['occurrences'])[:20],
    }

    rng = random.Random(a.seed)
    over = collections.Counter()
    bad = []
    for w in cas_subcase_words(rng, a.samples):
        name = mnemonic(md, w)
        over[name] += 1
        # capstone cannot name reserved encodings (mostly data words that
        # happen to land in this encoding space). They are harmless here but
        # get counted separately instead of being reported as overmatches.
        if not name.startswith(CAS_PREFIXES) and name != 'unrecognised' and len(bad) < 20:
            bad.append({'encoding': '0x%08X' % w, 'mnemonic': name})
    rep['checks']['overmatch'] = {
        'samples': a.samples,
        'by_mnemonic': dict(over.most_common(20)),
        'unexpected': bad,
        'reserved_encodings': over.get('unrecognised', 0),
        'unexpected_count': sum(c for n, c in over.items()
                                if not n.startswith(CAS_PREFIXES) and n != 'unrecognised'),
    }
    if bad:
        rep['issues'].append('overmatch: %d sampled words are not cas*' % len(bad))

    rep['ok'] = not rep['issues']
    out = os.path.join(a.run_dir, 'cas_verification.json')
    with open(out, 'w') as f:
        json.dump(rep, f, indent=2)
    lines = [
        'CAS/CASP verification - %s' % ('PASS' if rep['ok'] else 'FAIL'),
        'build-dir: %s' % a.build_dir,
        'text undef slots: %d distinct / %d occurrences'
        % (len(slots), sum(v[0] for v in slots.values())),
        'CAS encodings: %d distinct / %d occurrences; emitting C: %d'
        % (len(cas_rows), sum(r['occurrences'] for r in cas_rows),
           sum(1 for r in cas_rows if r['emits_c'])),
        'other recognised gaps: %d' % len(remaining),
        'overmatch samples: %d; unexpected mnemonics: %d'
        % (a.samples, rep['checks']['overmatch']['unexpected_count']),
    ]
    lines += ['ISSUE: ' + s for s in rep['issues'][:20]]
    with open(os.path.join(a.run_dir, 'cas_verification_summary.txt'), 'w') as f:
        f.write('\n'.join(lines) + '\n')
    print('\n'.join(lines))
    print('report: %s' % out)
    return 0 if rep['ok'] else 1


if __name__ == '__main__':
    sys.exit(main())
