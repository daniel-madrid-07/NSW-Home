// See padsys.h.
#include "padsys.h"

#ifdef _WIN32
#include <windows.h>
#include <cstdio>
#include <cwctype>

namespace padsys {

bool KeyboardLayout(std::wstring& name) {
    // The low word of a layout handle is the language it types.
    const LANGID lang = LOWORD(reinterpret_cast<UINT_PTR>(GetKeyboardLayout(0)));
    // Language and country apart, put together as the console writes them:
    // the system's one-piece name adds the sort order ("España, internacional").
    wchar_t language[120] = {}, country[120] = {};
    const LCID locale = MAKELCID(lang, SORT_DEFAULT);
    if (!lang || !GetLocaleInfoW(locale, LOCALE_SLOCALIZEDLANGUAGENAME, language, 120) || !language[0]) return false;
    name = language;
    name[0] = static_cast<wchar_t>(towupper(name[0])); // Spanish names come in lower case
    if (GetLocaleInfoW(locale, LOCALE_SLOCALIZEDCOUNTRYNAME, country, 120) && country[0]) name += std::wstring(L" (") + country + L")";
    return true;
}

bool KeysHeld(std::vector<std::wstring>& names, size_t most) {
    names.clear();
    DWORD owner = 0;
    const HWND front = GetForegroundWindow();
    if (!front || !GetWindowThreadProcessId(front, &owner) || owner != GetCurrentProcessId()) return true;
    for (UINT vk = 8; vk < 255 && names.size() < most; vk++) {
        // 0x10 to 0x12 are "either Shift / Ctrl / Alt": the left and right keys
        // have numbers of their own further up.
        if ((vk >= 0x10 && vk <= 0x12) || !(GetAsyncKeyState(static_cast<int>(vk)) & 0x8000)) continue;
        UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        // The keys that share a scan code with the number pad carry this mark.
        const bool extended = vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN || vk == VK_PRIOR || vk == VK_NEXT ||
                              vk == VK_HOME || vk == VK_END || vk == VK_INSERT || vk == VK_DELETE || vk == VK_RCONTROL ||
                              vk == VK_RMENU || vk == VK_DIVIDE || vk == VK_NUMLOCK || vk == VK_LWIN || vk == VK_RWIN || vk == VK_APPS;
        wchar_t text[64] = {};
        if (!scan || !GetKeyNameTextW(static_cast<LONG>((scan << 16) | (extended ? 1u << 24 : 0u)), text, 64) || !text[0])
            swprintf_s(text, L"Tecla %u", vk);
        names.push_back(text);
    }
    return true;
}

} // namespace padsys

#else

namespace padsys {

bool KeyboardLayout(std::wstring&) {
    return false;
}

bool KeysHeld(std::vector<std::wstring>&, size_t) {
    return false;
}

} // namespace padsys

#endif
