#define _GNU_SOURCE 1  /* For MAP_ANONYMOUS. */
#include <errno.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define MEM_SIZE (2 << 20)  /* In bytes. 2 MiB. */
#define BASE_PARA 0x500
#define CODE_LOAD_OFS (BASE_PARA << 4)  /* Also the stack grows down from here. */
/*#define GUEST_MEM_MODULE_START 0x1000*/
#define GUEST_MEM_MODULE_START 0x0

static void load_guest(const char *filename, void *mem) {
  char *p;
  const int img_fd = open(filename, O_RDONLY);
  if (img_fd < 0) {
    fprintf(stderr, "can not open binary file: %d\n", errno);
    exit(1);
  }
  /* !! Set up stack. */
  p = (char *)mem + CODE_LOAD_OFS;  /* !! Check for buffer overflow. */
  for (;;) {
    const int r = read(img_fd, p, 8192);
    if (r <= 0) {
      if (r < 0) {
        perror("read");
        exit(1);
      }
      break;
    }
    p += r;
  }
  close(img_fd);
}

static void dump_regs(struct kvm_regs *regs, struct kvm_sregs *sregs) {
#define R16(name) ((unsigned)regs->r##name & 0xffff)
#define S16(name) ((unsigned)sregs->name.selector & 0xffff)
  printf("regs: ax:%04x bx:%04x cx:%04x dx:%04x si:%04x di:%04x sp:%04x bp:%04x ip:%04x flags:%08x cs:%04x ds:%04x es:%04x fs:%04x gs:%04x ss:%04x\n",
         R16(ax), R16(bx), R16(cx), R16(dx), R16(si), R16(di), R16(sp), R16(bp), R16(ip), R16(flags),
         S16(cs), S16(ds), S16(es), S16(fs), S16(gs), S16(ss));
  fflush(stdout);
}

