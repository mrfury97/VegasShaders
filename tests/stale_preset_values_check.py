"""stale_preset_values_check.py [repo root]   (UNOFFICIAL 2026-11-18)

Checks src/core/StalePresetValues.h's table against SettingManager.cpp's UpdateOldDefaults: every key a migration step moves through
replace() / replaceBool() / toTrue() / setBool() must be either in the table (with the same old value) or in LOOK_OR_CHOICE below
(the preset's look and its effect switches, which presets keep on purpose). A new migration step that moves a fix fails this check
until its key is added to the table. Steps written out by hand (saved->contains("Key") blocks) are not parsed: keep them in step by eye.
Run with Python 3.11+ (py -3.11). exit 0 = consistent
"""
import os, re, sys

root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..')
sm = open(os.path.join(root, 'src/core/SettingMigrations.cpp'), encoding='utf-8', errors='replace').read()
hdr = open(os.path.join(root, 'src/core/StalePresetValues.h'), encoding='utf-8', errors='replace').read()

body = sm[sm.index('void SettingManager::Configuration::UpdateOldDefaults()'):]
body = body[:body.index('\n}\n')]

table = {}
for sec, key, val in re.findall(r'\{ "([^"]+)", "([^"]+)", ([0-9.]+)f, \d+(?:, true)? \}', hdr):
    table[(sec.lower(), key.lower())] = float(val)

GONE = {
    ('shaders.shadowsexteriors.shadowmaps', 'mode'),   # 70's key is no longer a setting (ignored by the build itself)
    ('shaders.pbr.linear', 'linearscene'),
    ('main.main.quality', 'materialbuffer'),
    ('main.main.quality', 'albedobuffer'),
    ('shaders.shadowsinteriors.atlas', 'enabled'),
    ('shaders.shadowsinteriors.deferred', 'enabled'),
    ('main.main.quality', 'deferredrenderer'),
    ('shaders.taa.fsr', 'version'),
}

LOOK_OR_CHOICE = {
    ('shaders.ambientlight.status', 'enabled'),            # 68: an effect switch
    ('shaders.terrain.rain', 'lightingscale'), ('shaders.pbr.rain', 'lightingscale'),  # 79: the look
    # 98 (2026-11-24): the dynamic-reflections upgrade's look (PolishedRoughness, a fix, is in the table)
    ('shaders.pbr.metal', 'roughness'), ('shaders.pbr.envmaps', 'lighting'), ('shaders.pbr.envmaps', 'strength'),
    ('shaders.pbr.envmaps', 'withmetal'), ('shaders.reflections.main', 'cubecapture'), ('shaders.reflections.main', 'cubefade'),
    ('shaders.ambientlight.main', 'exteriorbalance'),
    # 109 (2026-11-27 upstream 1.1.5 merge): defaults upstream changed without a step; tunings, not fixes
    ('shaders.shadowsexteriors.shadowmaps', 'casterfademin'), ('main.main.raytracing', 'arenamb'), ('main.main.raytracing', 'actorrebuildinterval'),
}

moved = []
for sec, key, old in re.findall(r'replace\("([^"]+)", "([^"]+)", ([0-9.]+),', body):
    moved.append((sec, key, float(old)))
for sec, key, old in re.findall(r'replaceBool\("([^"]+)", "([^"]+)", (true|false),', body):
    moved.append((sec, key, 1.0 if old == 'true' else 0.0))
for sec, key in re.findall(r'(?:toTrue|setBool)\("([^"]+)", "([^"]+)"\)', body):
    moved.append((sec, key, 0.0))
# 80's look keys go through replace(k.section, ...) from a table: the look, never in ours
if 'replace(k.section' not in body:
    print('note: step 80 no longer uses its look table; check this script')

bad = 0
for sec, key, old in moved:
    k = (sec.lower(), key.lower())
    if k in LOOK_OR_CHOICE or k in GONE:
        continue
    if k not in table:
        print(f'MISSING  [{sec}] {key} (old {old:g}) is moved by UpdateOldDefaults but not in StalePresetValues.h')
        bad += 1
    elif abs(table[k] - old) > 1e-4 * max(1.0, abs(old)):
        print(f'MISMATCH [{sec}] {key}: migration old value {old:g}, table {table[k]:g}')
        bad += 1
# every key in the table must still be a setting of this build (a dead entry logs a misleading line)
try:
    import tomllib
    d = tomllib.loads(open(os.path.join(root, 'resource/VegasShaders.defaults.toml'), encoding='utf-8').read())
    tab = hdr[hdr.index('kOldDefaults[]'):hdr.index('kRenamed[]')]
    for sec, key in re.findall(r'\{ "([^"]+)", "([^"]+)", [0-9.]+f', tab):
        t = d
        for part in ('_' + sec).split('.'):
            t = t.get(part, {}) if isinstance(t, dict) else {}
        if not (isinstance(t, dict) and key in t):
            print(f'GONE     [{sec}] {key} is in StalePresetValues.h but not a setting of this build')
            bad += 1
except ImportError:
    print('note: Python 3.11+ (tomllib) needed for the existence check; skipped')
print(f'{len(moved)} migration entries parsed, {len(table)} in the table, {bad} problems')
sys.exit(1 if bad else 0)
