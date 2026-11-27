"""Test set-up: makes a NEW, portable emulator produce no sound, in its own settings file.

    python tools/emu_silence.py <Config.json | qt-config.ini>

Ryujinx family: audio_backend becomes "Dummy". yuzu family: audio_muted=true and volume=0
(each with its "\\default=false" line, without which the emulator ignores the value).
It refuses anything that is not inside a "portable" or "user" folder, and the owner's
Citron: the test mutes the owner's own emulators in the Windows mixer instead.
"""
import re
import sys

p = sys.argv[1]
low = p.lower().replace("/", "\\")
assert "\\appdata\\" not in low and "citron" not in low and ("\\portable\\" in low or "\\user\\" in low), \
    "not a new portable emulator"
s = open(p, "rb").read().decode("utf-8")
out = s
if low.endswith(".json"):
    out = re.sub(r'("audio_backend":\s*)"[A-Za-z0-9]+"', r'\1"Dummy"', out, count=1)
else:
    for key, val in (("audio_muted", "true"), ("volume", "0")):
        out = re.sub(r"(?m)^%s\\default=\w+(\r?)$" % key, lambda m, k=key: k + "\\default=false" + m.group(1), out, count=1)
        out = re.sub(r"(?m)^%s=.*?(\r?)$" % key, lambda m, k=key, v=val: k + "=" + v + m.group(1), out, count=1)
open(p, "wb").write(out.encode("utf-8"))
print("changed" if out != s else "unchanged")