int main(int argc, char *argv[]) {
  int kvm_fd;
  int vm_fd;
  int vcpu_fd;
  void *mem;
  struct kvm_userspace_memory_region region;
  int kvm_run_mmap_size;
  struct kvm_run *run;
  struct kvm_regs regs;
  struct kvm_sregs sregs;
  unsigned sp;

  if (argc != 2) {
    fprintf(stderr, "Usage: %s <guest-image>\n", argv[0]);
    return 1;
  }

  if ((kvm_fd = open("/dev/kvm", O_RDWR)) < 0) {
    fprintf(stderr, "failed to open /dev/kvm: %d\n", errno);
    return 1;
  }

  if ((vm_fd = ioctl(kvm_fd, KVM_CREATE_VM, 0)) < 0) {
    fprintf(stderr, "failed to create vm: %d\n", errno);
    return 1;
  }

  if ((mem = mmap(NULL, MEM_SIZE - GUEST_MEM_MODULE_START, PROT_READ | PROT_WRITE,
		  MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0)) ==
      NULL) {
    fprintf(stderr, "mmap failed: %d\n", errno);
    return 1;
  }

  memset(&region, 0, sizeof(region));
  region.slot = 0;
  region.guest_phys_addr = GUEST_MEM_MODULE_START;  /* Must be a multiple of the page size (0x1000), otherwise KVM_SET_USER_MEMORY_REGION returns EINVAL. */
  region.memory_size = MEM_SIZE - GUEST_MEM_MODULE_START;
  region.userspace_addr = (uintptr_t)mem;
  /*region.flags = KVM_MEM_READONLY;*/  /* Not needed, default. */
  if (ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &region) < 0) {
    perror("ioctl KVM_SET_USER_MEMORY_REGION");
    return 1;
  }
  /* Any read/write outside these regions will trigger a KVM_EXIT_MMIO. */

  mem = (char*)mem - GUEST_MEM_MODULE_START;
  load_guest(argv[1], mem);

  if ((vcpu_fd = ioctl(vm_fd, KVM_CREATE_VCPU, 0)) < 0) {
    fprintf(stderr, "can not create vcpu: %d\n", errno);
    return 1;
  }
  kvm_run_mmap_size = ioctl(kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
  if (kvm_run_mmap_size < 0) {
    fprintf(stderr, "ioctl KVM_GET_VCPU_MMAP_SIZE: %d\n", errno);
    return 1;
  }
  run = (struct kvm_run *)mmap(
      NULL, kvm_run_mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu_fd, 0);
  if (run == NULL) {
    fprintf(stderr, "mmap kvm_run: %d\n", errno);
    return 1;
  }

  if (ioctl(vcpu_fd, KVM_GET_REGS, &regs) < 0) {
    perror("KVM_GET_REGS");
    return 1;
  }
  if (ioctl(vcpu_fd, KVM_GET_SREGS, &(sregs)) < 0) {
    perror("KVM_GET_SREGS");
    return 1;
  }

  /* Fill magic interrupt table. */
  { unsigned u;
    for (u = 0; u < 0x100; ++u) { ((unsigned*)mem)[u] = 0x400000 | u; }
    memset((char*)mem + 0x400, 0xf4, 256);  /* 256 hlt instructions, one for each int. */
  }

/* We have to set both selector and base, otherwise it won't work. A `mov
 * ds, ax' instruction in the 16-bit guest will set both.
 */
#define SET_SEGMENT_REG(name, para_value) do { sregs.name.base = (sregs.name.selector = para_value) << 4; } while(0)
  SET_SEGMENT_REG(cs, BASE_PARA);
  SET_SEGMENT_REG(ds, BASE_PARA);
  SET_SEGMENT_REG(es, BASE_PARA);
  SET_SEGMENT_REG(fs, BASE_PARA);
  SET_SEGMENT_REG(gs, BASE_PARA);

  SET_SEGMENT_REG(ss, 0);
  regs.rsp = sp = (CODE_LOAD_OFS >> 16 ? 1 << 16 : CODE_LOAD_OFS) - 2;
  memset((char*)mem + sp, 0, 2);  /* Push a 0 byte. */

  /* EFLAGS https://en.wikipedia.org/wiki/FLAGS_register */
  regs.rflags = 1 << 1;  /* Reserved bit. */
  regs.rip = 0;

  dump_regs(&regs, &sregs);

 set_sregs_regs_and_continue:
  if (ioctl(vcpu_fd, KVM_SET_SREGS, &sregs) < 0) {
    perror("KVM_SET_SREGS");
    return 1;
  }
  if (ioctl(vcpu_fd, KVM_SET_REGS, &regs) < 0) {
    perror("KVM_SET_REGS\n");
    return 1;
  }

  /* !! Trap it if it tries to enter protected mode (cr0 |= 1). Is this possible? */
  for (;;) {
    int ret = ioctl(vcpu_fd, KVM_RUN, 0);
    if (ret < 0) {
      fprintf(stderr, "KVM_RUN failed");
      return 1;
    }
    if (ioctl(vcpu_fd, KVM_GET_REGS, &regs) < 0) {
      perror("KVM_GET_REGS");
      return 1;
    }
    if (ioctl(vcpu_fd, KVM_GET_SREGS, &sregs) < 0) {
      perror("KVM_GET_REGS");
      return 1;
    }
    dump_regs(&regs, &sregs);

    switch (run->exit_reason) {
     case KVM_EXIT_IO:
      printf("IO port: port=0x%02x data=%04x size=%d direction=%d\n", run->io.port,
	     *(int *)((char *)(run) + run->io.data_offset),
	     run->io.size, run->io.direction);
      fflush(stdout);
      sleep(1);
      break;
     case KVM_EXIT_SHUTDOWN:  /* How do we trigger it? */
      printf("shutdown\n");
      goto exit;
     case KVM_EXIT_HLT:
      if ((unsigned)sregs.cs.selector == 0x40 && (unsigned)((unsigned)regs.rip - 1) < 0x100) {  /* hlt cause by int through our magic interrupt table */
        const unsigned char int_num = ((unsigned)regs.rip - 1) & 0xff;
        const unsigned short *csip_ptr = (const unsigned short*)((char*)mem + ((unsigned)sregs.ss.selector << 4) + ((unsigned)regs.rsp & 0xffff));
        const unsigned short int_ip = csip_ptr[0], int_cs = csip_ptr[1];  /* Return address. */  /* !! Security: check bounds, also check that rsp <= 0xfffe. */
        printf("int 0x%02x cs:%04x ip:%04x\n", int_num, int_cs, int_ip);
        /* Return from the interrupt. */
        SET_SEGMENT_REG(cs, int_cs);
        regs.rip = int_ip;
        goto set_sregs_regs_and_continue;
      } else {
        printf("hlt\n");
        goto exit;
      }
     case KVM_EXIT_MMIO:
      printf("mmio phys_addr=%08x value=%08x%08x size=%d is_write=%d\n", (unsigned)run->mmio.phys_addr, ((unsigned*)run->mmio.data)[1], ((unsigned*)run->mmio.data)[0], run->mmio.len, run->mmio.is_write);
      break;  /* Just continue at cs:ip. */
     default:
      printf("exit_reason: %d\n", run->exit_reason);
      fflush(stdout);
      goto exit;
    }
  }
exit:
  close(kvm_fd);
  return 0;
}
