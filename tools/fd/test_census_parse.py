"""Unit test of census_parse.py on a synthetic log. Run: py -3.11 -I tools/fd/test_census_parse.py"""
import importlib.util
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('census_parse', os.path.join(HERE, 'census_parse.py'))
cp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cp)

# Two census frames of one exterior scene (fork), one of an interior (native), one block cut off by the end of the log, and noise.
LOG = """\
12:00:01 GBufferCensus: on (mode 2: draws and pixel counts), one census frame every 10 s; log lines 'GBC' (tools/fd/census_parse.py)
12:00:05 GBC BEGIN v=1 serial=1 frame=300 rt=fork scene=WastelandNV/-2,-1 interior=0 pos=-7000,-3000,9000 w=1920 h=1080 mode=2
12:00:05 GBC D n=400 px=900000 pxn=400 ctx=world span=main pass=0FC pn=BSSM_ADT tgt=main ps=SLS2001.pso fam=Object col=PBR nvr=1 swap=0 blend=opaque zw=1 gb=NMA why=wrote
12:00:05 GBC D n=120 px=500000 pxn=120 ctx=world span=main pass=1F0 pn=BSSM_LAND tgt=main ps=SLS2100.pso fam=Terrain col=Terrain nvr=1 swap=0 blend=opaque zw=1 gb=NMA why=wrote
12:00:05 GBC D n=60 px=200000 pxn=60 ctx=world span=main pass=200 pn=BSSM_LANDLO_A tgt=main ps=SLS2102.pso fam=TerrainLOD col=Terrain nvr=1 swap=0 blend=opaque zw=1 gb=- why=notwriter
12:00:05 GBC D n=90 px=80000 pxn=90 ctx=world span=main pass=030 pn=BSSM_LEAVES_AD tgt=main ps=STLEAF2000.pso fam=own col=SpeedTree nvr=1 swap=0 blend=test zw=1 gb=- why=foliageidle
12:00:05 GBC D n=30 px=10000 pxn=30 ctx=world span=grass-early pass=25C pn=BSSM_GRASS_DIRONLY_LF tgt=main ps=GRASS23x000.pso fam=own col=Grass nvr=1 swap=0 blend=alpha zw=1 gb=- why=nospan
12:00:05 GBC D n=10 px=4000 pxn=10 ctx=world span=main pass=0FC pn=BSSM_ADT tgt=main ps=unnamed@0BADF00D fam=- col=- nvr=0 swap=0 blend=opaque zw=1 gb=- why=notwriter
12:00:05 GBC D n=50 px=-1 pxn=0 ctx=world span=none pass=- pn=- tgt=off ps=unnamed@01234567 fam=- col=- nvr=0 swap=0 blend=opaque zw=1 gb=- why=nospan
12:00:05 GBC D n=200 px=-1 pxn=0 ctx=refl span=main pass=0FC pn=BSSM_ADT tgt=off ps=SLS2001.pso fam=Object col=PBR nvr=1 swap=0 blend=opaque zw=1 gb=- why=nospan
12:00:05 GBC END serial=1 frame=300 draws=960 world=760 keys=8 passes=900 passdraws=910 queries=710 pxlate=0 pxfail=0 pxmiss=0 msaa=0 reinstalls=0 cn=1
some other log line
12:00:15 GBC BEGIN v=1 serial=2 frame=900 rt=fork scene=WastelandNV/-2,-1 interior=0 pos=-7010,-3000,9000 w=1920 h=1080 mode=2
12:00:15 GBC D n=420 px=920000 pxn=420 ctx=world span=main pass=0FC pn=BSSM_ADT tgt=main ps=SLS2001.pso fam=Object col=PBR nvr=1 swap=0 blend=opaque zw=1 gb=NMA why=wrote
12:00:15 GBC D n=60 px=220000 pxn=60 ctx=world span=main pass=200 pn=BSSM_LANDLO_A tgt=main ps=SLS2102.pso fam=TerrainLOD col=Terrain nvr=1 swap=0 blend=opaque zw=1 gb=- why=notwriter
12:00:15 GBC END serial=2 frame=900 draws=480 world=480 keys=2 passes=470 passdraws=480 queries=480 pxlate=0 pxfail=0 pxmiss=0 msaa=0 reinstalls=1 cn=1
12:01:00 GBC BEGIN v=1 serial=3 frame=4000 rt=native scene=Vault21 interior=1 pos=0,0,0 w=1920 h=1080 mode=1
12:01:00 GBC D n=300 px=-1 pxn=0 ctx=world span=main pass=0C3 pn=BSSM_ADT10 tgt=main ps=SLS2040.pso fam=Object col=PBR nvr=1 swap=0 blend=opaque zw=1 gb=NMA why=wrote
12:01:00 GBC D n=900 px=-1 pxn=0 ctx=world span=main pass=130 pn=BSSM_DIFFUSEPT tgt=main ps=SLS2046.pso fam=Object col=PBR nvr=1 swap=0 blend=add zw=0 gb=- why=refused
12:01:00 GBC D n=40 px=-1 pxn=0 ctx=world span=main pass=0C3 pn=BSSM_ADT10 tgt=main ps=SLS2040.pso fam=Object col=PBR nvr=1 swap=1 blend=opaque zw=1 gb=NMA why=wrote
12:01:00 GBC D n=7 px=-1 pxn=0 ctx=fp span=none pass=0FC pn=BSSM_ADT tgt=main ps=SLS2001.pso fam=Object col=PBR nvr=1 swap=0 blend=opaque zw=1 gb=NMA why=wrote
12:01:00 GBC END serial=3 frame=4000 draws=1247 world=1240 keys=4 passes=60 passdraws=5 queries=0 pxlate=0 pxfail=0 pxmiss=0 msaa=0 reinstalls=0 cn=1
12:02:00 GBC BEGIN v=1 serial=4 frame=5000 rt=native scene=Vault21 interior=1 pos=0,0,0 w=1920 h=1080 mode=1
12:02:00 GBC D n=300 px=-1 pxn=0 ctx=world span=main pass=0C3 pn=BSSM_ADT10 tgt=main ps=SLS2040.pso fam=Object col=PBR nvr=1 swap=0 blend=opaque zw=1 gb=NMA why=wrote
"""


