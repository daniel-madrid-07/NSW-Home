"""Uses every Settings row of "Mandos y accesorios" and the remapping rows of
"Accesibilidad" in scripted offscreen runs and checks what each one wrote:
the emulator's own settings file byte by byte against the file as it was, and
NSW-Home's configuration.

    python tools/padrows_test.py [--shots]

Runs on throw-away copies of the data folder (build/padrows/<case>), one with
Ryujinx and one with Citron as the emulator games start with. Nothing is
launched, no window, no sound; no pad is needed (the pages are fed made-up
pads). --shots also writes a few frames of each run to shots/padrows.
"""
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
EXE = os.path.join(ROOT, "build", "dev.exe")
WORK = os.path.join(ROOT, "build", "padrows")
SHOTS = "--shots" in sys.argv

passed = failed = 0


def check(ok, what):
    global passed, failed
    if ok:
        passed += 1
    else:
        failed += 1
    print(("  ok    " if ok else "  FAIL  ") + what)


def fresh(case, emulator, console=None):
    """A new copy of the data folder with `emulator` as the default one."""
    data = os.path.join(WORK, case)
    shutil.rmtree(data, ignore_errors=True)
    shutil.copytree(os.path.join(ROOT, "data"), data,
                    ignore=shutil.ignore_patterns("icons", "*.dockside-backup", "*.dockside-tmp", "captures"))
    path = os.path.join(data, "config.json")
    cfg = json.load(open(path, encoding="utf-8"))
    cfg["defaultEmu"] = emulator
    cfg["console"] = dict(console or {})
    json.dump(cfg, open(path, "w", encoding="utf-8"), ensure_ascii=False)
    return data


def run(data, screen, frames, script, name=None, keep=()):
    env = dict(os.environ, NSWHOME_DATA=data)
    out = ""
    if SHOTS and name and keep:
        out = os.path.join(WORK, "frames")
        shutil.rmtree(out, ignore_errors=True)
        os.makedirs(out)
    code = subprocess.run([EXE, "--shotseq", screen, out, str(frames), script], env=env).returncode
    if out:
        dest = os.path.join(ROOT, "shots", "padrows")
        os.makedirs(dest, exist_ok=True)
        for f in keep:
            src = os.path.join(out, "f_%03d.png" % f)
            if os.path.exists(src):
                shutil.copy(src, os.path.join(dest, "%s_%03d.png" % (name, f)))
        shutil.rmtree(out, ignore_errors=True)
    return code


def read(path):
    with open(path, "rb") as f:
        return f.read()


def changed_lines(a, b):
    """The lines of b that differ from a's, when both have the same number of lines."""
    x, y = a.split(b"\n"), b.split(b"\n")
    if len(x) != len(y):
        return None
    return [l.strip().decode("utf-8") for i, l in enumerate(y) if l != x[i]]


def console(data):
    return json.load(open(os.path.join(data, "config.json"), encoding="utf-8")).get("console", {})


def down(n, start=0, step=12):
    return ",".join("DOWN@%d" % (start + i * step) for i in range(n)), start + n * step


# Focusable rows of the section in identical mode, from the top.
RUMBLE, MOUSE, SPEED, KEYBOARD, CAL, TEST, WIRED, FORGET = 2, 4, 5, 8, 9, 11, 12, 14


