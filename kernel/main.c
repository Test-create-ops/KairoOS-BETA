#include "fs/vfs.h"
#include "initrd/initrd.c"
#include "elf/elf.c"
#include "sched/sched.c"
#include "syscall/syscall.c"
#include "smp/smp.c"
#include "drivers/input/keyboard.c"
#include "drivers/input/mouse.c"
#include "drivers/pci/pci.c"
#include "drivers/ahci/ahci.c"
#include "drivers/net/rtl8139.c"
#include "net/tcpip.c"
#include "drivers/usb/usb.c"
#include "drivers/audio/ac97.c"
#include "drivers/gpu/vbe.c"
#include "ui/wm/wm_adv.c"
#include "lib/framebuffer.h"
#include "lib/memory.h"
#include "proc/proc.h"

void kernel_main_legacy(void) {
    fb_init();
    fb_write("Kernel avviato\n");

    pci_scan();
    ahci_init();
    rtl8139_init();
    usb_init();
    ac97_init();
    vbe_init();

    keyboard_init();
    mouse_init();

    initrd_load();

    sched_init();
    smp_init();

    fb_write("Avvio userspace...\n");
    elf_load_and_exec("/bin/init");

    while (1) {
        wm_draw();
    }
}
