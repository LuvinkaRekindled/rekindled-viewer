"""The weekly look at the three upstreams (doc/vulkan/UPSTREAM-WATCH.md; this script lives in doc/vulkan/upstream-watch/): Linden Lab's viewer (bug fixes only), the Firestorm releases, Aurora (ideas only).
Read-only: it asks GitHub through `gh`, fetches release tags of the Firestorm remote into the local repository, and dry-runs a merge with `git merge-tree`; it changes no branch and no working file.
The vendor SDK section (NVIDIA DLSS, Streamline, AMD FidelityFX: releases, is dlss_nr public, Vulkan support of the FSR features) is in the same digest.
It writes the digest <work>\\phoenix-firestorm\\doc\\vulkan\\upstream-watch\\reports\\<date>-digest.md and prints a short summary.

Usage:
  python <work>\\phoenix-firestorm\\doc\\vulkan\\upstream-watch\\upstream-check.py                  the digest of everything since the reviewed pointers in upstream-watch\\state.json
  python <work>\\phoenix-firestorm\\doc\\vulkan\\upstream-watch\\upstream-check.py --advance        after the triage: move the reviewed pointers to the heads the last digest looked at
  python <work>\\phoenix-firestorm\\doc\\vulkan\\upstream-watch\\upstream-check.py --baseline       set every pointer to the current head (first run), reviewing nothing
  python <work>\\phoenix-firestorm\\doc\\vulkan\\upstream-watch\\upstream-check.py --state <file>   use another state file (tests); with --ll-from / --fs-from / --aurora-from to override one pointer
"""
import argparse
import datetime
import json
import os
import re
import subprocess
import sys
import collections

REPO = r'<work>\phoenix-firestorm'
WATCH = os.path.join(REPO, 'doc', 'vulkan', 'upstream-watch')
STATE = os.path.join(WATCH, 'state.json')
LL = 'secondlife/viewer'
AURORA = 'Aurora-Viewer/Aurora-Viewer'
FS_REMOTE = 'origin'
FS_TAG_GLOB = 'Firestorm_Release_*'
# vendor SDKs (not code we merge: what the vendors ship that our upscaler and neural paths could use)
SDK_REPOS = {
    'dlss': ('NVIDIA/DLSS', 'NVIDIA DLSS SDK'),
    'streamline': ('NVIDIAGameWorks/Streamline', 'NVIDIA Streamline'),
    'fidelityfx': ('GPUOpen-LibrariesAndSDKs/FidelityFX-SDK', 'AMD FidelityFX / FSR SDK'),
}
NR_RX = re.compile(r'dlss_?nr|dlssnr|neural', re.I)

FIX_WORDS = re.compile(r'\b(fix(es|ed)?|crash(es)?|overflow|leak(s)?|use-after-free|uaf|hardening|harden|prevent(s)?|guard|null|assert|deadlock|race|regression|revert|corrupt|invalid|overread|out.of.bounds|freeze|hang|persists?|desync|protections?|reinforce|stale|dangling|dead)\b', re.I)
# Aurora subjects that touch what we are working on (flagged in the digest)
OURS = re.compile(r'shadow|cull|occlu|texture|mip|bc7|skin|rigged|avatar|\brt\b|ray|dlss|fsr|upscal|bindless|indirect|compute|particle|lod|mesh|batch|draw|frame ?time|perf|gpu|vram|stall|hitch|thread', re.I)


def run(cmd, cwd=REPO, check=False):
    p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, encoding='utf-8', errors='replace')
    if check and p.returncode:
        raise RuntimeError('%s failed: %s' % (' '.join(cmd), p.stderr.strip()))
    return p.stdout


def gh_json(path):
    out = subprocess.run(['gh', 'api', path], capture_output=True, text=True, encoding='utf-8', errors='replace')
    if out.returncode:
        raise RuntimeError('gh api %s: %s' % (path, out.stderr.strip()))
    return json.loads(out.stdout)


def load_state(path):
    with open(path, encoding='utf-8') as f:
        return json.load(f)


def save_state(path, st):
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(st, f, indent=2)
        f.write('\n')


