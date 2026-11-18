"""census_parse.py <log> [<log> ...] [--top N] [--json out.json] [--all-contexts]   (2026-11-14, full deferred renderer phase 0)

Reads the "GBC" blocks that [Main.Main.Misc] GBufferCensus writes into the Vegas Shaders log (src/core/GBufferCensus.cpp; format in
docs/next/FD-CENSUS-RESULTS.md) and prints, per scene (scene name + runtime), averaged over that scene's census frames:
  1. the opaque main-view draws (world context, main-size target, opaque or alpha-tested, colour written) by where they are drawn
     and by shader family, and how many wrote the G-buffer (the normal record, 'N');
  2. the "not covered" list: those opaque main-view draws that wrote no normal record, by place, pass, shader and reason, sorted by
     draws and then pixels;
  3. the totals, and every other draw of the frame by context / span / blend (blended, first person, reflections, other targets).
  4. (2026-11-14, blocks written with [Main.Main.Misc] GBufferCensusMerge = 1: "GBC I" / "GBC V" lines) the merge census of the
     batch-renderer question (docs/next/FD-BATCH-RESULTS.md): how many draws of the main opaque list would merge into instanced
     draws under the rules a (exact, game order), b (lamps ignored, game order), c (b + sorted inside a pass bucket), the two
     ENGINE-ANSWERS D7 levels for comparison, an upper bound, and which constant registers differ between copies.
Run with: py -3.11 -I tools/fd/census_parse.py <VegasShaders.log>
"""
import json
import sys
from collections import defaultdict

FIELDS_INT = ('n', 'px', 'pxn', 'zw', 'nvr', 'swap')
OPAQUE_BLENDS = ('opaque', 'test')


def parse_fields(text):
    out = {}
    for tok in text.split():
        if '=' in tok:
            k, v = tok.split('=', 1)
            out[k] = v
    return out


def read_blocks(lines):
    """[(begin dict, [row dict], end dict)] for every complete block; incomplete ones are counted in the second value."""
    blocks, dropped = [], 0
    cur = None
    for line in lines:
        at = line.find('GBC ')
        if at < 0:
            continue
        body = line[at + 4:].strip()
        kind, _, rest = body.partition(' ')
        if kind == 'BEGIN':
            if cur is not None:
                dropped += 1
            cur = (parse_fields(rest), [], None)
        elif kind == 'D' and cur is not None:
            row = parse_fields(rest)
            for k in FIELDS_INT:
                if k in row:
                    try:
                        row[k] = int(row[k])
                    except ValueError:
                        row[k] = 0
            cur[1].append(row)
        elif kind in ('I', 'V') and cur is not None:
            # merge census lines: kept on the BEGIN dict ('_I', '_V') so that the block stays a (begin, rows, end) triple
            cur[0].setdefault('_' + kind, []).append(parse_fields(rest))
        elif kind == 'END' and cur is not None:
            blocks.append((cur[0], cur[1], parse_fields(rest)))
            cur = None
    if cur is not None:
        dropped += 1
    return blocks, dropped


# Pass-ID ranges of the game's main list (ENGINE-ANSWERS.md, "How the opaque draws are ordered today"); names first, ranges second.
RANGES = [
    (0x001, 0x015, 'depth'), (0x016, 0x063, 'nolight-ambient'), (0x064, 0x12B, 'object-base'), (0x12C, 0x149, 'object-lamp-pass'),
    (0x14A, 0x153, 'object-texture-pass'), (0x154, 0x177, 'object-highlight-pass'), (0x178, 0x1E1, 'lighting30'),
    (0x1EF, 0x230, 'land'), (0x236, 0x23E, 'sky'), (0x241, 0x251, 'particles-env-decals'), (0x252, 0x256, 'fog'),
    (0x25C, 0x287, 'grass'), (0x288, 0x2CE, 'water'), (0x2CF, 0x2D7, 'after-water'),
]
NAME_RULES = [  # (substring of the pass name, group); first match wins
    ('GRASS', 'grass'), ('LEAVES', 'trees'), ('LEAF', 'trees'), ('BRANCH', 'trees'), ('TRUNK', 'trees'), ('SPEEDTREE', 'trees'),
    ('LANDLO', 'lod-land'), ('LODLAND', 'lod-land'), ('LOD', 'lod-object'), ('LAND', 'land'), ('SKY', 'sky'), ('WATER', 'water'),
    ('DECAL', 'decal'), ('PSYS', 'particles'), ('PARTICLE', 'particles'), ('FOG', 'fog'),
]


