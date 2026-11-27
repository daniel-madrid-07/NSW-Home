"""tools/soak.py for the Linux/macOS build: the same random walk (same steps,
same seeds give the same presses), offscreen, on a throw-away copy of a data
folder.

    python3 tools/linux_soak.py <nsw-home binary> <data folder> [seed] [presses] [start screen]

No window, sound or emulator: --shotseq never opens or starts anything, and
the emulators' own folders are out of the way (XDG_CONFIG_HOME and
XDG_DATA_HOME point into the throw-away folder).
"""
import os
import random
import shutil
import subprocess
import sys
import tempfile

STEPS = (["A"] * 9 + ["B"] * 5 + ["UP"] * 6 + ["DOWN"] * 9 + ["LEFT"] * 5 + ["RIGHT"] * 6 +
         ["X", "Y", "PLUS", "MINUS", "L", "R", "HOME", "RUN", "EXIT", "DOWN+", "RIGHT+", "LEFT+", "UP+"] +
         ["TYPE"] * 2 + ["ENTER"] * 2 + ["ESC"] * 2)


def main():
    exe, source = sys.argv[1], sys.argv[2]
    seed = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    presses = int(sys.argv[4]) if len(sys.argv) > 4 else 2000
    screen = sys.argv[5] if len(sys.argv) > 5 else "home-2"
    rng = random.Random(seed)
    tmp = tempfile.mkdtemp(prefix="nswhome_soak_")
    try:
        data = os.path.join(tmp, "data")
        shutil.copytree(source, data)
        frame, script = 5, []
        for _ in range(presses):
            script.append(f"{rng.choice(STEPS)}@{frame}")
            frame += rng.choice((2, 3, 4, 6, 9, 14, 25, 60))
        env = dict(os.environ, NSWHOME_DATA=data, XDG_CONFIG_HOME=os.path.join(tmp, "config"),
                   XDG_DATA_HOME=os.path.join(tmp, "share"), SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
        env.pop("NSWHOME_EMUDATA", None)
        env.pop("DOCKSIDE_EMUDATA", None)  # historical name, still read by the program
        run = subprocess.run([exe, "--shotseq", screen, "", str(frame + 120), ",".join(script)], env=env)
        ok = run.returncode == 0
        print(f"seed {seed}: {presses} presses over {frame + 120} frames from {screen}: "
              f"{'ok' if ok else 'FAILED, exit code %d' % run.returncode}")
        if not ok:
            print(screen + "\n" + ",".join(script))
        return 0 if ok else 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