# One merge-census block (GBufferCensusMerge = 1): rules a / b / c, the D7 levels and the upper bound worked out by hand in the test.
def _i(s, p, f, b, k, gm, sh, pc, pl, md='M1', fam='Object'):
    return ('12:03:00 GBC I s=%d p=%s f=%d nl=1 b=%s k=%d gm=%s sh=%s st=T1 tx=X1 vc=V%d pc=%s pl=%s ls=L%s md=%s fam=%s'
            % (s, p, f, b, k, gm, sh, s, pc, pl, pl, md, fam))


MERGE_LOG = '\n'.join([
    '12:03:00 GBC BEGIN v=1 serial=9 frame=9000 rt=native scene=Freeside interior=0 pos=0,0,0 w=1920 h=1080 mode=1 merge=1',
    '12:03:00 GBC D n=9 px=-1 pxn=0 ctx=world span=main pass=0FC pn=BSSM_ADT tgt=main ps=SLS2001.pso fam=Object col=PBR nvr=1 swap=0 blend=opaque zw=1 gb=NMA why=wrote',
    _i(1, '0FC', 1, 'opaque', 1, 'A', 'S', 'P', '1'),
    _i(2, '0FC', 1, 'opaque', 1, 'A', 'S', 'P', '1', md='M2'),
    _i(3, '0FC', 1, 'opaque', 1, 'A', 'S', 'P', '2', md='M3'),
    _i(4, '12C', 0, 'add', 1, 'A', 'S3', 'P', '2', md='M3'),
    _i(5, '0FC', 1, 'opaque', 1, 'A', 'S', 'P', '3', md='M4'),
    _i(6, '0FC', 1, 'opaque', 1, 'B', 'S', 'P', '1', md='M5'),
    _i(7, '0FC', 1, 'opaque', 1, 'A', 'S', 'P', '1', md='M6'),
    '12:03:00 GBC I s=8 p=- f=0 nl=0 b=opaque k=0 gm=C sh=S st=T1 tx=X1 vc=V8 pc=P pl=L ls=L md=00000000 fam=-',
    _i(9, '0FD', 1, 'opaque', 1, 'A', 'S2', 'Q', '1', md='M7', fam='Parallax'),
    '12:03:00 GBC V set=deferred-vs groups=1 members=5 regs=0:1,1:1,2:1,3:1,16:1',
    '12:03:00 GBC V set=deferred-ps groups=1 members=5 regs=3:1,19:1',
    '12:03:00 GBC V set=free-ps groups=1 members=5 regs=3:1',
    '12:03:00 GBC V set=uses records=9 vs-table=9 vs-scan=0 vs-indexed=0 vs-unknown=0 ps-table=8 ps-scan=1 ps-indexed=0 ps-unknown=0',
    '12:03:00 GBC END serial=9 frame=9000 draws=9 world=9 keys=1 passes=8 passdraws=8 queries=0 pxlate=0 pxfail=0 pxmiss=0 msaa=0 reinstalls=0 cn=1',
]) + '\n'


