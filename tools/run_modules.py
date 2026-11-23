#!/usr/bin/env python3
"""Run each test module in its own process group with a hard timeout, so a
blocked child cannot hold the harness. Prints one line per module."""
import glob, os, signal, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
LIMIT = int(os.environ.get('LIMIT', '180'))
SKIP = {'test_installer'}          # needs per-class selection
mods = [os.path.basename(p)[:-3] for p in sorted(glob.glob('tests/test_*.py'))]
mods = [m for m in mods if m not in SKIP]

bad = []
for m in mods:
    t0 = time.time()
    p = subprocess.Popen([sys.executable, '-m', 'unittest', '-v', 'tests.' + m],
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         text=True, start_new_session=True)
    try:
        out, _ = p.communicate(timeout=LIMIT)
        status = 'ok' if p.returncode == 0 else 'FAIL'
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGKILL)
        out, _ = p.communicate()
        status = 'TIMEOUT'
    lines = [l for l in (out or '').splitlines() if l.strip()]
    last = lines[-1] if lines else ''
    lasttest = next((l for l in reversed(lines) if l.startswith('test_')), '')
    print(f'{m:28} {status:8} {time.time()-t0:6.1f}s  {last[:90]}')
    if status != 'ok':
        bad.append((m, status, lasttest[:100]))
    sys.stdout.flush()

print()
if bad:
    print('NOT OK:')
    for m, s, t in bad:
        print(f'  {m}: {s}  last test: {t}')
else:
    print('all modules ok')
sys.exit(1 if bad else 0)