def pass_group(pass_hex, pass_name):
    if not pass_hex or pass_hex == '-':
        return 'no-pass'
    name = (pass_name or '').upper()
    for sub, group in NAME_RULES:
        if sub in name:
            return group
    try:
        pid = int(pass_hex, 16)
    except ValueError:
        return 'other'
    for lo, hi, group in RANGES:
        if lo <= pid <= hi:
            return group
    return 'pass-%03X' % pid


def where(row):
    """'<span>/<pass group>', e.g. 'main/object-base', 'grass-early/grass', 'blended/no-pass'."""
    return '%s/%s' % (row.get('span', '?'), pass_group(row.get('pass'), row.get('pn')))


def family(row):
    fam = row.get('fam', '-')
    if fam in ('-', ''):
        return 'unnamed' if row.get('ps', '').startswith('unnamed') else '-'
    if fam == 'own':
        return 'own:%s' % row.get('col', '-')
    if fam == 'game':
        return 'game'
    return fam + ('(swap)' if row.get('swap') else '')


def ps_label(row):
    ps = row.get('ps', '-')
    if ps.startswith('unnamed@'):
        return 'unnamed'
    return ps + ('*' if row.get('swap') else '')


def is_main_view(row):
    return row.get('ctx') == 'world' and row.get('tgt') == 'main'


def is_opaque(row):
    return row.get('blend') in OPAQUE_BLENDS


def wrote(row):
    return 'N' in row.get('gb', '')


class Scene:
    def __init__(self, key):
        self.key = key
        self.frames = 0
        self.pixels_measured = False
        self.ends = []
        self.begins = []
        self.rows = []

    def add(self, begin, rows, end):
        self.frames += 1
        self.begins.append(begin)
        self.ends.append(end)
        self.rows.extend(rows)
        if int(begin.get('mode', '1') or 1) >= 2:
            self.pixels_measured = True


def scene_key(begin):
    return '%s [%s]' % (begin.get('scene', '-'), begin.get('rt', '?'))


def collect(blocks):
    scenes = {}
    for begin, rows, end in blocks:
        k = scene_key(begin)
        scenes.setdefault(k, Scene(k)).add(begin, rows, end)
    return scenes


