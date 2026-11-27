// Stand-in for <d2d1.h>. gfx.h names one Direct2D type: the decoded picture
// an Img holds on to. The Cairo backend (../gfx_cairo.cpp) supplies its own
// object behind this interface.
#pragma once
#include <windows.h>

struct ID2D1Bitmap {
    virtual ULONG Release() = 0;

protected:
    virtual ~ID2D1Bitmap() = default;
};