def emulator_rows(case, emulator, file, expect):
    print("%s: the rows that change the emulator's own settings" % emulator)
    data = fresh(case, emulator)
    path = os.path.join(data, *file)
    original = read(path)
    state = "on-settings-in-controllers-2"

    def toggle(row, what, lines):
        steps, at = down(row)
        check(run(data, state, at + 60, steps + ",A@%d" % (at + 10), case + "_" + what, (at + 50,)) == 0, what + ": the run ended normally")
        got = changed_lines(original, read(path))
        check(got == lines, "%s switched: the lines that differ are %s" % (what, got))
        check(read(path + ".dockside-backup") == original, what + ": the backup holds the file as it was")
        run(data, state, at + 60, steps + ",A@%d" % (at + 10))
        check(read(path) == original, what + " switched back: the original file, byte for byte")
        check(not any(k.startswith("pads.wish.") for k in console(data)), what + ": nothing is left waiting in the configuration")

    toggle(RUMBLE, "vibration", expect["rumble"])
    toggle(MOUSE, "mouse", expect["mouse"])

    # The keyboard row opens a list of two choices.
    steps, at = down(KEYBOARD)
    run(data, state, at + 120, steps + ",A@%d,DOWN@%d,A@%d" % (at + 10, at + 40, at + 60), case + "_keyboard", (at + 30, at + 110))
    got = changed_lines(original, read(path))
    check(got == expect["keyboard"], "keyboard handed to games: exactly these lines differ: %s" % got)
    run(data, state, at + 120, steps + ",A@%d,UP@%d,A@%d" % (at + 10, at + 40, at + 60))
    check(read(path) == original, "keyboard off again: the original file, byte for byte")

    # Mouse speed: a list of five where the emulator has the number, nothing otherwise.
    steps, at = down(SPEED)
    run(data, state, at + 120, steps + ",A@%d,DOWN@%d,A@%d" % (at + 10, at + 40, at + 60), case + "_speed", (at + 30, at + 110))
    got = changed_lines(original, read(path))
    if expect["speed"] is None:
        check(read(path) == original and not os.path.exists(path + ".dockside-tmp"), "mouse speed: this emulator has none, the row does nothing, file untouched")
    else:
        check(got == expect["speed"], "mouse speed 75: exactly these lines differ: %s" % got)
        run(data, state, at + 120, steps + ",A@%d,UP@%d,A@%d" % (at + 10, at + 40, at + 60))
        check(read(path) == original, "mouse speed 50 again: the original file, byte for byte")

    # With a game open the change waits; it is written once the game has closed.
    steps, at = down(RUMBLE, 20)
    run(data, state, at + 80, "RUN@0," + steps + ",A@%d" % (at + 10), case + "_waiting", (at + 60,))
    check(read(path) == original, "vibration changed with a game open: the file is not touched while it runs")
    check(console(data).get("pads.wish.rumble") in ("on", "off"), "... the choice waits in the configuration (pads.wish.rumble)")
    run(data, state, 40, "DOWN@0")
    got = changed_lines(original, read(path))
    check(got == expect["rumble"], "... and is written the next time the page is shown with no game open: %s" % got)
    check("pads.wish.rumble" not in console(data), "... after which nothing is left waiting")
    steps, at = down(RUMBLE)
    run(data, state, at + 60, steps + ",A@%d" % (at + 10))
    check(read(path) == original, "... put back: the original file")
    return data


