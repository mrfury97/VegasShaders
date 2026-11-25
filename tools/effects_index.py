"""effects_index.py <repo root> [check]: keeps the index table at the top of docs/EFFECTS.md in step with the effect folders
(2026-11-02 modular restructure; 2026-11-25 the per-effect README.md files and src/effects/INDEX.md became sections of docs/EFFECTS.md).
One `## <Name>` section per effect folder holds what the README said. Without `check` the table between the two marker comments is
rewritten; with `check` nothing is written and the exit code is 1 when the table is stale or the markers are missing. A `## <Name>` section
without a folder in src/effects is allowed (its row says "(no folder)"). Re-run after adding, removing or renaming an effect folder or section."""
import os, re, sys
root = os.path.abspath(sys.argv[1]); os.chdir(root)
check = len(sys.argv) > 2 and sys.argv[2] == 'check'
E = 'src/effects'
DOC = 'docs/EFFECTS.md'
BEGIN, END = '<!-- effects-index:begin -->', '<!-- effects-index:end -->'
defaults = open('resource/VegasShaders.defaults.toml', encoding='utf-8').read()
doc = open(DOC, encoding='utf-8').read().replace('\r\n', '\n')

# sections: `## <Name>` outside code fences, split at the next `## ` heading
sections, cur, fence = {}, None, False
for line in doc.split('\n'):
    if line.lstrip().startswith(('```', '~~~')):
        fence = not fence
    h = None if fence else re.match(r'^## (.+?)\s*$', line)
    if h:
        cur = h.group(1); sections[cur] = []
    elif cur is not None:
        sections[cur].append(line)

folders = sorted(d for d in os.listdir(E) if os.path.isdir(os.path.join(E, d)))
# a section without a folder is allowed (an effect that is documented but has no code of its own, e.g. PipboyFit); it gets a row
problems = []
folders = sorted(set(folders) | {s for s in sections if s != 'Index'})
rows = []
for name in folders:
    d = os.path.join(E, name)
    if not os.path.isdir(d): d = os.path.join(E, '.nonexistent')
    on = re.search(r'^\[_?Shaders\.' + re.escape(name) + r'\.Status\]\s*\nEnabled\s*=\s*(\w+)', defaults, re.M | re.I)
    shaders = 0
    if os.path.exists(os.path.join(d, 'SHADERS.txt')):
        shaders = sum(1 for l in open(os.path.join(d, 'SHADERS.txt'), encoding='utf-8') if l.strip() and not l.startswith('#'))
    nsec = 0
    if os.path.exists(os.path.join(d, 'defaults.toml')):
        nsec = sum(1 for l in open(os.path.join(d, 'defaults.toml'), encoding='utf-8') if l.startswith('['))
    purpose = ''
    if name in sections:
        txt = '\n'.join(sections[name])
        m = re.search(r'###\s*1\.?\s*Purpose\s*\n+(.+?)(?:\n\n|\n#)', txt, re.S)
        if m: purpose = ' '.join(m.group(1).split())[:160].replace('|', '/')
    code = ', '.join(sorted(f for f in os.listdir(d) if f.endswith(('.cpp', '.h')))) if os.path.isdir(d) else '(no folder)'
    link = f'[{name}](#{name.lower()})' if name in sections else f'[{name}](../{E}/{name}/)'
    rows.append(f'| {link} | {(on.group(1) if on else "-")} | {"yes" if name in sections else "-"} | {shaders} | {nsec} | {code} | {purpose} |')
table = ('| Effect | On by default | Doc | Shader files | Settings sections | C++ | Purpose |\n|---|---|---|---|---|---|---|\n'
         + '\n'.join(rows) + '\n')
a, b = doc.find(BEGIN), doc.find(END)
if a < 0 or b < a:
    sys.exit(f'{DOC}: the {BEGIN} / {END} markers are missing')
new = doc[:a + len(BEGIN)] + '\n' + table + doc[b:]
if check:
    if new != doc: problems.append('the index table in ' + DOC + ' is stale (run tools/effects_index.py <root>)')
    for p in problems: print('FAIL', p)
    if problems: sys.exit(1)
    print(len(rows), 'effects indexed, check passed')
else:
    for p in problems: print('WARN', p)
    with open(DOC, 'w', encoding='utf-8', newline='\n') as f: f.write(new)
    print(len(rows), 'effects indexed')
