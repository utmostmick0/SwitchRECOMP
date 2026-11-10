#!/usr/bin/env python3
# Verify a64.py instruction-table changes against the dump census and capstone.
#
# Two independent checks, both logged:
#   1. coverage  - every encoding recorded as an instruction gap must now decode,
#                  and we record which TABLE entry claimed it.
#   2. overmatch - random words matching each NEW pattern must disassemble (via
#                  capstone) to an instruction of the intended family. A pattern
#                  that swallows unrelated opcodes is a correctness bug, because
#                  decode() returns the FIRST matching entry in TABLE order.
import argparse, collections, json, os, random, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, ROOT)
import a64

NEW = ('LDAPR', 'LSE_ATOMIC', 'PAC_NOP', 'RETAA', 'RETAB')
FAMILY = {
    'LDAPR': ('ldaprb', 'ldaprh', 'ldapr'),
    'LSE_ATOMIC': ('ldadd', 'ldclr', 'ldeor', 'ldset', 'ldsmax', 'ldsmin', 'ldumax', 'ldumin', 'swp',
                   'stadd', 'stclr', 'steor', 'stset', 'stsmax', 'stsmin', 'stumax', 'stumin'),
    'PAC_NOP': ('pacia', 'pacib', 'autia', 'autib', 'paciza', 'pacizb',
                'autiza', 'autizb', 'xpaci', 'xpacd'),
    'RETAA': ('retaa',),
    'RETAB': ('retab',),
}


def is_new(name):
    return any(name.startswith(k) for k in NEW)


def family_of(name):
    for k in NEW:
        if name.startswith(k):
            return k
    return None


def sample_words(mask, value, count, rng):
    free = [b for b in range(32) if not (mask >> b) & 1]
    out = []
    for _ in range(count):
        w = value
        for b in free:
            if rng.getrandbits(1):
                w |= 1 << b
        out.append(w)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gaps', default='/data/switchrecomp/runs/latest/opcode_gaps.json')
    ap.add_argument('--out', default='', help='directory for opcode_verification.json')
    ap.add_argument('--samples', type=int, default=1500)
    ap.add_argument('--seed', type=int, default=20261108)
    a = ap.parse_args()
    rng = random.Random(a.seed)
    try:
        from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN
        md = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN)

        def mnemonic(w):
            ins = list(md.disasm(w.to_bytes(4, 'little'), 0x1000))
            return ins[0].mnemonic if ins else '<unrecognised>'
    except ImportError:
        md = None
        mnemonic = lambda w: '<no-capstone>'

    lines = []
    def say(s):
        lines.append(s)
        print(s, flush=True)

    say('a64.py table: %d entries (%d new)' % (len(a64.TABLE), sum(1 for t in a64.TABLE if is_new(t[0]))))

    # ---- check 1: census gaps now decode -----------------------------------
    resolved, remaining = 0, collections.Counter()
    claimed = collections.Counter()
    gaps_count = 0
    if os.path.isfile(a.gaps):
        with open(a.gaps, encoding='utf-8') as f:
            gaps = json.load(f)
        for e in gaps.get('encodings', []):
            gaps_count += 1
            w = int(e['word'], 16)
            i = a64.decode(w)
            if i >= 0:
                resolved += 1
                claimed[a64.TABLE[i][0]] += e['count']
            else:
                remaining[mnemonic(w)] += e['count']
        say('census: %d distinct gap encodings, %d now decode (%.2f%%), %d still missing' % (
            gaps_count, resolved, 100.0 * resolved / gaps_count if gaps_count else 0,
            gaps_count - resolved))
        for name, n in claimed.most_common():
            say('   claimed by %-14s %d word occurrences' % (name, n))
        if remaining:
            say('   remaining by mnemonic: %s' % ', '.join('%s x%d' % kv for kv in remaining.most_common(20)))
    else:
        say('census: %s not found, skipping coverage check' % a.gaps)

    # ---- check 2: new patterns do not overmatch ----------------------------
    over = []
    checked = 0
    if md is None:
        say('overmatch: SKIPPED (capstone not installed)')
    else:
        for idx, (name, m, v, spec, fn) in enumerate(a64.TABLE):
            fam = family_of(name)
            if not fam:
                continue
            allowed = FAMILY[fam]
            bad = collections.Counter()
            examples = {}
            for w in sample_words(m, v, a.samples, rng):
                di = a64.decode(w)
                if di < 0 or a64.TABLE[di][0] != name:
                    continue          # a more specific entry owns this encoding
                mn = mnemonic(w)
                checked += 1
                if not mn.startswith(allowed):
                    bad[mn] += 1
                    examples.setdefault(mn, '%08X' % w)
            if bad:
                over.append(dict(entry=name, index=idx, pattern_mask='%08X' % m,
                                 bad=dict(bad), examples=examples))
                say('  OVERMATCH %-14s %d/%d samples -> %s' % (
                    name, sum(bad.values()), a.samples,
                    ', '.join('%s x%d (eg %s)' % (k, c, examples[k]) for k, c in bad.most_common(5))))
            else:
                say('  ok        %-14s %d samples all in %s' % (name, a.samples, '/'.join(allowed)))
        say('overmatch: %d sampled words across %d new entries, %d entries with anomalies' % (
            checked, sum(1 for t in a64.TABLE if family_of(t[0])), len(over)))

    report = dict(table_entries=len(a64.TABLE),
                  census_encodings=gaps_count, census_resolved=resolved,
                  census_remaining=dict(remaining), claimed=dict(claimed),
                  sampled_words=checked, overmatches=over,
                  capstone=md is not None, log=lines)
    if a.out:
        os.makedirs(a.out, exist_ok=True)
        with open(os.path.join(a.out, 'opcode_verification.json'), 'w', encoding='utf-8') as f:
            json.dump(report, f, indent=2, ensure_ascii=False)
        with open(os.path.join(a.out, 'opcode_verification.log'), 'w', encoding='utf-8') as f:
            f.write('\n'.join(lines) + '\n')
    return 1 if over else 0


if __name__ == '__main__':
    sys.exit(main())
