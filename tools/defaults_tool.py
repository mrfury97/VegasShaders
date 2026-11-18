"""defaults_tool.py split|merge|check <repo root>   (2026-11-02, modular restructure)

The one settings defaults file (resource/VegasShaders.defaults.toml) is assembled from per-owner fragments:
  - each effect / shader collection owns its [Shaders.<Name>.*] sections:  src/effects/<Name>/defaults.toml
    (until the effect has its folder: resource/defaults/<Name>.toml)
  - everything else ([Main.*], the file header):                           resource/defaults/Core.toml
  - resource/defaults/ORDER.txt lists every section in file order with its owner, so the merge rebuilds the file in exactly the
    original order: a fragment never decides where its sections go.

split: one-time, from the current defaults file -> fragments + ORDER.txt.
merge: fragments + ORDER.txt -> the defaults file (the build runs this; the result is committed too, so nothing else changes).
       Fail safe: everything is validated first; the file is written via a temp file and replaced only when the whole merge succeeded,
       so an error never leaves it empty or half written. The merged file is always CRLF (as committed and shipped), whatever line
       ends the fragments were saved with.
check: merge into memory and compare with the committed defaults file byte for byte (exit 1 on a difference).
"""
import os, re, sys

DEFAULTS = 'resource/VegasShaders.defaults.toml'
FRAG_DIR = 'resource/defaults'
SECTION = re.compile(r'^\[_?([A-Za-z0-9]+)\.([A-Za-z0-9]+)(?:\.[^\]]*)?\]\s*$')

def owner_of(header_line):
    m = SECTION.match(header_line)
    if not m: return 'Core'
    top, name = m.group(1), m.group(2)
    return name if top == 'Shaders' else 'Core'

def fragment_path(root, owner):
    folder = os.path.join(root, 'src', 'effects', owner)
    if owner != 'Core' and os.path.isdir(folder):
        return os.path.join(folder, 'defaults.toml')
    return os.path.join(root, FRAG_DIR, owner + '.toml')

def read_raw(p):
    return open(p, 'rb').read().decode('utf-8')

def blocks(text):
    """[(header line or None for the preamble, block text)] keeping every byte (line endings included)."""
    lines = text.splitlines(keepends=True)
    out, cur_head, cur = [], None, []
    for l in lines:
        if l.startswith('['):
            out.append((cur_head, ''.join(cur))); cur_head, cur = l.rstrip('\r\n'), [l]
        else:
            cur.append(l)
    out.append((cur_head, ''.join(cur)))
    return out

def split(root):
    text = read_raw(os.path.join(root, DEFAULTS))
    frags, order = {}, []
    for head, body in blocks(text):
        key = head if head is not None else '<preamble>'
        own = 'Core' if head is None else owner_of(head)
        frags.setdefault(own, []).append(body)
        order.append(f'{own}\t{key}')
    os.makedirs(os.path.join(root, FRAG_DIR), exist_ok=True)
    for own, parts in frags.items():
        p = fragment_path(root, own)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, 'wb').write(''.join(parts).encode('utf-8'))
    open(os.path.join(root, FRAG_DIR, 'ORDER.txt'), 'wb').write(('\n'.join(order) + '\n').encode('utf-8'))
    print(f'split into {len(frags)} fragments, {len(order)} blocks')

def merged(root):
    order = [l.rstrip('\r\n').split('\t', 1) for l in open(os.path.join(root, FRAG_DIR, 'ORDER.txt'), encoding='utf-8') if l.strip()]
    pool = {}
    for own in sorted({o for o, _ in order}):
        p = fragment_path(root, own)
        pool[own] = {(h if h is not None else '<preamble>'): b for h, b in blocks(read_raw(p))}
        pool[own].pop('<preamble>', None) if own != 'Core' and pool[own].get('<preamble>', '') == '' else None
    out = []
    for own, key in order:
        if key not in pool[own]: sys.exit(f'missing section {key} in fragment {own}')
        out.append(pool[own].pop(key))
    for own, rest in pool.items():
        extra = [k for k, v in rest.items() if k != '<preamble>' or v]
        if extra: sys.exit(f'fragment {own} has sections ORDER.txt does not list: {extra[:5]} (add them to ORDER.txt)')
    return re.sub(r'\r?\n', '\r\n', ''.join(out))  # CRLF like the committed file, whatever the fragments use

if __name__ == '__main__':
    cmd, root = sys.argv[1], sys.argv[2]
    if cmd == 'split': split(root)
    elif cmd == 'merge':
        data = merged(root).encode('utf-8')  # exits on any error before anything is written
        dst = os.path.join(root, DEFAULTS); tmp = dst + '.tmp'
        try:
            with open(tmp, 'wb') as f:
                f.write(data); f.flush(); os.fsync(f.fileno())
            os.replace(tmp, dst)
        finally:
            if os.path.exists(tmp): os.remove(tmp)
        print('merged', DEFAULTS)
    elif cmd == 'check':
        same = merged(root) == read_raw(os.path.join(root, DEFAULTS))
        print('defaults file matches its fragments' if same else 'DIFFERENT: run merge (or fix ORDER.txt / the fragments)'); sys.exit(0 if same else 1)
