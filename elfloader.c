/* Copyright © 2014, Owen Shepherd
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted without restriction.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include "elfload.h"
#include <inttypes.h>
#include <mach/arm/kern_return.h>
#include <mach/arm/vm_types.h>
#include <mach/mach_vm.h>
#include <mach/message.h>
#include <mach/task.h>
#include <mach/thread_act.h>
#include <mach/vm_map.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>

FILE *f;
// void *buf;
static el_status target_allocate_shared(el_ctx *ctx,
                                        mach_vm_address_t *target_addr,
                                        size_t size, mach_vm_address_t *addr,
                                        vm_prot_t prot);
typedef void (*entrypoint_t)(int (*putsp)(const char *));

static bool fpread(el_ctx *ctx, void *dest, size_t nb, size_t offset) {
  (void)ctx;

  if (fseek(f, offset, SEEK_SET))
    return false;

  if (fread(dest, nb, 1, f) != 1)
    return false;

  return true;
}
static void *alloccb(el_ctx *ctx, Elf_Addr phys, Elf_Addr virt, Elf_Addr size) {
  return (void *)ctx->sh_base_vaddr + virt;
}

// static void *alloccb_fix(el_ctx *ctx, Elf_Addr phys, Elf_Addr virt,
//                          Elf_Addr size) {
//   mach_vm_address_t loader_address;
//   el_status status = target_allocate_shared(ctx, virt, size, &loader_address,
//                                             VM_PROT_READ | VM_PROT_WRITE);
//   if (status != EL_OK) {
//     return NULL;
//   }
//   return (void *)loader_address;
// }
static void *alloccb_dyn(el_ctx *ctx, Elf_Addr phys, Elf_Addr virt,
                         Elf_Addr size) {
  return (void *)ctx->sh_base_vaddr + virt;
}

static void check(el_status stat, const char *expln) {
  if (stat) {
    fprintf(stderr, "%s: error %d\n", expln, stat);
    exit(1);
  }
}
static void print_thread_state(arm_thread_state64_t *state) {
  EL_DEBUG("pc: %llx\n", state->__pc);
  EL_DEBUG("lr: %llx\n", state->__lr);
  EL_DEBUG("sp: %llx\n", state->__sp);
  EL_DEBUG("fp: %llx\n", state->__fp);
  for (int i = 0; i < 29; i++) {
    EL_DEBUG("x%d: %llx\n", i, state->__x[i]);
  }
}
struct target_thread_t {
  thread_act_t thread;
  arm_thread_state64_t state;
};
typedef struct target_thread_t target_thread_t;
static el_status target_threads_state(el_ctx *ctx, target_thread_t **threads,
                                      mach_msg_type_number_t *thread_count) {
  kern_return_t kr;
  thread_act_array_t thread_ports;

  kr = task_threads(ctx->child_task, &thread_ports,
                    (mach_msg_type_number_t *)thread_count);
  if (kr) {
    fprintf(stderr, "task_threads failed: %s\n", mach_error_string(kr));
    return EL_MACH;
  }
  *threads = malloc(*thread_count * sizeof(target_thread_t));

  for (int i = 0; i < *thread_count; i++) {
    thread_act_t thread = thread_ports[i];
    arm_thread_state64_t state;
    mach_msg_type_number_t state_count = ARM_THREAD_STATE64_COUNT;
    kr = thread_get_state(thread, ARM_THREAD_STATE64, (thread_state_t)&state,
                          &state_count);
    if (kr) {
      fprintf(stderr, "thread_get_state failed: %s\n", mach_error_string(kr));
      return EL_MACH;
    }
    EL_DEBUG("==============task %d============\n", thread);
    print_thread_state(&state);
    (*threads)[i].thread = thread;
    (*threads)[i].state = state;
  }
  return EL_OK;
}
static el_status target_set_state(el_ctx *ctx, target_thread_t *thread) {

  kern_return_t kr = thread_set_state(thread->thread, ARM_THREAD_STATE64,
                                      (thread_state_t) & (thread->state),
                                      ARM_THREAD_STATE64_COUNT);
  if (kr) {
    fprintf(stderr, "thread_set_state failed: %s\n", mach_error_string(kr));
    return EL_MACH;
  }
  return EL_OK;
}

static void go(el_ctx *ctx, entrypoint_t ep) {
  target_thread_t *threads;
  unsigned int thread_count;
  check(target_threads_state(ctx, &threads, &thread_count),
        "get task thread state");
  if (thread_count == 0) {
    fprintf(stderr, "no thread found\n");
    exit(1);
  }

  arm_thread_state64_t *state = &threads[0].state;
  state->__pc = (uint64_t)ep;
  // truncate the stack page
  printf("page mask %lx\n", PAGE_MASK);
  state->__sp = (state->__sp & PAGE_MASK);
  check(target_set_state(ctx, threads), "set thread state");

  kern_return_t kr= task_resume(ctx->child_task);
  if (kr) {
    fprintf(stderr, "thread_resume failed: %s\n", mach_error_string(kr));
    exit(1);
  }
  // kr=thread_resume(threads[0].thread);
  // if (kr) {
  //   fprintf(stderr, "thread_resume failed: %s\n", mach_error_string(kr));
  //   exit(1);
  // }
}
/**
 * @brief allocate memory in target process with fix address
 * target address must align with page size otherwise it will be truncated
 * @param ctx
 * @param target_addr target address
 * @param size size of memory
 * @param addr shared address
 * @param prot protection
 * @return el_status
 */
