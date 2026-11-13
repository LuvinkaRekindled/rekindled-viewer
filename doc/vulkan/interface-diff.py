#!/usr/bin/env python3
"""Interface comparison of the stock and the native side of every manifest program (stage 4, step 0 of DESIGN-stock-carrier-removal.md).

Input: the `InterfaceDiff` log lines of one run with LL_VK_INTERFACE_DIFF=1, LL_VK_UNICENSUS=1 (which uniforms the callers wrote) and LL_VK_NATIVE_BUILD_ALL=1 (every manifest line built):
    stock  | <program> | u name:type;name:type[n] | s name:type | a name@loc | o <outputs> | set name;name
    native | <program> | <family> | <vertex>,<fragment> | u ... | s ... | a ... | o <n>
    native-none | <program> | <family> | build failed | never built
Output: doc/vulkan/INTERFACE-DIFF.md. The question: when the stock program behind a native one is removed, which names do the callers lose?
  - variant keys: uniforms that a manifest line's `require` / `when` reads from the old program. Under a native-only program they are read from the program's own values, so the native program must declare them
    (or the choice moves to the C++ side). Flagged when the native program does not declare one;
  - dropped writes, specific: uniforms the callers wrote on the stock program that the native program does not declare, other than the ones the shader manager writes to (nearly) every program
    ("broadcast": the environment values). The write finds no slot and is ignored; today the copy by name ignores it too, so a specific drop is no change but each one is read once to see that the native shader
    does not need a value under another name;
  - type mismatches (same name, different type or array size), native only samplers, attribute locations that differ, a different number of fragment outputs (the G-buffer family is expected to differ: the stock
    program declares one array `frag_data[4]`, the native one four variables).
Usage: interface-diff.py [log text file [output file]]   (defaults: inventory-data/interface-diff.txt next to this script, INTERFACE-DIFF.md; the RT-on run: inventory-data/interface-diff-rt.txt INTERFACE-DIFF-RT.md)
"""
import collections
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..'))
SRC = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, 'inventory-data', 'interface-diff.txt')
MANIFEST = os.path.join(REPO, 'indra', 'newview', 'app_settings', 'shaders', 'native', 'manifest.txt')
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, 'INTERFACE-DIFF.md')
BROADCAST_SHARE = 0.10          # a name written on more than this share of the stock programs is an environment value the manager writes everywhere


def parse_list(text):
    out = {}
    for item in [x for x in text.split(';') if x]:
        name, _, typ = item.partition(':')
        out[name] = typ
    return out


def parse_attr(text):
    out = {}
    for item in [x for x in text.split(';') if x]:
        name, _, loc = item.partition('@')
        out[name] = loc
    return out


def fields(rest):
    d = {}
    for part in rest.split(' | '):
        key, _, val = part.partition(' ')
        d[key] = val
    return d


stock, native, none = {}, {}, {}
for line in open(SRC, encoding='utf-8', errors='replace'):
    m = re.search(r'#InterfaceDiff#[^:]*: (.*)$', line)
    body = m.group(1) if m else line.strip()
    if body.startswith('stock | '):
        p = body.split(' | ', 2)
        f = fields(p[2])
        stock.setdefault(p[1], []).append({'u': parse_list(f.get('u', '')), 's': parse_list(f.get('s', '')), 'a': parse_attr(f.get('a', '')), 'o': f.get('o', '0'), 'set': set(x for x in f.get('set', '').split(';') if x)})
    elif body.startswith('native | '):
        p = body.split(' | ', 4)
        f = fields(p[4])
        native.setdefault(p[1], []).append({'family': p[2], 'files': p[3], 'u': parse_list(f.get('u', '')), 's': parse_list(f.get('s', '')), 'a': parse_attr(f.get('a', '')), 'o': f.get('o', '0')})
    elif body.startswith('native-none | '):
        p = body.split(' | ')
        none[p[1]] = p[4] if len(p) > 4 else '?'

# the manifest conditions of each line, in the order the lines of one program appear
conds = collections.defaultdict(list)
for raw in open(MANIFEST, encoding='utf-8'):
    f = [x.strip() for x in raw.strip().split('|')]
    if len(f) < 5 or f[0] != 'override':
        continue
    keys = []
    for part in f[5:]:
        kind, _, rest = part.partition(' ')
        if kind == 'require':
            keys.append(rest.strip())
        elif kind == 'when':
            keys.append(rest.split('=')[0].strip())
    conds[f[1]].append(keys)

# names written on many programs: environment values the manager writes to all of them
nprog = sum(len(v) for v in stock.values())
written_on = collections.Counter()
for progs in stock.values():
    for s in progs:
        for n in s['set']:
            written_on[n] += 1
broadcast = {n for n, c in written_on.items() if c > BROADCAST_SHARE * nprog}

