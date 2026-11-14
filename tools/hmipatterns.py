#!/usr/bin/env python3
"""Writes the HmiMain3D search patterns of carplay-advanced.sh (the hmi_patterns block) from the known builds.

    python3 tools/hmipatterns.py <HmiMain3D 006_017_006> <HmiMain3D 1703 Build 139> [--check <other HmiMain3D> ...] [--write]

For maintainers only: it needs the plain ARM ELF of both known builds (the car's /opt/hmi/bin/zipped/HmiMain3D read through the
ziploader, or the SWFL's opt/hmi/ID5/bin/HmiMain3D). The block it writes holds no firmware: each anchor is a short window of
instruction words with the build-dependent parts masked out ('.' = any hex digit), so it finds the same code in another build:
  * branch targets (B/BL), pc-relative loads, MOVW/MOVT immediates and pc-relative ADD/SUB are masked (they move between builds);
  * a word that differs between the two known builds in any other way is masked completely;
  * the words the installer reads values from (CTouchData offsets) keep only their opcode: the offset itself is masked.
Each window grows from the anchor until it matches exactly one place in each known build, and it always ends in a word without a
mask, which the installer's scanner (awk) uses as its lookup key. The scanner's own rules (which value comes from which word, see
hmi_scan in carplay-advanced.sh) must agree with the RULES below.

--check prints what the patterns find in other builds (no values are known there, so nothing is compared).
--write replaces the block between "hmi_patterns() {" and its EOF in carplay-advanced.sh; without it the block is printed.
"""
import json, os, re, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SCRIPT = os.path.join(ROOT, 'carplay-advanced.sh')


def text_of(path):
    data = open(path, 'rb').read()
    if data[:4] != b'\x7fELF' or data[4] != 1:
        raise SystemExit(path + ': not an ELF32 (read the zipped HmiMain3D through the ziploader)')
    phoff, = struct.unpack_from('<I', data, 0x1c)
    phentsize, phnum = struct.unpack_from('<HH', data, 0x2a)
    for i in range(phnum):
        p_type, p_off, p_vaddr, _, p_filesz, _, p_flags, _ = struct.unpack_from('<8I', data, phoff + i * phentsize)
        if p_type == 1 and p_flags & 1:            # PT_LOAD, executable
            return p_vaddr, data[p_off:p_off + p_filesz]
    raise SystemExit(path + ': no executable segment')


def wild_mask(w):
    """Bytes (little-endian order) of an ARM word that move between builds: 1 = any."""
    if (w >> 25) & 7 == 5:                                       # B / BL / BLX imm24
        return (1, 1, 1, 0)
    if (w & 0x0e5f0000) == 0x041f0000:                           # LDR/STR rX, [pc, #imm12]
        return (1, 1, 0, 0)
    if (w & 0x0e5f00f0) == 0x004f00b0 or (w & 0x0e5f00d0) == 0x004f00d0:   # LDRH/LDRD [pc, #imm8]
        return (1, 1, 0, 0)
    if (w & 0x0ff00000) in (0x03000000, 0x03400000):             # MOVW / MOVT imm16
        return (1, 1, 0, 1)
    if (w & 0x0fef0000) == 0x028f0000 or (w & 0x0fef0000) == 0x024f0000:   # ADD/SUB rX, pc, #imm
        return (1, 1, 0, 0)
    if (w & 0x0e5f0000) == 0x0d1f0000:                           # VLDR [pc, #imm8]
        return (1, 0, 0, 0)
    return (0, 0, 0, 0)


def token(words, field=False):
    """The pattern token for one position: 8 hex digits (big-endian, as od -tx4 prints the word), '.' for a masked digit."""
    w = words[0]
    if field:                                                    # a value is read from it: keep the opcode, mask the imm12
        return '%05x...' % (w >> 12)
    if any(x is not None and x != w for x in words[1:]):
        return '........'                                        # differs between the known builds: any word
    m = wild_mask(w)
    for x in words[1:]:
        if x is not None:
            m = tuple(a | b for a, b in zip(m, wild_mask(x)))
    h = '%08x' % w
    return ''.join('..' if m[3 - k] else h[2 * k:2 * k + 2] for k in range(4))