def nswhome_rows():
    print("rows kept by NSW-Home itself")
    data = fresh("own", "ryujinx", {"pads.order": "mac:aabbccddeeff|keyboard"})
    emu = os.path.join(data, "ryujinx", "Config.json")
    original = read(emu)
    state = "on-settings-in-controllers-2"

    steps, at = down(WIRED)
    run(data, state, at + 60, steps + ",A@%d" % (at + 10), "wired", (at + 50,))
    check(console(data).get("controllers.pro_wired") == "on", "Pro Controller on a cable: switched on, kept as controllers.pro_wired = on")
    run(data, state, at + 60, steps + ",A@%d" % (at + 10))
    check(console(data).get("controllers.pro_wired") == "off", "... and off again")

    steps, at = down(FORGET)
    run(data, state, at + 80, steps + ",A@%d,LEFT@%d,A@%d" % (at + 10, at + 45, at + 55), "forget_cancel", (at + 40,))
    check(console(data).get("pads.order") == "mac:aabbccddeeff|keyboard", "forget the pads, cancelled: the order is still there")
    run(data, state, at + 200, steps + ",A@%d,A@%d,A@%d" % (at + 10, at + 60, at + 150), "forget", (at + 50, at + 140))
    check("pads.order" not in console(data), "forget the pads, confirmed: the remembered order is gone")

    # The quick-access switch of the remapping.
    run(data, "on-settings-in-accessibility-2", 60, "DOWN@0,A@20", "quick", (50,))
    check(console(data).get("accessibility.remap_quick") == "on", "remapping from HOME held: switched on (accessibility.remap_quick = on)")

    # The remapping page, with a made-up pad pressing the new buttons.
    run(data, "remap-set-2", 160, "DOWN@0,DOWN@10,DOWN@20,DOWN@30,A@45,X@70", "remap_confirm_x", (60, 150))
    check(console(data).get("pads.remap") == "0,1,2,3,6,5,4,7,8,9,10,11,12,13", "confirm moved to X: stored, and X's action took A (%s)" % console(data).get("pads.remap"))
    run(data, "remap-set-2", 200, "DOWN@0,DOWN@10,DOWN@20,DOWN@30,DOWN@40,A@55,MINUS@80,A@150", "remap_back_home", (140,))
    check(console(data).get("pads.remap") == "0,1,2,3,6,5,4,7,8,9,10,11,12,13", "back onto HOME: refused, the mapping stays as it was")
    run(data, "remap-set-2", 160, "RIGHT@0,DOWN@10,DOWN@20,DOWN@30,A@45,R@70", "remap_l_zr", (150,))
    check(console(data).get("pads.remap") == "0,1,2,3,6,5,4,7,8,9,15,11,12,13", "L's action moved to ZR: stored (%s)" % console(data).get("pads.remap"))
    run(data, "remap-set-2", 120, "DOWN@0,DOWN@10,DOWN@20,DOWN@30,A@45,B@70")
    check(console(data).get("pads.remap") == "0,1,2,3,6,5,4,7,8,9,15,11,12,13", "a wait for a button cancelled from the keyboard changes nothing")
    steps, at = down(7)
    run(data, "remap-set-2", at + 100, steps + ",A@%d,B@%d" % (at + 10, at + 50))
    check(console(data).get("pads.remap") == "0,1,2,3,6,5,4,7,8,9,15,11,12,13", "restore, cancelled: the mapping stays")
    run(data, "remap-set-2", at + 200, steps + ",A@%d,A@%d" % (at + 10, at + 60), "remap_restore", (at + 40, at + 190))
    check("pads.remap" not in console(data), "restore, confirmed: the stock mapping (nothing stored)")

    # The test page: left by a pad button kept down, or from the keyboard.
    check(run(data, "padtest-set-2", 120, "Y@10", "padtest_hold", (5, 50, 110)) == 0, "input test: a pad button held for a second leaves the page")
    check(run(data, "padtest-none-2", 80, "B@10", "padtest_none", (5, 70)) == 0, "input test with no pad: left from the keyboard")

    # The stick page: measure, then change the dead zone the emulator uses.
    frames = int((1.5 + 4.0) * 60) + 40
    run(data, "padcal-set-2", frames + 100, "A@0,DOWN@%d,RIGHT@%d,RIGHT@%d,RIGHT@%d" % (frames, frames + 12, frames + 24, frames + 36), "padcal",
        (30, 200, frames - 5, frames + 90))
    got = changed_lines(original, read(emu))
    check(got == ['"deadzone_left": 0.13,'], "left dead zone stepped up three times: one line differs: %s" % got)
    run(data, "padcal-set-2", frames + 60, "A@0,DOWN@%d,A@%d" % (frames, frames + 20), "padcal_suggested", (frames + 50,))
    got = changed_lines(original, read(emu))
    check(got == ['"deadzone_left": 0.11,'], "A after measuring: the suggested dead zone is written: %s" % got)
    run(data, "padcal-set-2", 80, "DOWN@0,DOWN@12,RIGHT@24,LEFT@36,UP@48,LEFT@60")
    check(read(emu) == original, "right dead zone up and down, left one back to 10 %: the original file, byte for byte")
    run(data, "padcal-set-2", 80, "RUN@0,DOWN@10,RIGHT@24,A@60")
    check(read(emu) == original, "with a game open the dead zone is not written (the page says so)")
    check(run(data, "padcal-none-2", 60, "A@0,DOWN@10,RIGHT@20,B@40", "padcal_none", (30,)) == 0, "stick page with no pad: nothing to do, leaves normally")
    check(read(emu) == original, "... and nothing was written")


def main():
    if not os.path.exists(EXE):
        sys.exit("build\\dev.exe is missing: run build.bat dev first")
    os.makedirs(WORK, exist_ok=True)
    emulator_rows("ryujinx", "ryujinx", ("ryujinx", "Config.json"), {
        "rumble": ['"enable_rumble": true'],
        "mouse": ['"enable_mouse": true,'],
        "keyboard": ['"enable_keyboard": true,'],
        "speed": None,
    })
    emulator_rows("citron", "citron", ("citron", "config", "qt-config.ini"), {
        "rumble": ["vibration_enabled\\default=false", "vibration_enabled=false"],
        "mouse": ["mouse_enabled\\default=false", "mouse_enabled=true"],
        "keyboard": ["keyboard_enabled\\default=false", "keyboard_enabled=true"],
        "speed": ["mouse_panning_x_sensitivity\\default=false", "mouse_panning_x_sensitivity=75",
                  "mouse_panning_y_sensitivity\\default=false", "mouse_panning_y_sensitivity=75"],
    })
    nswhome_rows()
    print("%d passed, %d failed" % (passed, failed))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
