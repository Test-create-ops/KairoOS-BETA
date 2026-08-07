#include "elf.h"
#include "../fs/vfs.h"
#include "../mmu/mmu.h"
#include "../proc/proc.h"
#include "../lib/memory.h"
#include "../lib/string.h"

int elf_load_and_exec(const char *path)
{
    vfs_node_t *node = vfs_open(path);
    if (!node) return -1;

    long size = vfs_size(node);
    if (size <= 0) return -1;

    void *file_buf = kmalloc(size);
    vfs_read(node, file_buf, size, 0);

    Elf64_Ehdr *eh = (Elf64_Ehdr *)file_buf;

    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F') {
        kfree(file_buf);
        return -2;
    }

    Elf64_Phdr *ph = (Elf64_Phdr *)((unsigned char *)file_buf + eh->e_phoff);

    struct proc *p = proc_create();
    proc_setup_address_space(p);

    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != 1) continue;

        void *seg = alloc_user_region(p, ph[i].p_vaddr, ph[i].p_memsz);

        memcpy(seg,
               (unsigned char *)file_buf + ph[i].p_offset,
               ph[i].p_filesz);

        memset((unsigned char *)seg + ph[i].p_filesz, 0,
               ph[i].p_memsz - ph[i].p_filesz);
    }

    proc_set_entry(p, eh->e_entry);
    proc_set_user_stack(p);

    kfree(file_buf);

    proc_run(p);
    return 0;
}