def heat(base):
    """lines changed per file by our branch since the base (what an upstream change to the file would have to merge with)"""
    h = {}
    for ln in run(['git', 'diff', '--numstat', '--no-renames', base, 'native-vk']).splitlines():
        a, d, name = ln.split('\t', 2)
        if a == '-' or d == '-':
            h[name] = 1000000
        else:
            h[name] = int(a) + int(d)
    return h


def label(n):
    if not n:
        return 'clean'
    return ('light %d' % n) if n <= 40 else ('HEAVY %d' % n)


def commits_since(repo, branch, pointer, cap_pages=8):
    """newest first, until the pointer (not included); merges left out"""
    out = []
    for page in range(1, cap_pages + 1):
        chunk = gh_json('repos/%s/commits?sha=%s&per_page=100&page=%d' % (repo, branch, page))
        if not chunk:
            break
        for c in chunk:
            if pointer and c['sha'].startswith(pointer[:9]):
                return out, True
            out.append(c)
    return out, False


def first_line(c):
    return c['commit']['message'].split('\n')[0].strip()


def section_ll(st, heats, lines, heads, override):
    pointer = override or st['ll'].get('reviewed_sha')
    lines.append('## Linden Lab viewer (%s, branch %s): bug fixes only\n' % (LL, st['ll']['branch']))
    commits, found = commits_since(LL, st['ll']['branch'], pointer)
    heads['ll'] = None
    if not commits:
        lines.append('Nothing new since `%s`.\n' % (pointer or '')[:9])
        heads['ll'] = pointer
        return
    heads['ll'] = commits[0]['sha']
    commits = [c for c in commits if len(c['parents']) < 2]
    lines.append('%d commits since `%s`%s (merges left out). Newest `%s`.\n' % (len(commits), (pointer or '')[:9], '' if found else ' (pointer not found within the pages read: the list is cut)', heads['ll'][:9]))
    fixes, others = [], []
    for c in commits:
        (fixes if FIX_WORDS.search(first_line(c)) else others).append(c)
    lines.append('### Fixes (%d)\n' % len(fixes))
    lines.append('| commit | date | subject | files, against our changes | hint |')
    lines.append('|---|---|---|---|---|')
    for c in fixes:
        d = gh_json('repos/%s/commits/%s' % (LL, c['sha']))
        files = d.get('files', [])
        worst = max([heats.get(f['filename'], 0) for f in files] or [0])
        names = ', '.join('%s (%s)' % (os.path.basename(f['filename']), label(heats.get(f['filename'], 0))) for f in files[:6])
        if len(files) > 6:
            names += ', +%d more' % (len(files) - 6)
        hint = 'applies' if worst == 0 else ('applies, check the merge' if worst <= 40 else 'shared or replaced: read it')
        lines.append('| `%s` | %s | %s | %s | %s |' % (c['sha'][:9], c['commit']['committer']['date'][:10], first_line(c).replace('|', '/')[:110], names, hint))
    lines.append('\n### Everything else (%d, titles only; features wait for Firestorm)\n' % len(others))
    for c in others:
        lines.append('- `%s` %s' % (c['sha'][:9], first_line(c)[:130]))
    lines.append('')


