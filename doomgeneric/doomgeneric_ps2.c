#include "doomkeys.h"

#include "doomgeneric.h"

#include <ctype.h>
#include <unistd.h>
#include <sys/time.h>
//#include <sifrpc_common.h>

#include <stdio.h>
#include <tamtypes.h>
#include <sifcmd.h>
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <string.h>
#include <malloc.h>
#include <libhdd.h>
#include <libmc.h>
#include <libpad.h>
#include <sys/stat.h>
#include <iopheap.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <iopcontrol.h>
#include <stdarg.h>
#include <sbv_patches.h>
#include <slib.h>
#include <smem.h>
#include <smod.h>
#include <sys/fcntl.h>
#include <debug.h>
#include <gsKit.h>
#include <dmaKit.h>
#include <libcdvd.h>
#include <libjpg_ps2_addons.h>
#include <libkbd.h>
#include <math.h>
#include <usbhdfsd-common.h>

#include <sio.h>
#include <sior_rpc.h>


#define KEYQUEUE_SIZE 16


#ifdef SIO_DEBUG
#define DPRINTF(args...) sio_printf(args)
#else
#define DPRINTF(args...) printf(args)
#endif

extern u8 iomanx_irx[];
extern int size_iomanx_irx;
extern u8 filexio_irx[];
extern int size_filexio_irx;
extern u8 ps2dev9_irx[];
extern int size_ps2dev9_irx;
extern u8 ps2ip_irx[];
extern int size_ps2ip_irx;
extern u8 netman_irx[];
extern int size_netman_irx;
extern u8 ps2smap_irx[];
extern int size_ps2smap_irx;
extern u8 ps2host_irx[];
extern int size_ps2host_irx;
#ifdef SMB
extern u8 smbman_irx[];
extern int size_smbman_irx;
#endif
extern u8 vmc_fs_irx[];
extern int size_vmc_fs_irx;
extern u8 ps2ftpd_irx[];
extern int size_ps2ftpd_irx;
extern u8 ps2atad_irx[];
extern int size_ps2atad_irx;
extern u8 ps2hdd_irx[];
extern int size_ps2hdd_irx;
extern u8 ps2fs_irx[];
extern int size_ps2fs_irx;
extern u8 poweroff_irx[];
extern int size_poweroff_irx;
extern u8 ps2netfs_irx[];
extern int size_ps2netfs_irx;
extern u8 iopmod_irx[];
extern int size_iopmod_irx;
extern u8 usbd_irx[];
extern int size_usbd_irx;
extern u8 bdm_irx[];
extern int size_bdm_irx;
extern u8 bdmfs_fatfs_irx[];
extern int size_bdmfs_fatfs_irx;
extern u8 usbmass_bd_irx[];
extern int size_usbmass_bd_irx;
extern u8 cdfs_irx[];
extern int size_cdfs_irx;
extern u8 ps2kbd_irx[];
extern int size_ps2kbd_irx;
extern u8 hdl_info_irx[];
extern int size_hdl_info_irx;
extern u8 mcman_irx[];
extern int size_mcman_irx;
extern u8 mcserv_irx[];
extern int size_mcserv_irx;
#ifdef SIO_DEBUG
extern u8 sior_irx[];
extern int size_sior_irx;
#endif
extern u8 dvrdrv_irx[];
extern int size_dvrdrv_irx;
extern u8 dvrfile_irx[];
extern int size_dvrfile_irx;

//#define DEBUG
#ifdef DEBUG
#define dbgprintf(args...) scr_printf(args)
#define dbginit_scr()      init_scr()
#else
#define dbgprintf(args...) \
    do {                   \
    } while (0)
#define dbginit_scr() \
    do {              \
    } while (0)
#endif

enum {
    BUTTON,
    DPAD
};



static unsigned short s_KeyQueue[KEYQUEUE_SIZE];
static unsigned int s_KeyQueueWriteIndex = 0;
static unsigned int s_KeyQueueReadIndex = 0;

static unsigned char convertToDoomKey(unsigned int key)
{
	switch (key)
	{
/*
    case XK_Return:
		key = KEY_ENTER;
		break;
    case XK_Escape:
		key = KEY_ESCAPE;
		break;
    case XK_Left:
		key = KEY_LEFTARROW;
		break;
    case XK_Right:
		key = KEY_RIGHTARROW;
		break;
    case XK_Up:
		key = KEY_UPARROW;
		break;
    case XK_Down:
		key = KEY_DOWNARROW;
		break;
    case XK_Control_L:
    case XK_Control_R:
		key = KEY_FIRE;
		break;
    case XK_space:
		key = KEY_USE;
		break;
    case XK_Shift_L:
    case XK_Shift_R:
		key = KEY_RSHIFT;
		break;
*/
	default:
		key = tolower(key);
		break;
	}

	return key;
}

static void loadBasicModules(void)
{
    int ret;

    SifExecModuleBuffer(iomanx_irx, size_iomanx_irx, 0, NULL, &ret);
    SifExecModuleBuffer(filexio_irx, size_filexio_irx, 0, NULL, &ret);

    #warning "need to fix for PSX DVR platform"
    //SifExecModuleBuffer(allowdvdv_irx, size_allowdvdv_irx, 0, NULL, &ret);  // unlocks cdvd for reading on psx dvr

    SifLoadModule("rom0:SIO2MAN", 0, NULL);

#ifdef SIO_DEBUG
    int id;
    // I call this just after SIO2MAN have been loaded
    sio_init(38400, 0, 0, 0, 0);
    DPRINTF("Hello from EE SIO!\n");

    SIOR_Init(0x20);

    id = SifExecModuleBuffer(sior_irx, size_sior_irx, 0, NULL, &ret);
    scr_printf("\t sior id=%d _start ret=%d\n", id, ret);
    DPRINTF("sior id=%d _start ret=%d\n", id, ret);
#endif

    SifExecModuleBuffer(mcman_irx, size_mcman_irx, 0, NULL, &ret);  // Home
    // SifLoadModule("rom0:MCMAN", 0, NULL); //Sony
    SifExecModuleBuffer(mcserv_irx, size_mcserv_irx, 0, NULL, &ret);  // Home
    // SifLoadModule("rom0:MCSERV", 0, NULL); //Sony
    SifLoadModule("rom0:PADMAN", 0, NULL);
}


static void addKeyToQueue(int pressed, unsigned int keyCode)
{
	unsigned char key = convertToDoomKey(keyCode);

	unsigned short keyData = (pressed << 8) | key;

	s_KeyQueue[s_KeyQueueWriteIndex] = keyData;
	s_KeyQueueWriteIndex++;
	s_KeyQueueWriteIndex %= KEYQUEUE_SIZE;
}

void DG_Init()
{

}


void DG_DrawFrame()
{

}

void DG_SleepMs(uint32_t ms)
{

}

uint32_t DG_GetTicksMs()
{

}

int DG_GetKey(int* pressed, unsigned char* doomKey)
{

}

void DG_SetWindowTitle(const char * title)
{

}

int main(int argc, char **argv)
{
	loadBasicModules();
    printf("Hello, World!");
    while(1);
    doomgeneric_Create(argc, argv);

    while(1)
    {
      doomgeneric_Tick(); 
    }

    return 0;
}
