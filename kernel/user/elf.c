#include "elf.h"
#include "../fs/ramfs.h"
#include "../scheduler/process.h"
#include "../ui/console.h"

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} elf64_hdr_t;

int elf_load_and_run(const char *name)
{
    const char *data = ramfs_read_file(name);
    if (!data) return -1;

    const elf64_hdr_t *hdr = (const elf64_hdr_t *)data;
    if (hdr->e_ident[0] != 0x7F ||
        hdr->e_ident[1] != 'E' ||
        hdr->e_ident[2] != 'L' ||
        hdr->e_ident[3] != 'F')
        return -1;

    uint64_t entry = hdr->e_entry;
    process_t *p = process_create(entry);
    if (!p) return -1;

    return 0;
}