def regex(tokens):
    parts = []
    for t in tokens:
        w = t.replace('.', '0')
        b = bytes.fromhex(w)[::-1]                               # little-endian in the file
        for j in range(4):
            hexpair = t[6 - 2 * j:8 - 2 * j]
            if '.' in hexpair:                                   # a masked byte (a field's half byte counts as masked)
                parts.append(b'.')
            else:
                parts.append(re.escape(b[j:j + 1]))
    return re.compile(b''.join(parts), re.S)


def hits(rx, text):
    return [m.start() for m in rx.finditer(text) if m.start() % 4 == 0]


def word_at(base, text, addr):
    o = addr - base
    return struct.unpack_from('<I', text, o)[0] if 0 <= o <= len(text) - 4 else None


# anchors: name -> (address in 006_017_006, address in 1703 Build 139 or None, words the window must cover (offsets in words from
# the anchor), words values are read from (opcode kept, imm12 masked)). The tilt words are grouped: in both builds they sit at the
# same distances from tA and tB (t19 is further away by a different amount), and tB's window covers the targets of the branches the
# patch writes inside it (up to +0xa0), so their distances are checked too.
def anchors():
    t6 = json.load(open(os.path.join(ROOT, 'src', 'tilt_006.json')))['words']
    t7 = json.load(open(os.path.join(ROOT, 'src', 'tilt_1703.json')))['words']
    a = {
        'touch':   (0x43c02c, 0x8f55cc, range(0, 11), (10,)),   # handleTouchInput: prologue; +10 ldr r3, [r3, #tdOrigin]
        'tdfirst': (0x43c16c, 0x8f570c, (0,), (0,)),            # ldr sb, [r3, #tdFirst]
        'tdend':   (0x43c41c, 0x8f59bc, (0,), (0,)),            # ldr r3, [r3, #tdEnd]
        'ctl':     (0x43d6d4, 0x8f6148, (-14, -10, 0), ()),     # add #0x4e00 / add #0x60 (proxy), bl controller send
        'play':    (0x7244fc, 0xc7b520, (-2, -1, 0), ()),       # add #0x5200 / add #0xb0 (proxy), bl track send
        'tA':      (int(t6[0]['addr'], 16), int(t7[0]['addr'], 16), range(0, 3), ()),
        'tB':      (int(t6[3]['addr'], 16), int(t7[3]['addr'], 16), range(0, 41), ()),
        'tC':      (int(t6[18]['addr'], 16), int(t7[18]['addr'], 16), range(-3, 5), ()),   # +4: b tB (BRANCH_TO)
    }
    for i, (addr, _orig, _new) in enumerate(FS_006):
        a['f%d' % (i + 1)] = (addr, None, (0,), ())
    return a


def patches():
    """patch lines: group anchor offset new mode. mode '=' writes new as it is; 'imm24' keeps the found word's branch offset (only the
    condition changes); 'b:<anchor>+<offset>' is a branch to that address. orig is always the word the scanner found."""
    t6 = json.load(open(os.path.join(ROOT, 'src', 'tilt_006.json')))['words']
    t7 = json.load(open(os.path.join(ROOT, 'src', 'tilt_1703.json')))['words']
    groups = (('tA', 0, 3), ('tB', 3, 18), ('tC', 18, 19))
    out = []
    for g, i0, i1 in groups:
        for k in range(i0, i1):
            x, y = t6[k], t7[k]
            off6 = int(x['addr'], 16) - int(t6[i0]['addr'], 16)
            off7 = int(y['addr'], 16) - int(t7[i0]['addr'], 16)
            assert off6 == off7, x
            o6, n6, o7, n7 = (int(v, 16) for v in (x['orig'], x['new'], y['orig'], y['new']))
            if n6 == n7:
                mode, new = '=', n6
                if (n6 >> 25) & 7 == 5:                       # a branch written as it is: its target must be inside tB's window
                    assert g == 'tB', x
            elif (n6 >> 24) == (n7 >> 24) and n6 & 0xffffff == o6 & 0xffffff and n7 & 0xffffff == o7 & 0xffffff:
                mode, new = 'imm24', n6 & 0xff000000
            else:                                             # a branch to a place that moved: find it as tB + offset
                assert (n6 >> 25) & 7 == 5 and (n6 >> 24) == (n7 >> 24), x
                t6a = branch_target(int(x['addr'], 16), n6) - int(t6[3]['addr'], 16)
                t7a = branch_target(int(y['addr'], 16), n7) - int(t7[3]['addr'], 16)
                assert t6a == t7a, x
                mode, new = 'b:tB+%#x' % t6a, n6 & 0xff000000
            out.append('patch tilt %s %#x %08x %s' % (g, off6, new, mode))
    for i, (_addr, _orig, new) in enumerate(FS_006):
        out.append('patch fs f%d 0 %08x =' % (i + 1, new))
    return out