def section_fs(st, heats, lines, heads, override):
    lines.append('## Firestorm releases (%s tags %s)\n' % (FS_REMOTE, FS_TAG_GLOB))
    run(['git', 'fetch', FS_REMOTE, '--tags', '--no-recurse-submodules', '-q'])
    tags = run(['git', 'tag', '--list', FS_TAG_GLOB, '--sort=version:refname']).split()
    pointer = override or st['fs'].get('reviewed_tag')
    heads['fs'] = tags[-1] if tags else pointer
    new = tags[tags.index(pointer) + 1:] if pointer in tags else []
    merged = st['fs'].get('merged_tag')
    lines.append('Reviewed up to `%s`; merged into native-vk: `%s`; newest tag `%s`.\n' % (pointer, merged, tags[-1] if tags else '?'))
    if not new:
        lines.append('No new release since the reviewed one.\n')
        return
    prev = pointer
    for tag in new:
        log = run(['git', 'log', '--no-merges', '--format=%h\t%ad\t%s', '--date=short', '%s..%s' % (prev, tag)]).splitlines()
        files = run(['git', 'diff', '--name-only', '--no-renames', prev, tag]).split()
        overlap = [(f, heats[f]) for f in files if f in heats]
        lines.append('### %s: %d commits, %d files, %d of them also changed by us\n' % (tag, len(log), len(files), len(overlap)))
        lines.append('Overlap, heaviest first: ' + ', '.join('%s (%s)' % (os.path.basename(f), label(n)) for f, n in sorted(overlap, key=lambda x: -x[1])[:25]) + '\n')
        mt = run(['git', 'merge-tree', '--write-tree', '--name-only', 'native-vk', tag]).split('\n')
        conflicts = []
        for ln in mt[1:]:
            if not ln.strip():
                break
            conflicts.append(ln.strip())
        lines.append('Dry-run merge into native-vk: %s\n' % (('%d conflicts: ' % len(conflicts)) + ', '.join(conflicts) if conflicts else 'no conflicts'))
        lines.append('| commit | date | subject | touches (against our changes) |')
        lines.append('|---|---|---|---|')
        for ln in log:
            sha, date, subj = ln.split('\t', 2)
            fs = run(['git', 'diff-tree', '--no-commit-id', '--name-only', '--no-renames', '-r', sha]).split()
            touched = [(f, heats[f]) for f in fs if f in heats]
            note = ', '.join('%s (%s)' % (os.path.basename(f), label(n)) for f, n in sorted(touched, key=lambda x: -x[1])[:4]) or ''
            lines.append('| `%s` | %s | %s | %s |' % (sha, date, subj.replace('|', '/')[:110], note))
        lines.append('')
        prev = tag


def section_aurora(st, lines, heads, override):
    pointer = override or st['aurora'].get('reviewed_sha')
    lines.append('## Aurora (%s, branch %s): ideas only (GPL-3.0, Firestorm is LGPL: ideas and measurements, never code)\n' % (AURORA, st['aurora']['branch']))
    commits, found = commits_since(AURORA, st['aurora']['branch'], pointer, cap_pages=12)
    if not commits:
        lines.append('Nothing new since `%s`.\n' % (pointer or '')[:9])
        heads['aurora'] = pointer
        return
    heads['aurora'] = commits[0]['sha']
    lines.append('%d commits since `%s`%s. Newest `%s`.\n' % (len(commits), (pointer or '')[:9], '' if found else ' (pointer not found within the pages read: the list is cut)', heads['aurora'][:9]))
    rx = re.compile(r'^(\w+)(?:\(([^)]+)\))?!?:\s*(.*)$')
    counts = collections.Counter()
    shown = []
    for c in commits:
        subj = first_line(c)
        m = rx.match(subj)
        kind, scope, text = (m.group(1), m.group(2) or '', m.group(3)) if m else ('other', '', subj)
        counts[(kind, scope)] += 1
        want = kind in ('perf',) or (kind in ('feat', 'fix', 'docs', 'refactor') and scope in ('render', 'perf', 'gpu', 'world', 'streaming', 'textures', 'texture', 'mesh', 'avatar')) or OURS.search(text)
        if want:
            shown.append((c['sha'][:9], c['commit']['committer']['date'][:10], kind, scope, text))
    lines.append('By kind and scope: ' + ', '.join('%s(%s) %d' % (k, s, n) for (k, s), n in counts.most_common(14)) + '\n')
    lines.append('### Worth a look (%d: perf, render/world/texture/mesh/avatar scopes, or a word of ours)\n' % len(shown))
    for sha, date, kind, scope, text in shown:
        lines.append('- `%s` %s %s%s: %s' % (sha, date, kind, ('(%s)' % scope) if scope else '', text[:150]))
    lines.append('')


def gh_bytes(path, rng=None):
    cmd = ['gh', 'api', path, '-H', 'Accept: application/octet-stream']
    if rng:
        cmd += ['-H', 'Range: bytes=%s' % rng]
    out = subprocess.run(cmd, capture_output=True)
    if out.returncode:
        raise RuntimeError('gh api %s: %s' % (path, out.stderr.decode('utf-8', 'replace').strip()))
    return out.stdout


