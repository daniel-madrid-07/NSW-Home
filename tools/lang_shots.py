"""Stills of the menu in several languages, both styles, and one contact sheet per language.

    python tools/lang_shots.py [--new] [codes...]      (default: EUes USen EUfr EUde EUit JPja KRko CNzh USpt)

Writes shots/lang/<code>/<style>_<state>.png and shots/lang/sheet_<code>.jpg. Offscreen only
(--shot). With --new: the pages added after the first translation (friend requests, amiibo,
updates / DLC / mods / cheats, NSW-Home's own update, the browser, the keyboard, the picture's
framing page, the controllers' notes, the error history), as shots/lang/new_<code>/ and
shots/lang/sheet_new_<code>.jpg; those states write to the data folder (the amiibo collection, the
error history), so they run on a throw-away copy of it.
"""
import os
import shutil
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
STATES = ["home", "settings-sec-system", "settings-sec-sound", "settings-sec-data", "settings", "user", "dialog", "all",
          "setup-lang+first"]
NEW = ["friends-add", "friends-consent", "friends-found", "friends-received", "friends-received-trouble", "friends-asker",
       "friends-asked", "amiibo-empty", "amiibo-fill", "amiibo-fill-one", "amiibo-fill-use", "content-last", "content-upd",
       "content-dlc+eden", "content-mods", "upd-check", "upd-newer", "upd-noasset", "upd-done", "upd-aside", "web-who",
       "web-who-news", "web", "swkbd", "swkbd-num", "avatar-frame:%AVATAR%", "avatar-reframe", "pads-set", "padcal-done",
       "pcerr~langrow:s:0:3", "langrow:s:18:5", "sc-reached"]


def main():
    args = sys.argv[1:]
    new = "--new" in args
    codes = [a for a in args if a != "--new"] or ["EUes", "USen", "EUfr", "EUde", "EUit", "JPja", "KRko", "CNzh", "USpt"]
    exe = os.path.join(ROOT, "build", "dev.exe")
    tmp = tempfile.mkdtemp(prefix="nswhome_shots_") if new else None
    data = os.path.join(ROOT, "data")
    if new:
        shutil.copytree(data, os.path.join(tmp, "data"))
        figures = os.path.join(ROOT, "local", "amiibo_test")
        if os.path.isdir(figures):
            shutil.copytree(figures, os.path.join(tmp, "local", "amiibo_test"))
        data = os.path.join(tmp, "data")
    env = dict(os.environ, NSWHOME_DATA=data)
    states = [s.replace("%AVATAR%", os.path.join(data, "avatar.jpg")) for s in (NEW if new else STATES)]
    try:
        for code in codes:
            out = os.path.join(ROOT, "shots", "lang", ("new_" if new else "") + code)
            os.makedirs(out, exist_ok=True)
            cells = []
            for style in ("2", "1"):
                for st in states:
                    name = (st + "-2") if style == "2" else ("s1-" + st if not st.startswith("setup") else st + "-1")
                    label = st.split(":")[0] if st.startswith("avatar-frame:") else st
                    png = os.path.join(out, "s%s_%s.png" % (style, "".join(c if c.isalnum() or c in "-_" else "_" for c in label)))
                    r = subprocess.run([exe, "--shot", "lang:%s:%s" % (code, name), png], env=env, timeout=120)
                    cells.append((png if r.returncode == 0 and os.path.exists(png) else None, "%s %s" % (style, label)))
            cols, w, h = 6, 480, 270
            rows = (len(cells) + cols - 1) // cols
            sheet = Image.new("RGB", (cols * w, rows * (h + 18)), (40, 40, 40))
            d = ImageDraw.Draw(sheet)
            for i, (png, label) in enumerate(cells):
                x, y = (i % cols) * w, (i // cols) * (h + 18)
                if png:
                    sheet.paste(Image.open(png).convert("RGB").resize((w, h)), (x, y + 18))
                d.text((x + 4, y + 3), "%s  %s" % (code, label), fill=(255, 220, 0))
            sheet.save(os.path.join(ROOT, "shots", "lang", "sheet_%s%s.jpg" % ("new_" if new else "", code)), quality=88)
            print(code, "done")
    finally:
        if tmp:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
