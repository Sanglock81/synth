#include "PluginEditor.h"

// The R2 editor is header-only (all layout + behaviour is inline in
// PluginEditor.h). Preset load/save/random now lives in the top bar (UI/TopBar.h);
// this translation unit exists so the build has an object file for the editor and
// a stable place for any future out-of-line editor code.