rows = []       # (label, family, variant key issues, specific drops, other issues, notes)
no_stock = []
for target in sorted(native):
    for k, n in enumerate(native[target]):
        label = target + (' [line %d: %s]' % (k + 1, n['files']) if len(native[target]) > 1 else '')
        if target not in stock:
            no_stock.append(label)
            continue
        su, ss, sa, so, sset = {}, {}, {}, set(), set()
        for s in stock[target]:
            su.update(s['u'])
            ss.update(s['s'])
            sa.update(s['a'])
            so.add(s['o'])
            sset |= s['set']
        keyissues, drops, other, notes = [], [], [], []
        for key in (conds[target][k] if k < len(conds[target]) else []):
            if key.startswith('ll_ti_'):
                if key[6:] not in n['s']:
                    keyissues.append('%s (a sampler: the native program has no sampler of that name)' % key)
            elif key not in n['u']:
                keyissues.append('%s (not declared in the native program)' % key)
        keys = set(k2 for k2 in (conds[target][k] if k < len(conds[target]) else []))
        drops = sorted(x for x in sset if x not in n['u'] and x not in n['s'] and not x.startswith('ll_ti_') and x not in broadcast and x not in keys)
        mism = sorted('%s (%s vs %s)' % (x, su[x], n['u'][x]) for x in n['u'] if x in su and su[x] != n['u'][x])
        if mism:
            other.append('type mismatch: ' + ', '.join(mism))
        ns_only = sorted(x for x in n['s'] if x not in ss)
        if ns_only:
            other.append('native only samplers: ' + ', '.join(ns_only))
        loc_diff = sorted('%s (%s vs %s)' % (x, sa[x], n['a'][x]) for x in n['a'] if x in sa and sa[x] != n['a'][x])
        if loc_diff:
            other.append('attribute location differs: ' + ', '.join(loc_diff))
        if so and n['o'] not in so:
            msg = 'fragment outputs: stock %s, native %s' % (','.join(sorted(so)), n['o'])
            if n['family'] == 'gbuffer' and so == {'1'}:
                notes.append(msg + ' (expected for the G-buffer family: one array against four variables)')
            else:
                other.append(msg)
        nat_only = sorted(x for x in n['u'] if x not in su)
        if nat_only:
            notes.append('native only uniforms: ' + ', '.join(nat_only))
        ss_only = sorted(x for x in ss if x not in n['s'])
        if ss_only:
            notes.append('stock only samplers: ' + ', '.join(ss_only))
        a_nat = sorted(x for x in n['a'] if x not in sa)
        if a_nat:
            notes.append('native only vertex inputs: ' + ', '.join(a_nat))
        rows.append((label, n['family'], keyissues, drops, other, notes))

with_key = [r for r in rows if r[2]]
with_drop = [r for r in rows if r[3]]
with_other = [r for r in rows if r[4]]
lines = ['# Interface comparison: stock program against its native program', '',
         'Made by `doc/vulkan/interface-diff.py` from `inventory-data/interface-diff.txt` (one RT-off run with `LL_VK_INTERFACE_DIFF=1 LL_VK_UNICENSUS=1 LL_VK_NATIVE_BUILD_ALL=1`) and the manifest. Do not edit by hand.', '',
         '## Counts', '',
         '- manifest lines with a native interface: %d (not built or failed: %d); stock programs loaded: %d' % (sum(len(v) for v in native.values()), len(none), nprog),
         '- lines whose program name is not among the loaded stock programs (terrain variants this run did not make): %d' % len(no_stock),
         '- compared lines: %d' % len(rows),
         '  - with a variant key the native program does not declare: %d' % len(with_key),
         '  - with specific dropped writes (not environment values, not keys): %d' % len(with_drop),
         '  - with another issue (type mismatch, native only sampler, attribute location, output count outside the G-buffer family): %d' % len(with_other),
         '  - with none of these: %d' % len([r for r in rows if not r[2] and not r[3] and not r[4]]),
         '- environment values written on more than %d%% of the stock programs (%d names, not counted as drops): %s' % (int(BROADCAST_SHARE * 100), len(broadcast), ', '.join(sorted(broadcast))), '']
lines += ['## Variant keys the native program does not declare', '',
          'The manifest reads these from the old program at the draw. A native-only program has no old program: it must declare them (a member that nothing reads is enough) or the variant choice moves to the C++ side.', '',
          '| program (manifest line) | key |', '|---|---|']
for label, fam, keys, d, o, nt in with_key:
    lines.append('| %s | %s |' % (label, '<br>'.join(keys)))
lines += ['', '## Specific dropped writes', '',
          'Uniforms the callers wrote on the stock program that the native program does not declare. Each is ignored today as well; each is read once against the native shader to be sure it does not need the value under another name.', '',
          '| program (manifest line) | family | dropped |', '|---|---|---|']
for label, fam, k, drops, o, nt in with_drop:
    lines.append('| %s | %s | %s |' % (label, fam, ', '.join(drops)))
lines += ['', '## Other issues', '', '| program (manifest line) | issue |', '|---|---|']
for label, fam, k, d, other, nt in with_other:
    lines.append('| %s | %s |' % (label, '<br>'.join(other)))
lines += ['', '## Lines with no stock program of that name in this run', ''] + ['- ' + x for x in no_stock]
lines += ['', '## Notes (not issues)', '', '| program (manifest line) | notes |', '|---|---|']
for label, fam, k, d, o, notes in rows:
    if notes:
        lines.append('| %s | %s |' % (label, '<br>'.join(notes)))
open(OUT, 'w', newline='\n').write('\n'.join(lines) + '\n')
print('compared %d | key issues %d | specific drops %d | other %d | clean %d | no stock %d | broadcast names %d' % (len(rows), len(with_key), len(with_drop), len(with_other), len([r for r in rows if not r[2] and not r[3] and not r[4]]), len(no_stock), len(broadcast)))
