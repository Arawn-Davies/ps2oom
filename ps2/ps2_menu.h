// Boot-time menus drawn on the libdebug text screen, driven by the controller.
#ifndef PS2_MENU_H
#define PS2_MENU_H

// Simple list picker: returns the chosen index (0 if no controller).
int PS2_SelectMenu(const char *title, char **items, int count);

// One settable option on the single-page setup screen: a label plus a list of
// value strings; .cur is the selected index (in and out).
typedef struct
{
    const char  *label;
    char       **values;
    int          count;
    int          cur;
} ps2_setting_t;

// Single-page settings menu: Up/Down pick a row, Left/Right change its value,
// Start/Cross confirms. Each setting's .cur is left at the chosen index. With
// no controller it returns immediately, leaving the defaults.
void PS2_SettingsMenu(const char *title, ps2_setting_t *settings, int n);

#endif // PS2_MENU_H
