#!/usr/bin/env python3
"""Shader inventory (PLAN-draw-info-removal.md stage 4).

Reads the program census of one or more laps (RK_VK_PROGRAM_CENSUS=1, lines "name | loaded | binds | level | files | native") and the code
(indra/newview/llviewershadermgr.cpp assigns every program's name to a C++ global), and writes doc/vulkan/INVENTORY-shaders.md:

  * every program name: bound in a lap or not, the files, native or stock;
  * for every program never bound in a lap: the C++ global behind it and every place outside the shader manager's own setup that names it
    (a use site = a place that can bind it), plus the program whose rigged variant it is, if any.

A program with no use site and no parent that is used is dead code: the program and its files can be deleted.
Usage: inventory-shaders.py [census files...]   (default: the two kept census files beside this script in inventory-data/)
"""
import os
import re
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
HERE = os.path.dirname(os.path.abspath(__file__))
MGR = os.path.join(REPO, 'indra', 'newview', 'llviewershadermgr.cpp')
SRC_DIRS = [os.path.join(REPO, 'indra', d) for d in ('newview', 'llrender', 'llrender', 'llui', 'llwindow')]
MANIFEST = os.path.join(REPO, 'indra', 'newview', 'app_settings', 'shaders', 'native', 'manifest.txt')
OUT = os.path.join(HERE, 'INVENTORY-shaders.md')


def read(p):
    return open(p, 'rb').read().decode('utf-8', 'replace')


def load_census(path):
    rows = {}
    for line in read(path).splitlines():
        r = [x.strip() for x in line.split(' | ')]
        if len(r) < 6:
            continue
        binds = int(r[2].split()[1])
        name = r[0]
        row = rows.setdefault(name, {'binds': 0, 'files': r[4][6:], 'native': r[5][7:] != ';', 'level': r[3].split()[1], 'count': 0})
        row['binds'] += binds
        row['count'] += 1
    return rows


def base_name(name):
    n = name
    if n.endswith(' pull'):
        n = n[:-5]
    if n.startswith('Skinned '):
        n = n[8:]
    return n