def aligned_count(text, tok):
    b = bytes.fromhex(tok)[::-1]
    n, i = 0, text.find(b)
    while i >= 0:
        n += i % 4 == 0
        i = text.find(b, i + 1)
    return n


def branch_target(addr, w):
    imm = w & 0xffffff
    if imm & 0x800000:
        imm -= 1 << 24
    return addr + 8 + 4 * imm


# kFsCode in src/cpm.cpp (006_017_006 only): address, original, new
FS_006 = ((0x43bbe0, 0xe5d00108, 0xe3a00001), (0x43bbfc, 0xe5d00108, 0xe3a00001), (0x43c9ec, 0xe5d43108, 0xe3a03001),
          (0x43d1f0, 0xe5d43108, 0xe3a03001), (0x43e4d0, 0xe5d43108, 0xe3a03001), (0x43e974, 0xe5d43108, 0xe3a03001),
          (0x43c0c0, 0xe12fff33, 0xe3a00001))
# anchor -> (word offset, anchor): the branch at that word must go to the other anchor (a window that alone is not unique)
BRANCH_TO = {'tC': (4, 'tB')}
MIN_FIXED = 8                                                    # words without a mask in every window


def make(name, a6, a7, need, fields, b6, x6, b7, x7, found):
    lo, hi = min(need), max(need)
    before, after = -lo, hi
    bt = BRANCH_TO.get(name)
    for grow in range(0, 400):
        toks = []
        for k in range(-before, after + 1):
            w6 = word_at(b6, x6, a6 + 4 * k)
            w7 = word_at(b7, x7, a7 + 4 * k) if a7 is not None else None
            toks.append(token([w6, w7], field=k in fields))
        if sum('.' not in t for t in toks) >= MIN_FIXED:
            rx = regex(toks)
            h6 = keep(hits(rx, x6), b6, x6, before, bt, found, 0)
            h7 = keep(hits(rx, x7), b7, x7, before, bt, found, 1) if a7 is not None else [None]
            if len(h6) == 1 and len(h7) == 1:
                assert b6 + h6[0] + 4 * before == a6, name
                assert a7 is None or b7 + h7[0] + 4 * before == a7, name
                return before, toks
        if grow % 2:
            before += 1
        else:
            after += 1
    raise SystemExit('%s: no unique window' % name)


def keep(offs, base, text, before, bt, found, which):
    """The hits that satisfy the anchor's BRANCH_TO constraint (all of them without one)."""
    if not bt:
        return offs
    k, other = bt
    out = []
    for o in offs:
        a = base + o + 4 * (before + k)
        if branch_target(a, word_at(base, text, a)) == found[other][which]:
            out.append(o)
    return out


# What the scanner must find in the known builds: kBuilds in src/cpm.cpp (with the tilt words of src/tilt_*.json and kFsCode).
KNOWN = {
    '006_017_006': dict(touchFn=0x43c02c, ctlSetFn=0x1fd0d8c, ctlProxy=0x4e60, playFn=0x1fd0cec, playObj=0x52b0,
                        tdFirst=4, tdEnd=8, tdTime=0x14, tdOrigin=0x18, pbw=1, pbwBmw=1, ini=1, tilt='tilt_006.json', fs=True),
    '1703 Build 139': dict(touchFn=0x8f55cc, ctlSetFn=0x20d9e50, ctlProxy=0x4e60, playFn=0x20d9db0, playObj=0x52b0,
                           tdFirst=8, tdEnd=0xc, tdTime=0x18, tdOrigin=0x1c, pbw=2, pbwBmw=2, ini=1, tilt='tilt_1703.json', fs=False),
}
VALUES = ('touchFn', 'ctlSetFn', 'ctlProxy', 'playFn', 'playObj', 'tdFirst', 'tdEnd', 'tdTime', 'tdOrigin', 'pbw', 'pbwBmw', 'ini')