class MergeCensusTest(unittest.TestCase):
    def setUp(self):
        blocks, dropped = cp.read_blocks(MERGE_LOG.splitlines())
        self.assertEqual((len(blocks), dropped), (1, 0))
        self.assertEqual(len(blocks[0][1]), 1)  # the D row is read as before
        self.summary = cp.summarize(cp.collect(blocks)['Freeside [native]'])
        self.m = self.summary['merge']
        self.rules = {r['rule']: r for r in self.m['rules']}

    def test_universe(self):
        self.assertEqual(self.m['records'], 9)
        self.assertEqual(self.m['pass_dip'], 8)
        self.assertEqual(self.m['deferred'], 7)
        self.assertEqual(self.m['addon'], 1)

    def test_rules(self):
        expect = {  # rule: (considered, after, saved)
            'a': (8, 7, 1),       # only s1 + s2 are neighbours with equal lamps
            'b': (7, 4, 3),       # s1 s2 s3 (s4 skipped under deferred) s5; s8 (no pass) breaks the run before s9
            'c': (7, 3, 4),       # s1 s2 s3 s5 s7
            'd7-551': (8, 6, 2),  # s1 s2 s7 (lamps 1)
            'd7-870': (8, 4, 4),  # s1 s2 s3 s5 s7
            'ub': (7, 2, 5),      # + s9 (another pass and shader, same mesh, textures, state)
        }
        for name, (considered, after, saved) in expect.items():
            r = self.rules[name]
            self.assertEqual((r['considered'], r['draws_after'], r['saved']), (considered, after, saved), name)
        self.assertEqual(self.rules['c']['distinct_meshes_in_groups'], 5)
        self.assertEqual(dict(self.rules['c']['top_pass'])['0FC'], 4)
        self.assertAlmostEqual(self.rules['c']['ms_setup_skipped'], 4 * (cp.US_DRAW + cp.US_SETUP) / 1000.0)
        self.assertEqual(self.rules['c']['sizes']['4-7'], 5)

    def test_vary_and_render(self):
        v = self.m['vary']
        self.assertEqual(v['deferred-vs']['regs_per_group'], 5)
        cats = dict(v['deferred-vs']['categories'])
        self.assertEqual(cats['transform c0-c4'], 1.0)
        self.assertEqual(cats['eye c16'], 1.0)
        self.assertEqual(v['uses']['ps-scan'], 1)
        text = cp.render(self.summary)
        self.assertIn('### Merge census', text)
        self.assertIn('| c | lamps ignored + sorted inside a pass bucket | 7.0 | 3.0 | 4.0 |', text)

    def test_old_blocks_have_no_merge(self):
        blocks, _ = cp.read_blocks(LOG.splitlines())
        for sc in cp.collect(blocks).values():
            self.assertIsNone(cp.summarize(sc)['merge'])


