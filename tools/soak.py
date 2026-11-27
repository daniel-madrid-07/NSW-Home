"""Random-walk soak test: presses buttons at random through the menus, offscreen,
on a throw-away copy of the data folder, and reports whether the program survived.

    python tools/soak.py [seed] [presses] [start screen]

Nothing is launched and no window, dialog or sound is opened (offscreen runs
never do). The copy lives in the system temp folder and is deleted afterwards;
the emulator's own files are not touched (a copy of its settings and profile
files is put inside the throw-away data folder, which is where a test run
looks for them).
"""
import os
import random
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# Weighted towards moving and confirming; text keys only do something while a
# text box is open, and HOME/RUN/EXIT exercise the running-game states.
STEPS = (["A"] * 9 + ["B"] * 5 + ["UP"] * 6 + ["DOWN"] * 9 + ["LEFT"] * 5 + ["RIGHT"] * 6 +
         ["X", "Y", "PLUS", "MINUS", "L", "R", "HOME", "RUN", "EXIT", "DOWN+", "RIGHT+", "LEFT+", "UP+"] +
         ["TYPE"] * 2 + ["ENTER"] * 2 + ["ESC"] * 2 + ["MOUSE"] * 8)


def mouse_step(rng):
    """A random mouse step (see RunShotSeq in src/main.cpp), in the 1280x720 layout:
    mostly drags of the lists (a throw or a stop before letting go, now and then
    starting on the scroll bars' column), clicks, and the wheel."""
    x, y = rng.uniform(0, 1280), rng.uniform(60, 660)
    kind = rng.choice(["drag"] * 4 + ["bar", "click", "click", "wheel"])
    if kind == "click":
        return f"MCLICK:{x:.0f}:{y:.0f}"
    if kind == "wheel":
        return f"WHEEL:{rng.choice((-2, -1, 1, 2))}"
    if kind == "bar":
        x = rng.choice((1009, 1166, 1254, 1256, 911))
    vertical = rng.random() < 0.7
    x1 = x if vertical else x + rng.uniform(-700, 700)
    y1 = y + rng.uniform(-450, 450) if vertical or kind == "bar" else y
    hold = rng.choice((0, 0, 3, 12))
    return f"MDRAG:{x:.0f}:{y:.0f}:{x1:.0f}:{y1:.0f}:{rng.randint(1, 20)}:{hold}"


def main():
    seed = int(sys.argv[1]) if len(sys.argv) > 1 else 1
    presses = int(sys.argv[2]) if len(sys.argv) > 2 else 2000
    screen = sys.argv[3] if len(sys.argv) > 3 else "home-2"
    rng = random.Random(seed)

    exe = os.path.join(ROOT, "build", "dev.exe")
    if not os.path.exists(exe):
        exe = os.path.join(ROOT, "build", "NSW-Home.exe")

    tmp = tempfile.mkdtemp(prefix="nswhome_soak_")
    data = os.path.join(tmp, "data")
    try:
        real = os.path.join(os.environ["APPDATA"], "NSW-Home")
        # Until the program has moved it at its first start, the data folder still has the former name.
        if not os.path.isdir(real) and os.path.isdir(os.path.join(os.environ.get("APPDATA", ""), "Dockside")):  # historical name
            real = os.path.join(os.environ.get("APPDATA", ""), "Dockside")  # historical name
        shutil.copytree(real, data) if os.path.isdir(real) else os.makedirs(data)
        ryu = os.path.join(os.environ["APPDATA"], "Ryujinx")
        os.makedirs(os.path.join(data, "ryujinx", "system"), exist_ok=True)
        for rel in ("Config.json", os.path.join("system", "Profiles.json")):
            src = os.path.join(ryu, rel)
            if os.path.exists(src):
                shutil.copyfile(src, os.path.join(data, "ryujinx", rel))

        frame, script = 5, []
        for _ in range(presses):
            step = rng.choice(STEPS)
            script.append(f"{mouse_step(rng) if step == 'MOUSE' else step}@{frame}")
            frame += rng.choice((2, 3, 4, 6, 9, 14, 25, 60))
        env = dict(os.environ, NSWHOME_DATA=data)
        env.pop("NSWHOME_EMUDATA", None)
        env.pop("DOCKSIDE_EMUDATA", None)  # historical name, still read by the program
        run = subprocess.run([exe, "--shotseq", screen, "", str(frame + 120), ",".join(script)], env=env)
        code = run.returncode & 0xFFFFFFFF
        print(f"seed {seed}: {presses} presses over {frame + 120} frames from {screen}: "
              f"{'ok' if code == 0 else 'FAILED, exit code 0x%08X' % code}")
        if code != 0:
            path = os.path.join(ROOT, "shots", f"soak_fail_{seed}.txt")
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8") as f:
                f.write(screen + "\n" + ",".join(script) + "\n")
            print("script saved to", path)
        return 0 if code == 0 else 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
