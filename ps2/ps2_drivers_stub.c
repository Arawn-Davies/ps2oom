// Override libps2_drivers' USB + dev9 bring-up with no-ops.
//
// SDL2main's main() calls init_ps2_filesystem_driver() before ours, which
// loads the whole storage stack -- USB mass storage and dev9 included. With no
// real hardware present those enumerate against a long timeout (~28 s) on boot.
// We don't need USB or dev9 (the WAD is embedded / on host:), so stub them out;
// the linker prefers these definitions over the library's (same trick as
// ps2_audio_driver.c overriding init_audio_driver).

#include <stdbool.h>
#include <ps2_usb_driver.h>
#include <ps2_dev9_driver.h>

// THE boot delay: SDL2main calls waitUntilDeviceIsReady(boot path) after the
// filesystem init, and with an embedded/host: WAD the device it polls for never
// reports ready, so it spins out a ~28 s timeout (the EE loop PCSX2 logged at
// 0x00205FC0, inside this function). We don't wait on any device, so return
// ready immediately.
bool waitUntilDeviceIsReady(char *path)
{
    (void) path;
    return true;
}

enum USB_INIT_STATUS init_usb_driver(bool init_dependencies)
{
    (void) init_dependencies;
    return USB_INIT_STATUS_OK;
}

void deinit_usb_driver(bool deinit_dependencies)
{
    (void) deinit_dependencies;
}

enum DEV9_INIT_STATUS init_dev9_driver(void)
{
    return DEV9_INIT_STATUS_OK;
}

void deinit_dev9_driver(void)
{
}
