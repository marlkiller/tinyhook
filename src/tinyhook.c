#include <mach/mach_init.h> // mach_task_self()
#include <mach/mach_vm.h>   // mach_vm_*
#include <stdlib.h>         // atexit()
#include <string.h>         // memcpy()

#ifndef COMPACT
#include <mach/mach_error.h> // mach_error_string()
#include <printf.h>          // fprintf()
#endif

#ifdef __x86_64__
#include "fde64/fde64.h"
#endif

#include "../include/tinyhook.h"

#define MB (1ll << 20)
#define GB (1ll << 30)

#ifdef __aarch64__
#define AARCH64_B     0x14000000 // b        +0
#define AARCH64_BL    0x94000000 // bl       +0
#define AARCH64_ADRP  0x90000011 // adrp     x17, 0
#define AARCH64_BR    0xd61f0220 // br       x17
#define AARCH64_BLR   0xd63f0220 // blr      x17
#define AARCH64_ADD   0x91000231 // add      x17, x17, 0
#define AARCH64_SUB   0xd1000231 // sub      x17, x17, 0

#define MAX_JUMP_SIZE 12

#elif __x86_64__
#define X86_64_CALL     0xe8       // call
#define X86_64_JMP      0xe9       // jmp
#define X86_64_JMP_RIP  0x000025ff // jmp      [rip]
#define X86_64_CALL_RIP 0x000015ff // call     [rip]
#define X86_64_MOV_RI64 0xb848     // mov      r64, m64
#define X86_64_MOV_RM64 0x8b48     // mov      r64, [r64]

#define MAX_JUMP_SIZE   14
#endif

int tiny_insert(void *address, void *destination, bool link) {
    size_t jump_size;
    int assembly;
    unsigned char bytes[MAX_JUMP_SIZE];
#ifdef __aarch64__
    // b/bl imm    ; go to destination
    jump_size = 4;
    assembly = (destination - address) >> 2 & 0x3ffffff;
    assembly |= link ? AARCH64_BL : AARCH64_B;
    *(int *)bytes = assembly;
#elif __x86_64__
    // jmp/call imm    ; go to destination
    jump_size = 5;
    *bytes = link ? X86_64_CALL : X86_64_JMP;
    assembly = (long)destination - (long)address - 5;
    *(int *)(bytes + 1) = assembly;
#endif
    write_mem(address, bytes, jump_size);
    return jump_size;
}

int tiny_insert_far(void *address, void *destination, bool link) {
    size_t jump_size;
    unsigned char bytes[MAX_JUMP_SIZE];
#ifdef __aarch64__
    // adrp    x17, imm
    // add     x17, x17, imm    ; x17 -> destination
    // br/blr  x17
    jump_size = 12;
    int assembly;
    assembly = (((long)destination >> 12) - ((long)address >> 12)) & 0x1fffff;
    assembly = ((assembly & 0x3) << 29) | (assembly >> 2 << 5) | AARCH64_ADRP;
    *(int *)bytes = assembly;
    assembly = ((long)destination & 0xfff) << 10 | AARCH64_ADD;
    *(int *)(bytes + 4) = assembly;
    *(int *)(bytes + 8) = link ? AARCH64_BLR : AARCH64_BR;
#elif __x86_64__
    jump_size = 14;
    // jmp [rip]    ; rip stored destination
    *(int *)bytes = link ? X86_64_CALL_RIP : X86_64_JMP_RIP;
    bytes[5] = bytes[6] = 0;
    *(long long *)(bytes + 6) = (long long)destination;
#endif
    write_mem(address, bytes, jump_size);
    return jump_size;
}

int position = 0;
mach_vm_address_t vm;

static int get_jump_size(void *address, void *destination);
static int insert_jump(void *address, void *destination);
static int save_header(void *address, void *destination, int *skip_len);

int tiny_hook(void *function, void *destination, void **origin) {
    int kr = 0;
    if (origin == NULL)
        insert_jump(function, destination);
    else {
        if (!position) {
            // alloc a vm to store headers and jumps
            kr = mach_vm_allocate(mach_task_self(), &vm, PAGE_SIZE, VM_FLAGS_ANYWHERE);
#ifndef COMPACT
            if (kr != 0) {
                fprintf(stderr, "mach_vm_allocate: %s\n", mach_error_string(kr));
            }
#endif
        }
        int skip_len;
        *origin = (void *)(vm + position);
        position += save_header(function, (void *)(vm + position), &skip_len);
        position += insert_jump((void *)(vm + position), function + skip_len);
        insert_jump(function, destination);
    }
    return kr;
}

static int get_jump_size(void *address, void *destination) {
    long long distance = destination > address ? destination - address : address - destination;
#ifdef __aarch64__
    return distance < 128 * MB ? 4 : 12;
#elif __x86_64__
    return distance < 2 * GB ? 5 : 14;
#endif
}

static int insert_jump(void *address, void *destination) {
    if (get_jump_size(address, destination) <= 5)
        return tiny_insert(address, destination, false);
    else
        return tiny_insert_far(address, destination, false);
}

static int save_header(void *address, void *destination, int *skip_len) {
    int header_len = 0;
#ifdef __aarch64__
    header_len = *skip_len = get_jump_size(address, destination);
    unsigned char bytes_out[MAX_JUMP_SIZE];
    read_mem(bytes_out, address, MAX_JUMP_SIZE);
    for (int i = 0; i < header_len; i += 4) {
        int cur_asm = *(int *)(bytes_out + i);
        long cur_addr = (long)address + i, cur_dst = (long)destination + i;
        if (((cur_asm ^ 0x90000000) & 0x9f000000) == 0) {
            // adrp
            // modify the immediate
            int len = (cur_asm >> 29 & 0x3) | ((cur_asm >> 3) & 0x1ffffc);
            len += (cur_addr >> 12) - (cur_dst >> 12);
            cur_asm &= 0x9f00001f;
            cur_asm = ((len & 0x3) << 29) | (len >> 2 << 5) | cur_asm;
            *(int *)(bytes_out + i) = cur_asm;
        }
    }
#elif __x86_64__
    int min_len;
    struct fde64s assembly;
    unsigned char bytes_in[MAX_JUMP_SIZE * 2], bytes_out[MAX_JUMP_SIZE * 4];
    read_mem(bytes_in, address, MAX_JUMP_SIZE * 2);
    min_len = get_jump_size(address, destination);
    for (*skip_len = 0; *skip_len < min_len; *skip_len += assembly.len) {
        long long cur_addr = (long long)address + *skip_len;
        decode(bytes_in + *skip_len, &assembly);
        if (assembly.opcode == 0x8B && assembly.modrm_rm == 0b101) {
            // mov r64, [rip+]
            // split it into 2 instructions
            // mov r64 $rip+(immediate)
            // mov r64 [r64]
            *(short *)(bytes_out + header_len) = X86_64_MOV_RI64;
            bytes_out[header_len + 1] += assembly.modrm_reg;
            *(long long *)(bytes_out + header_len + 2) = assembly.disp32 + cur_addr + assembly.len;
            header_len += 10;
            *(short *)(bytes_out + header_len) = X86_64_MOV_RM64;
            bytes_out[header_len + 2] = assembly.modrm_reg << 3 | assembly.modrm_reg;
            header_len += 3;
        } else {
            memcpy(bytes_out + header_len, bytes_in + *skip_len, assembly.len);
            header_len += assembly.len;
        }
    }
#endif
    write_mem(destination, bytes_out, header_len);
    return header_len;
}