def summarize(scene, top=40):
    f = float(scene.frames)
    opaque = [r for r in scene.rows if is_main_view(r) and is_opaque(r)]
    by_family = defaultdict(lambda: [0, 0, 0, 0])  # draws, wrote draws, px, wrote px
    for r in opaque:
        e = by_family[(where(r), family(r))]
        e[0] += r['n']
        e[2] += max(r.get('px', -1), 0)
        if wrote(r):
            e[1] += r['n']
            e[3] += max(r.get('px', -1), 0)
    family_rows = sorted(({'where': w, 'family': fam, 'draws': v[0] / f, 'wrote': v[1] / f, 'not': (v[0] - v[1]) / f,
                           'px': v[2] / f, 'px_wrote': v[3] / f} for (w, fam), v in by_family.items()),
                         key=lambda d: (-d['draws'], d['where'], d['family']))
    missing = defaultdict(lambda: [0, 0])
    for r in opaque:
        if wrote(r):
            continue
        e = missing[(where(r), r.get('pass', '-'), r.get('pn', '-'), ps_label(r), family(r), r.get('blend', '?'), r.get('why', '?'))]
        e[0] += r['n']
        e[1] += max(r.get('px', -1), 0)
    missing_rows = sorted(({'where': k[0], 'pass': k[1], 'pass_name': k[2], 'ps': k[3], 'family': k[4], 'blend': k[5], 'why': k[6],
                            'draws': v[0] / f, 'px': v[1] / f} for k, v in missing.items()),
                          key=lambda d: (-d['draws'], -d['px'], d['where'], d['ps']))
    others = defaultdict(int)
    for r in scene.rows:
        if is_main_view(r) and is_opaque(r):
            continue
        others[(r.get('ctx', '?'), r.get('tgt', '?'), r.get('span', '?'), r.get('blend', '?'), 'gbuffer' if wrote(r) else '-')] += r['n']
    other_rows = sorted(({'ctx': k[0], 'tgt': k[1], 'span': k[2], 'blend': k[3], 'gbuffer': k[4], 'draws': v / f} for k, v in others.items()),
                        key=lambda d: -d['draws'])
    total_opaque = sum(r['n'] for r in opaque) / f
    total_wrote = sum(r['n'] for r in opaque if wrote(r)) / f
    px_all = sum(max(r.get('px', -1), 0) for r in opaque) / f
    px_wrote = sum(max(r.get('px', -1), 0) for r in opaque if wrote(r)) / f
    warnings = []
    for end in scene.ends:
        passes, pd = int(end.get('passes', 0) or 0), int(end.get('passdraws', 0) or 0)
        if passes > 50 and pd < passes // 4:
            warnings.append('census %s: %d passes but %d draws inside them: draw hooks lost, incomplete' % (end.get('serial', '?'), passes, pd))
        if end.get('cn') == '0':
            warnings.append('census %s: CapturedNormals was not live (nothing can write the G-buffer)' % end.get('serial', '?'))
        if int(end.get('pxlate', 0) or 0) or int(end.get('pxmiss', 0) or 0) or int(end.get('pxfail', 0) or 0):
            warnings.append('census %s: pixel counts incomplete (late %s, missed %s, failed %s)' % (end.get('serial', '?'), end.get('pxlate'), end.get('pxmiss'), end.get('pxfail')))
    return {
        'scene': scene.key, 'frames': scene.frames, 'pixels_measured': scene.pixels_measured,
        'msaa': sorted({e.get('msaa', '0') for e in scene.ends}),
        'totals': {
            'all_draws': sum(r['n'] for r in scene.rows) / f,
            'world_draws': sum(r['n'] for r in scene.rows if r.get('ctx') == 'world') / f,
            'opaque_main_view': total_opaque, 'opaque_wrote': total_wrote, 'opaque_not': total_opaque - total_wrote,
            'opaque_wrote_share': (total_wrote / total_opaque) if total_opaque else 0.0,
            'px_opaque': px_all, 'px_wrote': px_wrote, 'px_wrote_share': (px_wrote / px_all) if px_all else None,
        },
        'by_family': family_rows, 'not_covered': missing_rows[:top], 'not_covered_count': len(missing_rows), 'other_draws': other_rows,
        'warnings': warnings,
        'merge': merge_summary(scene),
    }


# ---- merge census (GBufferCensusMerge; docs/next/FD-BATCH-RESULTS.md) ----
# Per-entry costs measured in October (ENGINE-ANSWERS D8, this PC, native D3D9): ~1 us of the engine's per-entry setup and ~0.27 us
# per DrawIndexedPrimitive. A merged copy saves the draw call only if the setup still runs, both if the setup is skipped for copies.
US_DRAW = 0.27
US_SETUP = 1.0
DEFERRED_BLENDS = ('opaque', 'test')

