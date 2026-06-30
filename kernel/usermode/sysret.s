.global sysret_to_user

sysret_to_user:
    ; RAX = user RIP, RDX = user RSP
    mov rsp, rdx
    sysret
