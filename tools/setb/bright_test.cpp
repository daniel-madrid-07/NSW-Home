// Exercises pchost's real brightness path once: reads the monitor's value,
// writes that same value back, reads again, and prints the three with the
// time each took. Nothing else is written. Also prints the display facts.
//   cl /nologo /std:c++17 /EHsc /W4 /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX tools\setb\bright_test.cpp src\pchost.cpp
//      user32.lib gdi32.lib shell32.lib dxva2.lib shcore.lib bcrypt.lib
#include "../../src/pchost.h"
#include <windows.h>
#include <cstdio>

int main() {
    const pchost::Display d = pchost::DisplayNow();
    printf("display ok=%d %dx%d %d Hz scale=%d hdr=%d capable=%d bits=%d builtIn=%d\n", d.ok, d.width, d.height, d.hz, d.scale,
           d.hdr, d.hdrCapable, d.bits, d.builtIn);
    printf("adapter known: %s\n", pchost::AdapterName().empty() ? "no" : "yes");
    printf("sha256(abc) = %s\n", pchost::Sha256Hex("abc").c_str());
    int a = -1, b = -1;
    ULONGLONG t0 = GetTickCount64();
    const bool r1 = pchost::BrightnessGet(a);
    ULONGLONG t1 = GetTickCount64();
    printf("read: ok=%d value=%d (%llu ms)\n", r1, a, t1 - t0);
    if (!r1) return 0;
    const bool w = pchost::BrightnessSet(a);
    ULONGLONG t2 = GetTickCount64();
    printf("write of the same value: ok=%d (%llu ms)\n", w, t2 - t1);
    const bool r2 = pchost::BrightnessGet(b);
    printf("read again: ok=%d value=%d (%llu ms)\n", r2, b, GetTickCount64() - t2);
    return 0;
}
