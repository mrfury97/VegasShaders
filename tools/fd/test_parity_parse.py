"""Unit test of parity_parse.py on a synthetic log (the line format ParityTest.h writes).

Run: py -3.11 -I tools/fd/test_parity_parse.py
"""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import parity_parse as pp  # noqa: E402

R9 = lambda v: ",".join(f"{v:.3f}" for _ in range(9))  # noqa: E731


def pair(run, shot, m, n, point, name, mean, gt8, gt16, la=22.3, lb=22.0, regions=None):
    reg = regions or R9(gt16)
    return (f"[12:00:01 f{1000 + m}] PARITY v1 run={run} shot={shot} m={m}/{n} point={point} pair={name} mean={mean:.4f} gt8={gt8:.4f} "
            f"gt16={gt16:.4f} lumaA={la:.3f} lumaB={lb:.3f} r_mean={R9(mean)} r_gt8={R9(gt8)} r_gt16={reg}\n")


def summary(run, shot, m, n, point, readms=4.2, camera=0.0):
    return (f"[12:00:02 f2000] PARITY v1 run={run} shot={shot} m={m}/{n} point={point} kind=summary valid=1 "
            f"switch=Shaders.ShadowsInteriors.Deferred/Live A=false B=true size=960x540 format=A16B16G16R16F msaa=0 frames=64 "
            f"readms_max={readms:.2f} readms_mean=3.00 camera_moved={camera:.2f} missed=0 ab_gt16=1.0 floor_gt16=0.02 ab_gt8=2.0 floor_gt8=0.1\n")


def synthetic():
    run = "12-00-00"
    out = ["[12:00:00 f1] SETTING SHOTS 12-00-00: started, 2 shots\n",
           f"[12:00:00 f2] PARITY v1 run={run} shot=lucky38 kind=start switch=Shaders.ShadowsInteriors.Deferred/Live A=false B=true was=true "
           f"frames=64 count=3 | blocks A1 A2 B1 B2 of 64 frames\n"]
    ab = [1.10, 1.30, 1.20]
    for m in (1, 2, 3):
        for point, scale in (("world", 1.0), ("final", 1.5)):
            out.append(pair(run, "lucky38", m, 3, point, "AA", 0.05, 0.10, 0.02 * m))
            regions = "0.100,0.200,0.300,0.400,5.000,0.600,0.700,0.800,0.900" if point == "world" else None
            out.append(pair(run, "lucky38", m, 3, point, "AB", 0.40, 2.3, ab[m - 1] * scale, regions=regions))
            out.append(pair(run, "lucky38", m, 3, point, "BB", 0.05, 0.10, 0.03))
            out.append(summary(run, "lucky38", m, 3, point, readms=3.0 + m))
    out.append(f"[12:00:09 f3000] PARITY v1 run={run} shot=lucky38 kind=done measurements=3 | the switch is back at its own value\n")
    out.append("[12:00:10 f3001] some other line PARITY without the version\n")
    out.append(f"[12:00:11 f3002] PARITY v1 run=12-01-00 shot=vault21 kind=start switch=Shaders.ShadowsInteriors.Deferred/Live A=false B=true was=true frames=64 count=3 | x\n")
    out.append("[12:00:12 f3003] PARITY v1 run=12-01-00 shot=vault21 m=1/3 kind=stopped | the player changed place; the switch is back at its own value\n")
    out.append("[12:00:13 f3004] PARITY v1 run=12-02-00 shot=bad kind=refused | no setting [Shaders.Nope] Live\n")
    return out


class ParityParseTest(unittest.TestCase):
    def setUp(self):
        self.lines = synthetic()

    def test_parse_line(self):
        f, note = pp.parse_line(self.lines[1])
        self.assertEqual(f["kind"], "start")
        self.assertEqual(f["switch"], "Shaders.ShadowsInteriors.Deferred/Live")
        self.assertTrue(note.startswith("blocks"))
        self.assertIsNone(pp.parse_line("PARITY without version"))

    def test_tables(self):
        tables, notes = pp.build(self.lines)
        names = [(t["shot"], t["point"]) for t in tables]
        self.assertEqual(names, [("lucky38", "world"), ("lucky38", "final")])
        world = tables[0]
        self.assertEqual(len(world["rows"]), 3)
        med = world["median"]
        self.assertAlmostEqual(med["ab_gt16"], 1.20)
        self.assertAlmostEqual(med["floor_gt16"], 0.04)          # max(AA 0.04, BB 0.03) at m=2 is the median of 0.03, 0.04, 0.06
        self.assertAlmostEqual(med["readms"], 6.0)               # the largest read of the run
        self.assertEqual(med["size"], "960x540")
        self.assertAlmostEqual(med["regions_gt16"][4], 5.0)
        self.assertAlmostEqual(world["rows"][0]["excess_gt16"], 1.10 - 0.03)
        self.assertEqual(world["switch"], "Shaders.ShadowsInteriors.Deferred/Live")
        final = tables[1]
        self.assertAlmostEqual(final["median"]["ab_gt16"], 1.8)
        self.assertEqual(len(notes), 2)
        self.assertIn("vault21", notes[0])
        self.assertIn("stopped", notes[0])
        self.assertIn("refused", notes[1])

    def test_point_filter_and_render(self):
        tables, notes = pp.build(self.lines, "world")
        self.assertEqual(len(tables), 1)
        text = pp.render(tables, notes, band=1.8, regions=True)
        self.assertIn("### lucky38 (world", text)
        self.assertIn("| median | 0.400 | 2.300 | 1.200 |", text)
        self.assertIn("**WITHIN**", text)
        self.assertIn("5.00", text)
        self.assertIn("Not measured:", text)
        over = pp.render(tables, notes, band=1.0)
        self.assertIn("**OVER**", over)

    def test_cli(self):
        with tempfile.NamedTemporaryFile("w", suffix=".log", delete=False, encoding="utf-8") as fh:
            fh.writelines(self.lines)
            path = fh.name
        try:
            self.assertEqual(pp.main([path, "--json"]), 0)
            with open(path, "w", encoding="utf-8") as fh:
                fh.write("nothing here\n")
            self.assertEqual(pp.main([path]), 2)
        finally:
            os.unlink(path)


if __name__ == "__main__":
    unittest.main()
