// Console menus transcribed from the owner's Switch 2 captures (Spanish, firmware 23.5.0).
// Plain data for the "identical" mode: wording, order, grouping and row types are the console's.
// Nothing here does anything by itself; the page code decides what each row means on a PC.
#pragma once
#include <vector>

namespace console {

enum class RowKind { Heading, Action, Value, Toggle, Slider, Text, Picture };

struct Row {
    RowKind kind;
    const wchar_t* label;     // exactly as on the console
    const wchar_t* value;     // Value: the text shown on the right; Slider: "" ; else ""
    bool on;                  // Toggle: state shown in the capture
    const wchar_t* note;      // description shown under the row, "" if none
    const char* id;           // stable ASCII id, e.g. "internet.test_connection"
    bool captured;            // false if the row could only partly be seen
};

struct Section {
    const wchar_t* name;
    const char* id;
    std::vector<Row> rows;
    bool complete;            // whole section seen in the capture
};

struct Menu {
    const wchar_t* title;
    std::vector<Section> sections;
};

const Menu& Settings();      // "Configuración de la consola"
const Menu& UserPage();      // "Página de <name>"
const Menu& GameOptions();   // the panel opened with + on a game

}  // namespace console