# rule -> (description, universe, key fields, sorted)
#   universe 'all': every draw of the main list into the main view; 'pass': inside a game pass, DrawIndexedPrimitive;
#   'deferred': 'pass' + the object's first pass + opaque or alpha-tested (what remains drawn with lighting out: the add-on lamp,
#   highlight and texture passes are skipped under deferred)
RULES = [
    ('a', 'exact today: lamps and every constant but the vertex ones equal, game order', 'pass', ('p', 'gm', 'sh', 'st', 'tx', 'pc', 'pl'), False),
    ('b', 'lamps ignored (deferred records), game order', 'deferred', ('p', 'gm', 'sh', 'st', 'tx', 'pc'), False),
    ('c', 'lamps ignored + sorted inside a pass bucket', 'deferred', ('p', 'gm', 'sh', 'st', 'tx', 'pc'), True),
    ('d7-551', 'D7 level 1 for comparison: same pixel constants, sorted, all passes', 'pass', ('p', 'gm', 'sh', 'st', 'tx', 'pc', 'pl'), True),
    ('d7-870', 'D7 level 0 for comparison: pixel constants free, sorted, all passes', 'pass', ('p', 'gm', 'sh', 'st', 'tx'), True),
    ('ub', 'upper bound: same mesh, textures and state in any first pass (shaders and pass ID free)', 'deferred', ('gm', 'st', 'tx'), True),
]
SIZE_BINS = [(1, 1, '1'), (2, 3, '2-3'), (4, 7, '4-7'), (8, 15, '8-15'), (16, 63, '16-63'), (64, 1 << 30, '64+')]
VS_CATEGORIES = [((0, 4), 'transform c0-c4'), ((16, 16), 'eye c16'), ((14, 15), 'fog c14-c15'), ((25, 34), 'lamps c25-c34'),
                 ((44, 205), 'bones c44+'), ((17, 24), 'c17-c24'), ((5, 13), 'c5-c13')]


def vs_category(r):
    for (lo, hi), name in VS_CATEGORIES:
        if lo <= r <= hi:
            return name
    return 'other'


def merge_records(begin):
    recs = []
    for f in begin.get('_I', []):
        try:
            f['s'] = int(f.get('s', 0))
        except ValueError:
            f['s'] = 0
        recs.append(f)
    recs.sort(key=lambda r: r['s'])
    return recs


def in_universe(r, universe):
    if universe == 'all':
        return True
    if r.get('p', '-') == '-' or r.get('k') != '1':
        return False
    if universe == 'pass':
        return True
    return r.get('f') == '1' and r.get('b') in DEFERRED_BLENDS


def in_stream(r, universe):
    """The draws still drawn under the rule (they break a run in game order): add-on passes are gone under deferred."""
    if universe == 'deferred' and r.get('p', '-') != '-' and r.get('f') == '0':
        return False
    return True


def groups_of(recs, universe, fields, sort):
    """[[record, ...], ...]: the instanced draws the rule would make (a group of 1 is a plain draw)."""
    out = []
    if sort:
        idx = {}
        for r in recs:
            if not in_universe(r, universe):
                continue
            k = tuple(r.get(x) for x in fields)
            if k in idx:
                out[idx[k]].append(r)
            else:
                idx[k] = len(out)
                out.append([r])
        return out
    run, run_key = None, None
    for r in recs:
        if not in_stream(r, universe):
            continue
        if not in_universe(r, universe):
            run, run_key = None, None
            continue
        k = tuple(r.get(x) for x in fields)
        if run is not None and k == run_key:
            run.append(r)
        else:
            run, run_key = [r], k
            out.append(run)
    return out