def zip_names(repo, asset):
    """file names inside a release zip, read from its central directory with two range requests (the zips are 200+ MB)"""
    size = asset['size']
    path = 'repos/%s/releases/assets/%d' % (repo, asset['id'])
    tail = gh_bytes(path, '%d-%d' % (max(0, size - 70000), size - 1))
    i = tail.rfind(b'PK\x05\x06')
    if i < 0:
        raise RuntimeError('no end-of-central-directory record in the zip tail')
    cd_size = int.from_bytes(tail[i + 12:i + 16], 'little')
    cd_off = int.from_bytes(tail[i + 16:i + 20], 'little')
    if cd_off == 0xFFFFFFFF:
        raise RuntimeError('zip64 directory (not handled)')
    cd = gh_bytes(path, '%d-%d' % (cd_off, cd_off + cd_size - 1))
    names, pos = [], 0
    while pos + 46 <= len(cd) and cd[pos:pos + 4] == b'PK\x01\x02':
        n = int.from_bytes(cd[pos + 28:pos + 30], 'little')
        m = int.from_bytes(cd[pos + 30:pos + 32], 'little')
        k = int.from_bytes(cd[pos + 32:pos + 34], 'little')
        names.append(cd[pos + 46:pos + 46 + n].decode('utf-8', 'replace'))
        pos += 46 + n + m + k
    return names


def gh_file(repo, path):
    import base64
    return base64.b64decode(gh_json('repos/%s/contents/%s' % (repo, path))['content']).decode('utf-8', 'replace')


def section_fidelityfx(repo, rels, ent, heads, new, lines):
    # the binaries are the plain answer to "does feature X have Vulkan": a signed *_vk.dll per effect (the release notes only name some)
    tag = rels[0]['tag_name'] if rels else 'main'
    tree = [t['path'] for t in gh_json('repos/%s/git/trees/%s?recursive=1' % (repo, tag))['tree']]
    bins = sorted(os.path.basename(t) for t in tree if '/signedbin/' in t and t.lower().endswith('.dll'))
    vk = [b for b in bins if b.lower().endswith('_vk.dll')]
    known = ent.get('known_vk_dlls')
    lines.append('Signed binaries in %s: %d DLLs, Vulkan ones (%d): %s%s' % (tag, len(bins), len(vk), ', '.join(vk), '' if known is None else ('; new since the last review: ' + (', '.join(sorted(set(vk) - set(known))) or 'none'))))
    dx_only = sorted(b[:-9] for b in bins if b.lower().endswith('_dx12.dll') and (b[:-9] + '_vk.dll') not in vk and 'loader' not in b)
    lines.append('DX12-only effects (a `_dx12.dll` with no `_vk.dll`): %s%s' % (', '.join(dx_only) or 'none', '  <-- the denoiser (Ray Regeneration) appearing as a Vulkan DLL is the signal we wait for' if any('denoiser' in d for d in dx_only) else ''))
    lines.append('')
    heads['sdk_known']['fidelityfx'] = {'known_vk_dlls': vk}
    for r in rels:
        if r['tag_name'] not in new:
            continue
        body = r.get('body') or ''
        lines.append('#### %s release notes: the effects named and the Vulkan lines\n' % r['tag_name'])
        effects = re.findall(r'<h3>(.*?)</h3>', body)
        lines.append('Effects named: ' + ('; '.join(effects) if effects else '(none parsed)') + '\n')
        vk = [ln.strip() for ln in body.splitlines() if re.search(r'vulkan|\bvk\b', ln, re.I) or re.search(r'dx12 only|directx.{0,6}12 only|not .{0,20}vulkan', ln, re.I)]
        lines.append('Vulkan lines (%d)%s' % (len(vk), '' if vk else ': none, check whether the release is DX12 only'))
        shown = collections.Counter(re.sub(r'^[-*\s]+', '', ln)[:200] for ln in vk)
        for ln, n in list(shown.items())[:25]:
            lines.append('- %s%s' % (ln, (' (x%d)' % n) if n > 1 else ''))
        lines.append('')


