"""Lists the texts the menu draws that have no line in src/lang_strings.inc for a language.

    python tools/lang_missing.py [language] [soak runs] [presses]

language: a firmware code of lang.cpp (default USen, English (United States), the default of a
new install). Every test state below is drawn once in that language, in both styles, then every
row of Settings, of a game's options and of the user page is pressed once (what it opens is
drawn), and then `soak runs` random walks (default 6, of `presses` presses each, default 1500) go
through the menus as tools/soak.py does; whatever was drawn with no entry is printed, one text per line, most
frequent screens first, ready to be given an O(...) line. Offscreen only: no window, no sound, no
emulator; the walks run on a throw-away copy of the data folder (this copy's data\\ when there is
one, else %APPDATA%\\NSW-Home).

A text that is data (a game's or a user's name) is left out by the program itself. What remains
can still hold things that are not wording (an emulator's name, a path): give those a K(...)
line, or leave them. The amiibo states want tools\\build_amiibo_test.bat's made-up figures in
local\\amiibo_test (build\\amiibo_test.exe local\\amiibo_test).
"""
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

SECTIONS = ["help", "airplane", "brightness", "bluetooth", "internet", "parental", "accessibility", "data", "users",
            "controllers", "sound", "display", "mii", "amiibo", "themes", "notifications", "sleep", "system", "nswhome"]
UPD = ["row", "check", "latest", "newer", "noasset", "none", "offline", "refused", "odd", "fetch", "done", "bad", "ro",
       "locked", "aside", "net", "cancelled", "game"]
FRIENDS = ["list", "sec", "empty", "profile", "profile:2", "profile:5", "add", "add-on", "add-nf", "add-net", "add-unv", "pad",
           "found", "consent", "off", "received", "received-empty", "received-trouble", "asker", "sent", "asked", "reqsec",
           "home"]
STATES = (["home", "sys", "settings", "all", "all-open", "album", "album-empty", "pads", "pads-none", "user", "user-rows",
           "user-icon", "user-new", "user-plus", "who", "dialog", "dlg-close", "dlg-power", "dlg-sleep", "dlg-upd",
           "dlg-wait", "launch", "options", "options-cat", "options-open", "entry", "padcal", "padtest", "remap",
           "capture-notice", "capture-failed", "setup-lang+first", "setup-style", "setup-theme", "setup-emu",
           "setup-emu+noemu", "setup-keys", "setup-keys+nokeys+nofw", "setup-own+pc", "setup-games", "setup-games+nodirs",
           "setup-user", "setup-user+fresh", "setup-form", "setup-done", "setup-done+noemu+nodirs+nokeys"] +
          ["settings-in-" + s for s in SECTIONS] + ["settings-end-" + s for s in SECTIONS] +
          ["options-in-%d" % n for n in range(5)] + ["user-sec:%d" % n for n in range(6)] +
          ["off-settings-in-" + s for s in SECTIONS] +
          # The pieces of work that came after the first translation, by their own test states
          # (each block's Debug* function in ui.cpp says what its states show).
          ["friends-" + f for f in FRIENDS] +
          ["amiibo-empty", "amiibo-empty-use", "amiibo-fill", "amiibo-fill-delete", "amiibo-fill-format", "amiibo-fill-use",
           "amiibo-fill-one", "amiibo-fill-options", "amiibo-many"] +
          ["content", "content-last", "content-upd", "content-dlc", "content-mods", "content-cheats", "content-upd+eden",
           "content-dlc+eden", "content-mods+eden", "content-cheats+eden"] +
          ["upd-" + u for u in UPD] +
          ["web", "web-news", "web-who", "web-who-news", "web-bar", "web-bar-news", "web-one"] +
          ["swkbd", "swkbd-guide", "swkbd-num", "swkbd-long"] +
          ["avatar-frame:%AVATAR%", "avatar-reframe", "user-icon", "type:Prueba+user-new"] +
          ["cardpc", "cardall", "mouse-all", "album-sort", "album-demo", "dlg-one", "run", "more", "last", "ingame",
           "launchout"] +
          ["bar%d" % n for n in range(10)] +
          ["pads-set", "pads-order", "pads-jc", "pads-on", "pads-off", "padtest-set", "padtest-none", "padcal-set",
           "padcal-done", "padcal-none", "remap-set", "remap-capture", "remap-changed"] +
          ["sc-net", "sc-tone", "sc-mic", "sc-order", "sc-busy", "sc-pin", "sc-reached", "wifi-settings-in-internet",
           "cable-settings-in-internet", "nonet-settings-in-internet", "nomic-settings-in-sound", "users3-who",
           "limit-settings-in-parental", "pin1234-settings-in-parental", "a11y:zoom=on:zoomed-home"] +
          ["pc-sleep", "pcerr~settings-in-help", "pcpin:1234~pc-lock-start", "pcpin:1234~settings-in-system", "pcpin:~home"] +
          ["netplay", "netplay-game", "netplay-confirm", "netplay-sending", "netplay-wait", "netplay-answer",
           "netplay-joining", "netplay-declined", "netplay-noanswer", "netplay-unsent", "netplay-withdrawn",
           "netplay-unsupported", "netplay-noshare", "netplay-how", "netplay-popup", "netplay-bar", "netplay-bar-off",
           "netplay-run"])

# Where the random walks start, in turn.
WALKS = ["home-2", "settings-2", "s1-home", "s1-settings", "user-2", "home-2l", "amiibo-fill-2", "content-upd-2",
         "friends-received-2", "upd-row-2", "options-in-4-2", "s1-friends-add-on"]