def merge_frame(begin):
    recs = merge_records(begin)
    res = {'records': len(recs), 'pass_dip': sum(1 for r in recs if in_universe(r, 'pass')),
           'deferred': sum(1 for r in recs if in_universe(r, 'deferred')),
           'addon': sum(1 for r in recs if r.get('p', '-') != '-' and r.get('f') == '0'), 'rules': {}}
    for name, _, universe, fields, sort in RULES:
        g = groups_of(recs, universe, fields, sort)
        members = sum(len(x) for x in g)
        multi = [x for x in g if len(x) > 1]
        by_pass, by_family = defaultdict(int), defaultdict(int)
        for x in multi:
            by_pass[x[0].get('p', '-')] += len(x) - 1
            by_family[x[0].get('fam', '-')] += len(x) - 1
        sizes = {label: sum(len(x) for x in g if lo <= len(x) <= hi) for lo, hi, label in SIZE_BINS}
        # copies of one mesh (same model data) against one object drawn more than once (several partitions / the same md twice)
        distinct = sum(len({r.get('md') for r in x}) for x in multi)
        res['rules'][name] = {'considered': members, 'draws_after': len(g), 'saved': members - len(g), 'groups': len(multi),
                              'in_groups': sum(len(x) for x in multi), 'distinct_meshes_in_groups': distinct,
                              'sizes': sizes, 'by_pass': dict(by_pass), 'by_family': dict(by_family)}
    vary = {}
    for v in begin.get('_V', []):
        st = v.get('set', '?')
        if st == 'uses':
            vary['uses'] = {k: int(x) for k, x in v.items() if k != 'set' and x.lstrip('-').isdigit()}
            continue
        regs = {}
        if v.get('regs', '-') != '-':
            for item in v['regs'].split(','):
                r, _, c = item.partition(':')
                regs[int(r)] = int(c)
        vary[st] = {'groups': int(v.get('groups', 0) or 0), 'members': int(v.get('members', 0) or 0), 'regs': regs}
    res['vary'] = vary
    return res


def merge_summary(scene):
    frames = [merge_frame(b) for b in scene.begins if b.get('_I')]
    if not frames:
        return None
    f = float(len(frames))
    out = {'frames': len(frames), 'records': sum(x['records'] for x in frames) / f, 'pass_dip': sum(x['pass_dip'] for x in frames) / f,
           'deferred': sum(x['deferred'] for x in frames) / f, 'addon': sum(x['addon'] for x in frames) / f, 'rules': [], 'vary': {}}
    for name, desc, universe, fields, sort in RULES:
        rs = [x['rules'][name] for x in frames]
        avg = lambda k: sum(r[k] for r in rs) / f
        by_pass, by_family, sizes = defaultdict(float), defaultdict(float), defaultdict(float)
        for r in rs:
            for k, v in r['by_pass'].items():
                by_pass[k] += v / f
            for k, v in r['by_family'].items():
                by_family[k] += v / f
            for k, v in r['sizes'].items():
                sizes[k] += v / f
        saved = avg('saved')
        out['rules'].append({
            'rule': name, 'description': desc, 'considered': avg('considered'), 'draws_after': avg('draws_after'), 'saved': saved,
            'saved_share': saved / avg('considered') if avg('considered') else 0.0, 'groups': avg('groups'), 'in_groups': avg('in_groups'),
            'distinct_meshes_in_groups': avg('distinct_meshes_in_groups'),
            'ms_draw_only': saved * US_DRAW / 1000.0, 'ms_setup_skipped': saved * (US_DRAW + US_SETUP) / 1000.0,
            'sizes': {label: sizes.get(label, 0.0) for _, _, label in SIZE_BINS},
            'top_pass': sorted(by_pass.items(), key=lambda kv: -kv[1])[:8],
            'top_family': sorted(by_family.items(), key=lambda kv: -kv[1])[:8],
        })
    for st in ('deferred-vs', 'deferred-ps', 'free-ps'):
        vs = [x['vary'].get(st) for x in frames if x['vary'].get(st)]
        if not vs:
            continue
        groups = sum(v['groups'] for v in vs)
        regs = defaultdict(int)
        for v in vs:
            for r, c in v['regs'].items():
                regs[r] += c
        top = sorted(regs.items(), key=lambda kv: (-kv[1], kv[0]))
        entry = {'groups': groups / f, 'regs': [(r, c / groups if groups else 0.0) for r, c in top[:24]]}
        if st == 'deferred-vs':
            cats = defaultdict(float)
            for r, c in top:
                cats[vs_category(r)] = max(cats[vs_category(r)], c / groups if groups else 0.0)
            entry['categories'] = sorted(cats.items(), key=lambda kv: -kv[1])
            entry['regs_per_group'] = sum(regs.values()) / groups if groups else 0.0
        out['vary'][st] = entry
    uses = [x['vary'].get('uses') for x in frames if x['vary'].get('uses')]
    if uses:
        out['vary']['uses'] = {k: sum(u.get(k, 0) for u in uses) / f for k in uses[0]}
    return out


