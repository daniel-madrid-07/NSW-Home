"""Random presses inside the Settings sections whose rows act on files (Gestión de
datos, Control parental, Perfiles de usuario, Sonido, Internet), offscreen, on a
throw-away data folder that holds a stand-in emulator tree, and then a check of
what is left of that tree: whatever the presses did, no save, key, firmware or
settings file of the stand-in may be gone or changed.

    python tools/setc_soak.py [seed] [presses]

Needs build\\dev.exe and build\\setc_test.exe. Nothing real is touched: an
offscreen run only ever works on <data>\\standin (see scTest in src\\ui.cpp).
"""
import os
import random
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STEPS = ["A"] * 12 + ["B"] * 3 + ["UP"] * 4 + ["DOWN"] * 8 + ["LEFT"] * 2 + ["RIGHT"] * 3 + ["HOME", "RUN", "EXIT"]
STARTS = ["on-settings-in-data-2", "off-settings-in-data-2l", "on-settings-in-parental-2", "users3-on-settings-in-users-2",
          "on-settings-in-sound-2", "on-settings-in-internet-2", "limit-home-2", "on-s1-settings-in-data"]
CACHE_MARKS = ("\\cache\\cpu\\", "\\cache\\shader\\", "eden\\shader\\0100")


def listing(top):
    out = {}
    for base, _, files in os.walk(top):
        for f in files:
            p = os.path.join(base, f)
            out[os.path.relpath(p, top)] = os.path.getsize(p)
    return out


def main():
    seed = int(sys.argv[1]) if len(sys.argv) > 1 else 1
    presses = int(sys.argv[2]) if len(sys.argv) > 2 else 600
    rng = random.Random(seed)
    tmp = tempfile.mkdtemp(prefix="nswhome_setc_")
    bad = 0
    try:
        for start in STARTS:
            data = os.path.join(tmp, "data")
            shutil.rmtree(data, ignore_errors=True)
            shutil.copytree(os.path.join(ROOT, "data"), data)
            subprocess.run([os.path.join(ROOT, "build", "setc_test.exe"), "tree", os.path.join(data, "standin")],
                           check=True, stdout=subprocess.DEVNULL)
            before = listing(os.path.join(data, "standin"))
            script = ",".join("%s@%d" % (rng.choice(STEPS), i * 6) for i in range(presses))
            env = dict(os.environ, NSWHOME_DATA=data)
            rc = subprocess.run([os.path.join(ROOT, "build", "dev.exe"), "--shotseq", start, "", str(presses * 6 + 60), script],
                                env=env).returncode
            after = listing(os.path.join(data, "standin"))
            # Allowed: cache files gone, new copies under "copias", the stand-in game moved between lib1 and lib2.
            lost = [p for p in before if p not in after and not any(m in p for m in CACHE_MARKS) and not p.startswith("lib")]
            changed = [p for p in before if p in after and after[p] != before[p]]
            games = [p for p in after if p.startswith("lib")]
            new = [p for p in after if p not in before and not p.startswith("copias\\") and not p.startswith("lib")]
            ok = rc == 0 and not lost and not changed and not new and len(games) == 1 and after[games[0]] == 123456
            gone = sum(1 for p in before if p not in after)
            print("%-34s exit %d  files gone %d (caches only: %s)  copies made %d  game in %s  %s" %
                  (start, rc, gone, "yes" if not lost else "NO", sum(1 for p in after if p.startswith("copias\\")),
                   games[0].split("\\")[0] if games else "?", "ok" if ok else "FAILED"))
            for p in lost + changed + new:
                print("   unexpected:", p)
            bad += 0 if ok else 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("seed %d, %d presses per start: %s" % (seed, presses, "all ok" if not bad else "%d FAILED" % bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