static el_status target_allocate_shared(el_ctx *ctx,
                                        mach_vm_address_t *target_addr,
                                        size_t size, mach_vm_address_t *addr,
                                        vm_prot_t prot) {
  kern_return_t kr;

  kr = mach_vm_map(ctx->loader_task, addr, size, 0, VM_FLAGS_ANYWHERE, 0, 0,
                   FALSE, VM_PROT_DEFAULT, VM_PROT_ALL, VM_INHERIT_NONE);

  if (kr) {
    fprintf(stderr, "vm_allocate failed: %s\n", mach_error_string(kr));
    return EL_MACH;
  }
  mach_vm_address_t target_out_addr = *target_addr;
  vm_prot_t target_max_prot = VM_PROT_ALL;
  vm_prot_t target_cur_prot = prot;

  kr = mach_vm_remap(ctx->child_task, &target_out_addr, size, 0,
                     VM_FLAGS_OVERWRITE | VM_FLAGS_FIXED, ctx->loader_task,
                     *addr, FALSE, &target_cur_prot, &target_max_prot,
                     VM_INHERIT_NONE);
  if (kr) {
    fprintf(stderr, "mach_vm_remap failed: %s\n", mach_error_string(kr));
    return EL_MACH;
  }
  if (target_cur_prot != prot) {
    fprintf(stderr, "mach prot failed\n");
    return EL_MACH;
  }
  printf("target_allocate_shared: request address %llx size %lx actual address "
         "%llx shared "
         "address "
         "%llx\n",
         *target_addr, size, target_out_addr, *addr);
  *target_addr = target_out_addr;
  return EL_OK;
}
static el_status target_write(el_ctx *ctx, void *addr, void *buf, size_t size) {
  kern_return_t kr;
  kr = mach_vm_write(ctx->child_task, (mach_vm_address_t)addr, (vm_offset_t)buf,
                     size);
  if (kr) {
    fprintf(stderr, "mach_vm_write failed: %s\n", mach_error_string(kr));
    return EL_MACH;
  }
  return EL_OK;
}
static int target_protect(el_ctx *ctx, mach_vm_address_t vaddr, size_t size,
                          int prot) {
  mach_vm_address_t addr = vaddr + ctx->target_base_vaddr;
  kern_return_t kr;
  kr = mach_vm_protect(ctx->child_task, addr, size, FALSE, prot);
  if (kr) {
    fprintf(stderr, "mach_vm_protect failed: %s\n", mach_error_string(kr));
    return -1;
  }
  return 0;
}
static int target_free(void *addr, size_t size) { return EL_NOTIMPL; }
static int target_read(void *addr, void *buf, size_t size) { return 0; }

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s [elf-to-load]\n", argv[0]);
    return 1;
  }

  f = fopen(argv[1], "rb");
  if (!f) {
    perror("opening file");
    return 1;
  }
  el_ctx ctx;
  memset(&ctx, 0, sizeof ctx);
  ctx.pread = fpread;
  check(el_init(&ctx), "initialising");
  // ctx.base_load_paddr = ctx.base_load_vaddr = 0;

  if (ctx.ehdr.e_type == ET_DYN) {
    ctx.target_base_vaddr = 0x100000000;

    check(target_allocate_shared(&ctx, &ctx.target_base_vaddr, ctx.memsz,
                                 (mach_vm_address_t *)(&ctx.sh_base_vaddr),
                                 VM_PROT_READ | VM_PROT_WRITE),
          "allocate target memory");
    ctx.base_paddr = ctx.sh_base_vaddr;
  } else {
  }

  check(el_load(&ctx, alloccb), "loading");
  check(el_relocate(&ctx), "relocating");
  check(el_perm(&ctx, target_protect), "setting permissions");
  uintptr_t epaddr = ctx.ehdr.e_entry + ctx.target_base_vaddr;
  entrypoint_t ep = (entrypoint_t)epaddr;
  printf("Binary entrypoint is %" PRIxPTR "; invoking %p\n",
         (uintptr_t)ctx.ehdr.e_entry, ep);
  go(&ctx, ep);
  waitpid(ctx.child_pid, NULL, 0);
  fclose(f);
  mach_vm_deallocate(ctx.loader_task, (mach_vm_address_t)ctx.sh_base_vaddr,
                     ctx.memsz);

  return 0;
}