def expected(build):
    """The scanner's output for a known build, without its first line (hmi <version>): what its scan hash covers."""
    k = KNOWN[build]
    out = ['%s %x' % (n, k[n]) for n in VALUES]
    for w in json.load(open(os.path.join(ROOT, 'src', k['tilt'])))['words']:
        out.append('tilt %x %s %s' % (int(w['addr'], 16), w['orig'], w['new']))
    if k['fs']:
        out += ['fs %x %08x %08x' % f for f in FS_006]
    return '\n'.join(out) + '\n'


def main(argv):
    if argv[:1] == ['--expect']:                   # the expected scan of a known build (tests; the hash goes into supported_table)
        sys.stdout.write(expected(argv[1]))
        return
    write = '--write' in argv
    argv = [x for x in argv if x != '--write']
    checks = []
    if '--check' in argv:
        i = argv.index('--check')
        checks, argv = argv[i + 1:], argv[:i]
    if len(argv) != 2:
        raise SystemExit(__doc__)
    b6, x6 = text_of(argv[0])
    b7, x7 = text_of(argv[1])
    lines = ['# find <name> <index of the anchor word> <index of the lookup word> <words> / patch <group> <anchor> <offset> <new> <mode> (generated by tools/hmipatterns.py from 006_017_006 and 1703 Build 139; do not edit)']
    an = anchors()
    found = {n: (v[0], v[1]) for n, v in an.items()}
    for name, (a6, a7, need, fields) in an.items():
        idx, toks = make(name, a6, a7, need, fields, b6, x6, b7, x7, found)
        bt = BRANCH_TO.get(name)
        key = min((k for k, t in enumerate(toks) if '.' not in t), key=lambda k: aligned_count(x6, toks[k]))
        lines.append('find %s %d %d %s%s' % (name, idx, key, ' '.join(toks), ' to:%d:%s' % (idx + bt[0], bt[1]) if bt else ''))
    lines += patches()
    if fs_in(lines, b7, x7):
        raise SystemExit('the fullscreen-flag patterns also match in 1703 (which has no flag)')
    for path in checks:
        b, x = text_of(path)
        found = {}
        for line in lines[1:]:
            if not line.startswith('find '):
                continue
            _f, name, idx, _key, *toks = line.split()
            toks = [t for t in toks if not t.startswith('to:')]
            h = hits(regex(toks), x)
            found[name] = ' '.join('%#x' % (b + o + 4 * int(idx)) for o in h) if h else '-'
        print(path)
        for name, v in found.items():
            print('  %-8s %s' % (name, v))
    block = '\n'.join(lines)
    if not write:
        print(block)
        return
    s = open(SCRIPT).read()
    head = 'hmi_patterns() {\n    cat <<\'EOF\'\n'
    start = s.index(head) + len(head)
    end = s.index('EOF\n}\n', start)                 # the block's own end (it may be empty)
    assert '\nhmi_scan() {' not in s[start:end] and '\n}\n' not in s[start:end], 'hmi_patterns block not found intact'
    open(SCRIPT, 'w').write(s[:start] + block + '\n' + s[end:])
    print('hmi_patterns: %d patterns written to %s' % (len(lines) - 1, SCRIPT))


def fs_in(lines, b7, x7):
    for line in lines[1:]:
        if not line.startswith('find '):
            continue
        _f, name, idx, _key, *toks = line.split()
        toks = [t for t in toks if not t.startswith('to:')]
        if name.startswith('f') and hits(regex(toks), x7):
            return True
    return False


if __name__ == '__main__':
    main(sys.argv[1:])
