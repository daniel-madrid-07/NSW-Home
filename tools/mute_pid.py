"""Keeps one process's sound muted in the Windows mixer while a test runs.

    python tools/mute_pid.py <pid>            mute its audio sessions as they appear, until the process ends
    python tools/mute_pid.py <pid> --unmute   put them back (Windows remembers the mute per program otherwise)

Used by emu_test.ps1 so that the emulators' own settings need not be touched.
"""
import sys
import time

from pycaw.pycaw import AudioUtilities


def set_mute(pid, mute):
    found = False
    for s in AudioUtilities.GetAllSessions():
        if s.Process is not None and s.Process.pid == pid:
            found = True
            v = s.SimpleAudioVolume
            if bool(v.GetMute()) != mute:
                v.SetMute(1 if mute else 0, None)
    return found


def alive(pid):
    import ctypes
    h = ctypes.windll.kernel32.OpenProcess(0x1000, False, pid)  # query limited information
    if not h:
        return False
    code = ctypes.c_ulong()
    ctypes.windll.kernel32.GetExitCodeProcess(h, ctypes.byref(code))
    ctypes.windll.kernel32.CloseHandle(h)
    return code.value == 259  # still active


def main():
    pid = int(sys.argv[1])
    if "--unmute" in sys.argv:
        set_mute(pid, False)
        return
    while alive(pid):
        try:
            set_mute(pid, True)
        except Exception:
            pass
        time.sleep(0.15)


if __name__ == "__main__":
    main()