STEPS = (["A"] * 9 + ["B"] * 5 + ["UP"] * 6 + ["DOWN"] * 9 + ["LEFT"] * 5 + ["RIGHT"] * 6 +
         ["X", "Y", "PLUS", "MINUS", "L", "R", "HOME", "RUN", "EXIT", "DOWN+", "RIGHT+"] + ["TYPE", "ENTER", "ESC"])


def main():
    sys.stdout.reconfigure(encoding="utf-8")  # the texts are not in the console's code page
    code = sys.argv[1] if len(sys.argv) > 1 else "USen"
    runs = int(sys.argv[2]) if len(sys.argv) > 2 else 6
    presses = int(sys.argv[3]) if len(sys.argv) > 3 else 1500
    exe = os.path.join(ROOT, "build", "dev.exe")
    if not os.path.exists(exe):
        exe = os.path.join(ROOT, "build", "NSW-Home.exe")
    tmp = tempfile.mkdtemp(prefix="nswhome_lang_")
    try:
        src = os.path.join(ROOT, "data")
        if not os.path.isdir(src):
            src = os.path.join(os.environ.get("APPDATA", ""), "NSW-Home")
            # Until the program has moved it at its first start, the data folder still has the former name.
            if not os.path.isdir(src) and os.path.isdir(os.path.join(os.environ.get("APPDATA", ""), "Dockside")):  # historical name
                src = os.path.join(os.environ.get("APPDATA", ""), "Dockside")  # historical name
        data = os.path.join(tmp, "data")
        shutil.copytree(src, data) if os.path.isdir(src) else os.makedirs(data)
        # The amiibo states read their made-up figures from local\amiibo_test beside the data
        # folder (tools\build_amiibo_test.bat makes them).
        figures = os.path.join(ROOT, "local", "amiibo_test")
        if os.path.isdir(figures):
            shutil.copytree(figures, os.path.join(tmp, "local", "amiibo_test"))
        picture = os.path.join(data, "avatar.jpg")
        log = os.path.join(tmp, "missing.txt")
        env = dict(os.environ, NSWHOME_DATA=data, NSWHOME_LANGMISS=log)
        env.pop("NSWHOME_EMUDATA", None)
        env.pop("DOCKSIDE_EMUDATA", None)  # historical name, still read by the program
        shot = os.path.join(tmp, "x.png")
        order = {}
        # Pressing every row flips switches too (identical mode, the style...): each run starts
        # from the same configuration.
        config = os.path.join(data, "config.json")
        with open(config, "rb") as f:
            pristine = f.read()

        def draw(name, timeout=120):
            before = len(read(log))
            subprocess.run([exe, "--shot", "lang:%s:%s" % (code, name), shot], env=env, timeout=timeout)
            with open(config, "wb") as f:
                f.write(pristine)
            for t in read(log)[before:]:
                order.setdefault(t, name)

        def styled(st, style):
            return ("s1-" + st) if style == "-1" else st + style

        for style in ("-2", "-2l", "-1"):
            for st in STATES:
                draw(styled(st.replace("%AVATAR%", picture), style))
        # Then every row of Settings, of a game's options and of the user page pressed once
        # (DebugLangRows in ui.cpp): what a row opens (its choices, its page, its box) is drawn
        # even where a walk would never get to.
        # In the second console's style also each choice of what the press opened (up to 16).
        counts, choices = os.path.join(tmp, "rows.txt"), os.path.join(tmp, "choices.txt")
        for style in ("-2", "-1"):
            draw(styled("langrows:" + counts, style))
            for line in read(counts):
                page, sec, n = line.split()
                for k in range(int(n)):
                    row = "langrow:%s:%s:%d" % (page, sec, k)
                    if os.path.exists(choices):
                        os.remove(choices)
                    draw(styled(row + "@" + choices, style))
                    m = int((read(choices) or ["0"])[0]) if style == "-2" else 0
                    for j in range(min(m, 16)):
                        draw(styled("%s:%d" % (row, j), style))
        rng = random.Random(1)
        for r in range(runs):
            start = WALKS[r % len(WALKS)]
            frame, script = 5, []
            for _ in range(presses):
                script.append("%s@%d" % (rng.choice(STEPS), frame))
                frame += rng.choice((2, 3, 4, 6, 9, 14, 25))
            before = len(read(log))
            subprocess.run([exe, "--shotseq", "lang:%s:%s" % (code, start), "", str(frame + 60), ",".join(script)],
                           env=env, timeout=1800)
            for t in read(log)[before:]:
                order.setdefault(t, "walk from " + start)
        # A line of a longer text that is wrapped by hand is drawn on its own: only the whole
        # text needs a line in the table.
        # Data rather than wording: a text box being typed in ("Name|"), sizes, addresses, ids.
        for t in list(order):
            if (t.endswith("|") or t.startswith("…") or set(t) <= set("x") or re.match(r"^[\d.,]+ [KMG]B$", t)
                    or re.match(r"^[0-9a-f]{16}$", t) or re.match(r"^[0-9a-f:]+$", t) and ":" in t):
                del order[t]
        texts = list(order)
        for t in texts:
            if len(t.split()) >= 3 and any(len(o) > len(t) and t in o for o in texts):
                del order[t]
        for t, where in order.items():
            print("%s\t%s" % (t, where))
        print("%d texts without a line for %s" % (len(order), code), file=sys.stderr)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def read(path):
    if not os.path.exists(path):
        return []
    with open(path, encoding="utf-8") as f:
        return [l.rstrip("\n") for l in f if l.strip()]


if __name__ == "__main__":
    main()