class CensusParseTest(unittest.TestCase):
    def setUp(self):
        self.blocks, self.dropped = cp.read_blocks(LOG.splitlines())
        self.scenes = cp.collect(self.blocks)

    def test_blocks(self):
        self.assertEqual(len(self.blocks), 3)
        self.assertEqual(self.dropped, 1)  # the block the log ends in
        self.assertEqual(sorted(self.scenes), ['Vault21 [native]', 'WastelandNV/-2,-1 [fork]'])
        self.assertEqual(self.scenes['WastelandNV/-2,-1 [fork]'].frames, 2)

    def test_pass_groups(self):
        self.assertEqual(cp.pass_group('0FC', 'BSSM_ADT'), 'object-base')
        self.assertEqual(cp.pass_group('200', 'BSSM_LANDLO_A'), 'lod-land')
        self.assertEqual(cp.pass_group('1F0', 'BSSM_LAND'), 'land')
        self.assertEqual(cp.pass_group('030', 'BSSM_LEAVES_AD'), 'trees')
        self.assertEqual(cp.pass_group('130', 'BSSM_DIFFUSEPT'), 'object-lamp-pass')
        self.assertEqual(cp.pass_group('-', '-'), 'no-pass')
        self.assertEqual(cp.pass_group('2F0', '-'), 'pass-2F0')

    def test_exterior_summary(self):
        s = cp.summarize(self.scenes['WastelandNV/-2,-1 [fork]'])
        t = s['totals']
        # opaque main view: frame 1: 400 + 120 + 60 + 90 (test) + 10 = 680 (the grass blends, the other two are off-target / reflections)
        # frame 2: 420 + 60 = 480; mean 580
        self.assertAlmostEqual(t['opaque_main_view'], 580.0)
        self.assertAlmostEqual(t['opaque_wrote'], (400 + 120 + 420) / 2.0)
        self.assertAlmostEqual(t['opaque_not'], (60 + 90 + 10 + 60) / 2.0)
        self.assertTrue(s['pixels_measured'])
        self.assertAlmostEqual(t['px_opaque'], (900000 + 500000 + 200000 + 80000 + 4000 + 920000 + 220000) / 2.0)
        # not covered: sorted by draws: LOD land (60 a frame), leaves (45), the unnamed one (5)
        nc = s['not_covered']
        self.assertEqual([d['ps'] for d in nc], ['SLS2102.pso', 'STLEAF2000.pso', 'unnamed'])
        self.assertEqual(nc[0]['where'], 'main/lod-land')
        self.assertAlmostEqual(nc[0]['draws'], 60.0)
        self.assertAlmostEqual(nc[0]['px'], 210000.0)
        self.assertEqual(nc[1]['family'], 'own:SpeedTree')
        self.assertEqual(nc[1]['why'], 'foliageidle')
        fam = {(d['where'], d['family']): d for d in s['by_family']}
        self.assertAlmostEqual(fam[('main/object-base', 'Object')]['draws'], 410.0)
        self.assertAlmostEqual(fam[('main/object-base', 'Object')]['wrote'], 410.0)
        self.assertAlmostEqual(fam[('main/lod-land', 'TerrainLOD')]['not'], 60.0)
        others = {(d['ctx'], d['span'], d['blend']): d['draws'] for d in s['other_draws']}
        self.assertAlmostEqual(others[('refl', 'main', 'opaque')], 100.0)
        self.assertAlmostEqual(others[('world', 'grass-early', 'alpha')], 15.0)
        self.assertEqual(s['warnings'], [])

    def test_interior_summary_and_warning(self):
        s = cp.summarize(self.scenes['Vault21 [native]'])
        t = s['totals']
        self.assertFalse(s['pixels_measured'])
        self.assertAlmostEqual(t['opaque_main_view'], 340.0)  # the add-on lamp pass is blended, the first-person draw is not the world
        self.assertAlmostEqual(t['opaque_wrote_share'], 1.0)
        self.assertEqual(s['not_covered'], [])
        fam = {(d['where'], d['family']) for d in s['by_family']}
        self.assertIn(('main/object-base', 'Object(swap)'), fam)
        self.assertEqual(len(s['warnings']), 1)  # 60 passes, 5 draws inside: hooks lost
        self.assertIn('hooks lost', s['warnings'][0])

    def test_main_end_to_end(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = os.path.join(tmp, 'VegasShaders.log')
            js = os.path.join(tmp, 'out.json')
            with open(log, 'w', encoding='utf-8') as fh:
                fh.write(LOG)
            import io
            import contextlib
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = cp.main([log, '--json', js, '--top', '2'])
            self.assertEqual(rc, 0)
            text = buf.getvalue()
            self.assertIn('## WastelandNV/-2,-1 [fork]: 2 census frame(s)', text)
            self.assertIn('1 incomplete block(s) left out', text)
            self.assertIn('top 2 shown', text)
            self.assertTrue(os.path.exists(js))

    def test_no_blocks(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = os.path.join(tmp, 'empty.log')
            with open(log, 'w', encoding='utf-8') as fh:
                fh.write('nothing here\n')
            import io
            import contextlib
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(cp.main([log]), 1)


# 2026-11-14 (fd-lod, docs/next/FD-LOD-RESULTS.md): the Goodsprings native census of phase 0 as it should read with DeferredDistant on: the
# game's geometry group 0 (far land SLS2003, far objects SLS2000 / SLS2031, distant trees DISTTREE) drawn in the G-buffer span ("distant"):
# the far land and objects write it, the distant trees (the game's shader) stay "notwriter", water LOD as before.
DISTANT_LOG = """\
21:00:05 GBC BEGIN v=1 serial=1 frame=300 rt=native scene=WastelandNV/-18,0/Goodsprings interior=0 pos=0,0,0 w=1920 h=1080 mode=1
21:00:05 GBC D n=273 px=-1 pxn=0 ctx=world span=main pass=0FC pn=BSSM_ADT tgt=main ps=SLS2001.pso fam=Object col=PBR nvr=1 swap=0 blend=opaque zw=1 gb=NMA why=wrote
21:00:05 GBC D n=37 px=-1 pxn=0 ctx=world span=distant pass=0FE pn=BSSM_ADT_Mn tgt=main ps=SLS2003.pso fam=TerrainLOD col=Terrain nvr=1 swap=0 blend=opaque zw=1 gb=NMA why=wrote
21:00:05 GBC D n=24 px=-1 pxn=0 ctx=world span=distant pass=0FB pn=BSSM_ADT tgt=main ps=SLS2000.pso fam=Object col=PBR nvr=1 swap=0 blend=test zw=1 gb=NMA why=wrote
21:00:05 GBC D n=4 px=-1 pxn=0 ctx=world span=distant pass=0C9 pn=BSSM_ADT4 tgt=main ps=SLS2031.pso fam=Object col=PBR nvr=1 swap=0 blend=test zw=1 gb=NMA why=wrote
21:00:05 GBC D n=28 px=-1 pxn=0 ctx=world span=distant pass=236 pn=BSSM_DISTANT_TREE_BASE tgt=main ps=DISTTREE.pso fam=game col=- nvr=0 swap=0 blend=test zw=1 gb=- why=notwriter
21:00:05 GBC D n=3 px=-1 pxn=0 ctx=world span=main pass=23F pn=BSSM_WATER_LOD tgt=main ps=WATER033.pso fam=own col=Water nvr=1 swap=0 blend=opaque zw=1 gb=- why=notwriter
21:00:05 GBC END serial=1 frame=300 draws=369 world=369 keys=6 passes=369 passdraws=369 queries=0 pxlate=0 pxfail=0 pxmiss=0 msaa=0 reinstalls=0 cn=1
"""


class DistantSpanTest(unittest.TestCase):
    def test_distant_rows(self):
        blocks, dropped = cp.read_blocks(DISTANT_LOG.splitlines())
        self.assertEqual(dropped, 0)
        s = cp.summarize(cp.collect(blocks)['WastelandNV/-18,0/Goodsprings [native]'])
        fam = {(d['where'], d['family']): d for d in s['by_family']}
        self.assertAlmostEqual(fam[('distant/object-base', 'TerrainLOD')]['wrote'], 37.0)
        self.assertAlmostEqual(fam[('distant/object-base', 'Object')]['wrote'], 28.0)
        # not covered: only the distant trees (the game's shader, forward) and water LOD
        nc = s['not_covered']
        self.assertEqual([d['ps'] for d in nc], ['DISTTREE.pso', 'WATER033.pso'])
        self.assertEqual(nc[0]['where'], 'distant/sky')
        t = s['totals']
        self.assertAlmostEqual(t['opaque_wrote'], 273.0 + 37.0 + 24.0 + 4.0)
        self.assertAlmostEqual(t['opaque_not'], 31.0)


if __name__ == '__main__':
    unittest.main()