def section_nvidia(key, repo, rels, ent, heads, lines):
    branch = gh_json('repos/%s' % repo)['default_branch']
    tree = [t['path'] for t in gh_json('repos/%s/git/trees/%s?recursive=1' % (repo, branch))['tree']]
    nr_paths = [t for t in tree if NR_RX.search(t)]
    hdrs = sorted(os.path.basename(t) for t in tree if re.match(r'^include/[^/]+\.h$', t))
    plugins = sorted({t.split('/')[2] for t in tree if t.startswith('source/plugins/') and t.count('/') >= 3})
    known_h, known_p, kf = ent.get('known_headers'), ent.get('known_plugins'), ent.get('known_features')
    lines.append('`dlss_nr` / neural named in the repo tree: %s' % ((', '.join(nr_paths[:20])) if nr_paths else 'nothing'))
    lines.append('Public headers: %d%s' % (len(hdrs), '' if known_h is None else ('; new since the last review: ' + (', '.join(sorted(set(hdrs) - set(known_h))) or 'none'))))
    if plugins:
        lines.append('Plugins (source/plugins): %d%s' % (len(plugins), '' if known_p is None else ('; new: ' + (', '.join(sorted(set(plugins) - set(known_p))) or 'none'))))
    if key == 'dlss':
        feats = sorted(set(re.findall(r'NVSDK_NGX_Feature_(\w+)\s*=', gh_file(repo, 'include/nvsdk_ngx_defs.h'))))
    else:
        feats = sorted(set(re.findall(r'(kFeature\w+)\s*=\s*\d+', gh_file(repo, 'include/sl_core_types.h'))))
    lines.append('Feature ids (%d): %s%s' % (len(feats), ', '.join(feats), '' if kf is None else ('; new: ' + (', '.join(sorted(set(feats) - set(kf))) or 'none'))))
    zip_hits = []
    dlls = None
    if key == 'streamline' and rels:
        zasset = next((a for a in rels[0]['assets'] if re.fullmatch(r'streamline-sdk-v[\d.]+\.zip', a['name'])), None)
        if zasset:
            try:
                names = zip_names(repo, zasset)
                zip_hits = [n for n in names if NR_RX.search(n)]
                dlls = sorted({os.path.basename(n) for n in names if n.lower().endswith('.dll') and '/x64/' in n.lower() and 'development' not in n.lower()})
                lines.append('Release zip `%s` (%d entries): `dlss_nr` / neural named inside: %s' % (zasset['name'], len(names), ', '.join(zip_hits[:20]) if zip_hits else 'nothing'))
                lines.append('Streamline x64 release DLLs (%d): %s%s' % (len(dlls), ', '.join(dlls), '' if ent.get('known_dlls') is None else ('; new: ' + (', '.join(sorted(set(dlls) - set(ent['known_dlls']))) or 'none'))))
            except Exception as e:
                lines.append('Release zip listing failed: %s' % e)
    new_nr_feats = [f for f in feats if NR_RX.search(f) and f != 'kFeatureDLSS_NR' and f not in (kf or [])]   # the Streamline id is known (the sl core types name it, nothing else exists)
    nr_new_file = bool(nr_paths or zip_hits or new_nr_feats)
    lines.append('')
    lines.append('**%s**\n' % ('NR SIGNAL: a file or id named dlss_nr / neural is in this SDK: read it' if nr_new_file else 'no public NR files in this SDK (Streamline already names the id kFeatureDLSS_NR = 1004 and nothing more)'))
    known = {'known_headers': hdrs, 'known_plugins': plugins, 'known_features': feats}
    if dlls is not None:
        known['known_dlls'] = dlls
    heads['sdk_known'][key] = known


def section_sdks(st, lines, heads):
    sdk = st.setdefault('sdk', {})
    heads['sdk'] = {}
    heads['sdk_known'] = {}
    lines.append('## Vendor SDKs: what NVIDIA and AMD ship that our upscaler and neural paths could use\n')
    lines.append('Not merged code, a look at releases. NVIDIA: is `dlss_nr` (DLSS neural rendering) public yet, meaning a file, header, plugin or feature id with `dlss_nr` or `neural` in its name in the DLSS SDK or in Streamline (repo and release zip). AMD: which FSR features the SDK has Vulkan support for (we ship FSR 3.1 on the Vulkan ffx-api, SDK 1.1.4).\n')
    for key, (repo, title) in SDK_REPOS.items():
        ent = sdk.setdefault(key, {})
        lines.append('### %s (`%s`)\n' % (title, repo))
        try:
            rels = gh_json('repos/%s/releases?per_page=12' % repo)
            tags = [r['tag_name'] for r in rels]
            pointer = ent.get('reviewed_tag')
            heads['sdk'][key] = tags[0] if tags else pointer
            new = tags[:tags.index(pointer)] if pointer in tags else tags[:3]
            lines.append('Reviewed up to `%s`; newest release `%s`%s.\n' % (pointer, tags[0] if tags else '?', '' if new else ', nothing new'))
            for r in rels:
                if r['tag_name'] in new:
                    lines.append('- **%s** (%s): %s' % (r['tag_name'], r['published_at'][:10], r['name']))
            if new:
                lines.append('')
            if key == 'fidelityfx':
                section_fidelityfx(repo, rels, ent, heads, new, lines)
            else:
                section_nvidia(key, repo, rels, ent, heads, lines)
        except Exception as e:
            lines.append('FAILED: %s\n' % e)
        lines.append('')


