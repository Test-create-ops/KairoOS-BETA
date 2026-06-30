#include "../fs/ramfs.h"
#include "../scheduler/process.h"
#include "../memory/heap.h"
#include "../ui/console.h"
#include <stdint.h>

typedef struct {
    unsigned char ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
} elf64_hdr_t;

int elf_load_and_run(const char *name)
{
    const char *data = ramfs_read_file(name);
    if (!data) return -1;

    const elf64_hdr_t *hdr = (const elf64_hdr_t *)data;

    if (hdr->ident[0] != 0x7F || hdr->ident[1] != 'E' ||
        hdr->ident[2] != 'L' || hdr->ident[3] != 'F') {
        console_write("ELF header non valido.\n");
        return -1;
    }

    uint64_t entry = hdr->entry;

    process_t *p = process_create(entry);
    if (!p) {
        console_write("Impossibile creare processo.\n");
        return -1;
    }

    return 0;
}