def main():
    census_files = sys.argv[1:] or [os.path.join(HERE, 'inventory-data', 'program-census-off.txt'), os.path.join(HERE, 'inventory-data', 'program-census-rt.txt')]
    laps = [load_census(p) for p in census_files]
    names = sorted(set().union(*[set(l) for l in laps]))
    used_lap = {n: sum(l.get(n, {}).get('binds', 0) for l in laps) for n in names}

    mgr = read(MGR)
    # name -> global (an indexed global keeps its base and is flagged)
    glob_of = {}
    for m in re.finditer(r'(\w+)(\[[^\]]*\])?\.mName\s*=\s*"([^"]+)"', mgr):
        glob_of.setdefault(m.group(3), (m.group(1), bool(m.group(2))))
    parent_of = {}
    for m in re.finditer(r'(\w+)(?:\[[^\]]*\])?\.mRiggedVariant\s*=\s*&(\w+)', mgr):
        parent_of[m.group(2)] = m.group(1)

    files = []
    for d in set(SRC_DIRS):
        for root, _, fs in os.walk(d):
            for f in fs:
                if f.endswith(('.cpp', '.h')):
                    files.append(os.path.join(root, f))
    text = {p: read(p).splitlines() for p in files}

    def sites(g):
        out = []
        pat = re.compile(r'\b' + re.escape(g) + r'\b')
        setup = re.compile(r'\b' + re.escape(g) + r'(\[[^\]]*\])?\.(mName|mFeatures|mShaderFiles|mShaderLevel|mRiggedVariant|mGLTFVariants|mShaderGroup|mDefines|mUniform|attachNothing|createShader|unload|mFeatures)\b')
        for p, lines in text.items():
            rel = os.path.relpath(p, REPO).replace('\\', '/')
            for i, l in enumerate(lines, 1):
                if not pat.search(l):
                    continue
                s = l.strip()      # (a declaration is `RKShaderProgram gName;` alone on a line: `RKShaderProgram *shader = &gName;` is a use)
                if s.startswith('extern') or s.startswith('//') or re.match(r'(LLGLSLShader|RKShaderProgram)\s+\w+(\[[^\]]*\])?\s*;', s):
                    continue
                if rel.endswith('llviewershadermgr.cpp') and (setup.search(l) or re.search(r'\.createShader|\.unload\(', l)):
                    continue
                out.append('%s:%d' % (rel, i))
        return out

    manifest = [l.split('|')[1].strip() for l in read(MANIFEST).splitlines() if l.startswith('override')]

    lines = ['# Shader inventory', '', 'Made by `doc/vulkan/inventory-shaders.py` from the program census of %d laps (`inventory-data/`) and the code of `llviewershadermgr.cpp`. Do not edit by hand.' % len(laps), '']
    never = [n for n in names if used_lap[n] == 0]
    lines += ['## Counts', '',
              '- program names loaded: %d' % len(names),
              '- bound in at least one lap: %d' % (len(names) - len(never)),
              '- never bound in a lap: %d' % len(never),
              '- manifest overrides (native programs): %d' % len(manifest), '']
    dead, reach, unmapped = [], [], []
    rows = []
    for n in never:
        b = base_name(n)
        g = glob_of.get(b) or glob_of.get(n)
        if not g:
            unmapped.append(n)
            rows.append((n, '?', 'no global found (name built in code)', 'REVIEW'))
            continue
        gname, indexed = g
        s = sites(gname)
        par = parent_of.get(gname)
        par_note = ''
        if par:
            par_name = next((k for k, v in glob_of.items() if v[0] == par), par)
            par_note = 'variant of %s (%s)' % (par, 'bound in a lap' if used_lap.get(par_name, 0) else 'not bound in a lap')
        if indexed:
            verdict = 'REVIEW (indexed)'
            reach.append(n)
        elif s or (par and used_lap.get(next((k for k, v in glob_of.items() if v[0] == par), ''), 0) > 0):
            verdict = 'REACHABLE'
            reach.append(n)
        else:
            verdict = 'DEAD'
            dead.append(n)
        rows.append((n, gname, '%d sites: %s%s' % (len(s), ', '.join(s[:4]), ' ...' if len(s) > 4 else '') + ((' ; ' + par_note) if par_note else ''), verdict))
    lines += ['- never bound and no use site in the code (DEAD): %d' % len(dead),
              '- never bound but reachable from code (REACHABLE / REVIEW): %d' % len(reach),
              '- name not mapped to a global (REVIEW): %d' % len(unmapped), '']
    lines += ['## Never bound in a lap: what the code says', '', '| program | global | use sites outside the setup | verdict |', '|---|---|---|---|']
    for r in rows:
        lines.append('| %s | %s | %s | %s |' % r)
    # the names built in loops: grouped by family, with the line in the shader manager that makes the name
    mgr_lines = mgr.splitlines()
    fam = {}
    for n in unmapped:
        key = re.sub(r'\s+\d+(\s.*)?$', ' N', n) if re.search(r'\s\d+(\s|$)', n) else re.sub(r'\s*\((Low|Medium|High|Ultra)\)', ' (quality)', n)
        fam.setdefault(key, []).append(n)
    lines += ['', '## Never bound: families built in loops', '', 'Each family is chosen at run time by a selector, so the check is on the selector (which index or flag picks a member), not on a global. Review each family as a unit.', '', '| family | members never bound | where the name is made (llviewershadermgr.cpp) |', '|---|---|---|']
    for key, mem in sorted(fam.items()):
        probe = re.split(r'\s\d|\s\(|\s+N$', key)[0]
        where = [str(i) for i, l in enumerate(mgr_lines, 1) if 'mName' in l and probe in l]
        lines.append('| %s | %d | %s |' % (key, len(mem), ', '.join(where[:4]) or '(derived: pull twin / skinned variant of another program)'))
    # bound in a lap and no native override: the porting list, busiest first
    ov = set(manifest)

    def has_native(n):
        return n in ov or (n.endswith(' pull') and n[:-5] in ov) or (n.startswith('Skinned ') and n[8:] in ov)
    port = [n for n in names if used_lap[n] > 0 and not has_native(n)]
    done = [n for n in names if used_lap[n] > 0 and has_native(n)]
    lines += ['', '## Bound in a lap, no native override: port next (%d; %d bound ones already have one)' % (len(port), len(done)), '',
              'Busiest first. Binds are the two laps added (RT off + RT on). Group = the first directory of the stock vertex file.', '',
              '| program | binds | group | files |', '|---|---|---|---|']
    for n in sorted(port, key=lambda n: -used_lap[n]):
        r = next(l[n] for l in laps if n in l)
        grp = r['files'].split(';')[0].split('/')[0] if r['files'] else '?'
        lines.append('| %s | %d | %s | %s |' % (n, used_lap[n], grp, r['files']))
    lines += ['', '## Bound in a lap', '', '| program | binds | level | native | files |', '|---|---|---|---|---|']
    for n in names:
        if used_lap[n] > 0:
            r = next(l[n] for l in laps if n in l)
            lines.append('| %s | %d | %s | %s | %s |' % (n, used_lap[n], r['level'], 'native' if any(l.get(n, {}).get('native') for l in laps) else 'stock', r['files']))
    # native coverage: the NativeCensus lines of the laps (draws that took the native program, draws of the old program that did not, by reason)
    nat = []
    for f in ('native-census-off.txt', 'native-census-rt.txt'):
        fp = os.path.join(HERE, 'inventory-data', f)
        if os.path.exists(fp):
            nat.append([[x.strip() for x in l.split(' | ')] for l in read(fp).splitlines() if l.strip()])
    if nat:
        val = lambda r, i: int(r[i].split()[1])
        n = len(nat[0])
        uses = [sum(val(l[k], 2) for l in nat) for k in range(n)]
        reasons = ['family_off', 'not_fit', 'rt', 'not_ready', 'outputs']
        fb = [[sum(val(l[k], 3 + j) for l in nat) for j in range(5)] for k in range(n)]
        lines += ['', '## Native coverage (laps: %d)' % len(nat), '',
                  '- manifest lines: %d; made native draws in a lap: %d; never reached in a lap: %d' % (n, sum(1 for u in uses if u), sum(1 for k in range(n) if not uses[k] and not any(fb[k]))),
                  '- draws of the old program that did not take the native one, by reason: ' + ', '.join('%s %d' % (reasons[j], sum(f[j] for f in fb)) for j in range(5)), '',
                  '| manifest line (program) | family | native draws | ' + ' | '.join(reasons) + ' |', '|---|---|---|---|---|---|---|---|']
        for k in sorted(range(n), key=lambda k: -sum(fb[k])):
            if any(fb[k]):
                lines.append('| %s | %s | %d | %s |' % (nat[0][k][0], nat[0][k][1][7:], uses[k], ' | '.join(str(x) for x in fb[k])))
    open(OUT, 'w', newline='\n').write('\n'.join(lines) + '\n')
    print('names', len(names), 'bound', len(names) - len(never), 'never', len(never), 'dead', len(dead), 'reachable/review', len(reach), 'unmapped', len(unmapped))


main()