def main():
    sys.stdout.reconfigure(encoding='utf-8')
    ap = argparse.ArgumentParser()
    ap.add_argument('--advance', action='store_true')
    ap.add_argument('--baseline', action='store_true')
    ap.add_argument('--state', default=STATE)
    ap.add_argument('--ll-from')
    ap.add_argument('--fs-from')
    ap.add_argument('--aurora-from')
    a = ap.parse_args()
    os.makedirs(os.path.join(WATCH, 'reports'), exist_ok=True)
    last = os.path.join(WATCH, 'reports', 'last-digest.json')

    if a.baseline:
        st = {'our_base': 'master', 'll': {'branch': 'develop'}, 'fs': {'merged_tag': None}, 'aurora': {'branch': 'main'}}
        st['ll']['reviewed_sha'] = gh_json('repos/%s/commits/develop' % LL)['sha']
        run(['git', 'fetch', FS_REMOTE, '--tags', '--no-recurse-submodules', '-q'])
        st['fs']['reviewed_tag'] = run(['git', 'tag', '--list', FS_TAG_GLOB, '--sort=version:refname']).split()[-1]
        st['aurora']['reviewed_sha'] = gh_json('repos/%s/commits/main' % AURORA)['sha']
        save_state(a.state, st)
        print('baseline written to', a.state, json.dumps(st))
        return

    st = load_state(a.state)
    if a.advance:
        d = json.load(open(last, encoding='utf-8'))
        if d.get('ll'):
            st['ll']['reviewed_sha'] = d['ll']
        if d.get('fs'):
            st['fs']['reviewed_tag'] = d['fs']
        if d.get('aurora'):
            st['aurora']['reviewed_sha'] = d['aurora']
        for key, tag in (d.get('sdk') or {}).items():
            ent = st.setdefault('sdk', {}).setdefault(key, {})
            ent['reviewed_tag'] = tag
            ent.update((d.get('sdk_known') or {}).get(key, {}))
        save_state(a.state, st)
        print('reviewed pointers moved to the heads of the digest of', d['date'], json.dumps(st))
        return

    heats = heat(st.get('our_base', 'master'))
    today = datetime.date.today().isoformat()
    lines = ['# Upstream digest, %s\n' % today,
             'Read-only look since the reviewed pointers (`upstream-watch/state.json`). "against our changes": lines our branch changed in that file since `%s` (clean = untouched by us; HEAVY = over 40 lines, expect a merge to need care or the file to be one we replaced).\n' % st.get('our_base', 'master')]
    heads = {}
    for name, fn in (('ll', lambda: section_ll(st, heats, lines, heads, a.ll_from)), ('fs', lambda: section_fs(st, heats, lines, heads, a.fs_from)), ('aurora', lambda: section_aurora(st, lines, heads, a.aurora_from)), ('sdk', lambda: section_sdks(st, lines, heads))):
        try:
            fn()
        except Exception as e:      # one source failing must not hide the others
            lines.append('## %s: FAILED: %s\n' % (name, e))
    out = os.path.join(WATCH, 'reports', '%s-digest.md' % today)
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    heads['date'] = today
    json.dump(heads, open(last, 'w', encoding='utf-8'))
    print('digest written to', out, '(%d lines)' % len(lines))
    for ln in lines:
        if ln.startswith('## ') or ln.startswith('### ') or re.match(r'^\d+ commits', ln) or ln.startswith('Dry-run') or ln.startswith('No new') or ln.startswith('Nothing new'):
            print(ln)


main()