def render_merge(m):
    out = ['### Merge census: draws of the main opaque list that would merge into instanced draws (a frame, %d census frame(s))' % m['frames'], '']
    out.append('Main list draws into the main view %.1f; inside a game pass (DrawIndexedPrimitive) %.1f; of them first passes, opaque or '
               'alpha-tested (what stays drawn with lighting out) %.1f; add-on passes (gone under deferred pass skipping) %.1f.' % (
                   m['records'], m['pass_dip'], m['deferred'], m['addon']))
    out.append('')
    out.append('| rule | what | draws considered | after merging | saved | saved %% | groups > 1 | draws in them | distinct meshes in them | '
               'ms (draw call only) | ms (setup skipped) | top passes (saved) | top families (saved) |')
    out.append('|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---|')
    for r in m['rules']:
        out.append('| %s | %s | %.1f | %.1f | %.1f | %.1f | %.1f | %.1f | %.1f | %.3f | %.3f | %s | %s |' % (
            r['rule'], r['description'], r['considered'], r['draws_after'], r['saved'], 100.0 * r['saved_share'], r['groups'], r['in_groups'],
            r['distinct_meshes_in_groups'], r['ms_draw_only'], r['ms_setup_skipped'],
            ', '.join('%s %.0f' % kv for kv in r['top_pass']) or '-', ', '.join('%s %.0f' % kv for kv in r['top_family']) or '-'))
    out.append('')
    out.append('Group sizes (draws in groups of 1 / 2-3 / 4-7 / 8-15 / 16-63 / 64+), rule c: %s' % ' / '.join(
        '%.0f' % v for v in next(r for r in m['rules'] if r['rule'] == 'c')['sizes'].values()))
    v = m['vary']
    if 'deferred-vs' in v:
        d = v['deferred-vs']
        out.append('')
        out.append('Vertex registers that differ between copies (rule c groups: %.1f a frame, %.1f registers each): %s' % (
            d['groups'], d['regs_per_group'], ', '.join('c%d %.0f%%' % (r, 100 * c) for r, c in d['regs'][:16]) or '-'))
        out.append('By kind (share of groups): %s' % (', '.join('%s %.0f%%' % (k, 100 * c) for k, c in d['categories']) or '-'))
    if 'deferred-ps' in v:
        d = v['deferred-ps']
        out.append('Pixel registers that differ inside rule c groups (only lamp registers can, by construction): %s' % (
            ', '.join('c%d %.0f%%' % (r, 100 * c) for r, c in d['regs'][:16]) or 'none'))
    if 'free-ps' in v:
        d = v['free-ps']
        out.append('Pixel registers that differ between copies with free pixel constants (D7 level 0 groups: %.1f a frame): %s' % (
            d['groups'], ', '.join('c%d %.0f%%' % (r, 100 * c) for r, c in d['regs'][:16]) or 'none'))
    if 'uses' in v:
        u = v['uses']
        out.append('Register masks from: VS table %.0f / scan %.0f / indexed scan %.0f / unknown %.0f; PS table %.0f / scan %.0f / indexed %.0f / unknown %.0f '
                   '(an indexed scan or unknown counts every register from there on: groups can only split, never merge wrongly)' % tuple(
                       u.get(k, 0) for k in ('vs-table', 'vs-scan', 'vs-indexed', 'vs-unknown', 'ps-table', 'ps-scan', 'ps-indexed', 'ps-unknown')))
    out.append('')
    return '\n'.join(out)


def fmt(x, px=False):
    if px:
        return '%.0f' % x
    return '%.1f' % x


def render(summary):
    out = []
    t = summary['totals']
    out.append('## %s: %d census frame(s)%s' % (summary['scene'], summary['frames'], '' if summary['pixels_measured'] else ' (no pixel counts: GBufferCensus = 1)'))
    out.append('')
    for w in summary['warnings']:
        out.append('WARNING: ' + w)
    if summary['warnings']:
        out.append('')
    px = summary['pixels_measured']
    out.append('Totals a frame: %s draws, %s in the world context; opaque main-view %s, wrote the G-buffer %s (%.1f %%), not %s%s; game MSAA %s' % (
        fmt(t['all_draws']), fmt(t['world_draws']), fmt(t['opaque_main_view']), fmt(t['opaque_wrote']), 100.0 * t['opaque_wrote_share'], fmt(t['opaque_not']),
        ('; shaded pixels of those draws %s, %.1f %% by writers' % (fmt(t['px_opaque'], True), 100.0 * t['px_wrote_share'])) if px and t['px_wrote_share'] is not None else '',
        ','.join(summary['msaa'])))
    out.append('')
    out.append('### Opaque main-view draws by place and family (a frame)')
    out.append('')
    out.append('| where | family | draws | wrote | not | %s' % ('pixels | pixels by writers |' if px else ''))
    out.append('|---|---|---:|---:|---:|%s' % ('---:|---:|' if px else ''))
    for d in summary['by_family']:
        out.append('| %s | %s | %s | %s | %s | %s' % (d['where'], d['family'], fmt(d['draws']), fmt(d['wrote']), fmt(d['not']),
                                                       ('%s | %s |' % (fmt(d['px'], True), fmt(d['px_wrote'], True))) if px else ''))
    out.append('')
    out.append('### Not covered: opaque main-view draws without a normal record (%d groups%s)' % (
        summary['not_covered_count'], ', top %d shown' % len(summary['not_covered']) if summary['not_covered_count'] > len(summary['not_covered']) else ''))
    out.append('')
    out.append('| draws | %swhere | pass | pass name | pixel shader | family | blend | why |' % ('pixels | ' if px else ''))
    out.append('|---:|%s---|---|---|---|---|---|---|' % ('---:|' if px else ''))
    for d in summary['not_covered']:
        out.append('| %s | %s%s | %s | %s | %s | %s | %s | %s |' % (fmt(d['draws']), (fmt(d['px'], True) + ' | ') if px else '', d['where'], d['pass'],
                                                                   d['pass_name'], d['ps'], d['family'], d['blend'], d['why']))
    out.append('')
    out.append('### Every other draw (not opaque main-view), a frame')
    out.append('')
    out.append('| context | target | span | blend | G-buffer | draws |')
    out.append('|---|---|---|---|---|---:|')
    for d in summary['other_draws']:
        out.append('| %s | %s | %s | %s | %s | %s |' % (d['ctx'], d['tgt'], d['span'], d['blend'], d['gbuffer'], fmt(d['draws'])))
    out.append('')
    if summary.get('merge'):
        out.append(render_merge(summary['merge']))
    return '\n'.join(out)


def main(argv):
    paths, top, json_out = [], 40, None
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == '--top':
            top = int(argv[i + 1]); i += 2; continue
        if a == '--json':
            json_out = argv[i + 1]; i += 2; continue
        if a in ('-h', '--help'):
            print(__doc__); return 0
        paths.append(a); i += 1
    if not paths:
        print(__doc__); return 2
    lines = []
    for p in paths:
        with open(p, 'r', encoding='utf-8', errors='replace') as fh:
            lines.extend(fh.read().splitlines())
    blocks, dropped = read_blocks(lines)
    if not blocks:
        print('no complete GBC census block in %s (is [Main.Main.Misc] GBufferCensus on, and did a world frame draw?)' % ', '.join(paths))
        return 1
    scenes = collect(blocks)
    summaries = [summarize(scenes[k], top) for k in sorted(scenes)]
    print('# G-buffer census: %d block(s), %d scene(s)%s\n' % (len(blocks), len(scenes), ', %d incomplete block(s) left out' % dropped if dropped else ''))
    for s in summaries:
        print(render(s))
    if json_out:
        with open(json_out, 'w', encoding='utf-8') as fh:
            json.dump(summaries, fh, indent=1)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
