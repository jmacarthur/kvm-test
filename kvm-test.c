#include <linux/kvm.h>
#include <stdio.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>


const unsigned int MEMSIZE = 1024*1024;

typedef struct _vminfo {
  int devfd;
  int vmfd;
  int vcpufd;
  struct kvm_run *runData;
  size_t runDataSize;
} vminfo;

int configureMemory(vminfo *vm, char* buffer)
{
  int res;
  printf("Attempting to setup memory at host address %p\n",buffer);
  struct kvm_userspace_memory_region memory;
  memory.slot = 0;
  memory.flags = 0;
  memory.memory_size = MEMSIZE; // Must be a multiple of PAGE_SIZE
  memory.guest_phys_addr = 0xFFF00000; // Must be aligned to PAGE_SIZE
  memory.userspace_addr = (__u64) buffer;
  res = ioctl(vm->vmfd, KVM_SET_USER_MEMORY_REGION, &memory);
  printf("KVM_SET_USER_MEMORY_REGION result: %d\n",res);
}

int initvm(vminfo* vm)
{
  vm->vmfd = ioctl(vm->devfd, KVM_CREATE_VM, 0); 
  printf("Created a KVM FD: %d\n",vm->vmfd);
}
 
int initcpu(vminfo* vm)
{
  // Try to create a vcpu
  vm->vcpufd = ioctl(vm->vmfd, KVM_CREATE_VCPU, 0);
 
  printf("KVM_CREATE_VCPU: %d\n",vm->vcpufd);
  if(vm->vcpufd == -1) {
    exit(1);
  } 

  int mmapSize = ioctl(vm->devfd, KVM_GET_VCPU_MMAP_SIZE, 0);
  printf("VCPU mmap size is %d bytes\n",mmapSize); 

  void* vcpuMap = mmap(0, mmapSize, PROT_READ|PROT_WRITE, MAP_SHARED,
                       vm->vcpufd, 0);
  struct kvm_run *kvmRun = (struct kvm_run *) vcpuMap;
  kvmRun->request_interrupt_window = 1;
  vm->runData = kvmRun;
  vm->runDataSize = mmapSize;
}

void getRegs(int vcpufd, struct kvm_regs *regs)
{
  int res = ioctl(vcpufd, KVM_GET_REGS, regs);
  if(res != 0) {
    printf("Can't GET_REGS from VCPU\n");
    exit(1);
  }
}

void setRegs(vminfo* vm)
{
  int res;
  struct kvm_sregs sregs;
  res = ioctl(vm->vcpufd, KVM_GET_SREGS, &sregs);
  if(res == 0) {
    printf("Starting CS: Base 0x%llx Limit 0x%x Selector 0x%hx\n",
           sregs.cs.base, sregs.cs.limit, sregs.cs.selector);
  } else {
    printf("Can't GET_SREGS from VCPU\n");
    exit(1);
  }

  // Set CS base due to bug in early KVM
  sregs.cs.base = 0xFFFF0000;
  res = ioctl(vm->vcpufd, KVM_SET_SREGS, &sregs);
  if(res == 0) {
    printf("Corrected CS: Base 0x%llx Limit 0x%x Selector 0x%hx\n",
           sregs.cs.base, sregs.cs.limit, sregs.cs.selector);
  } else {
    printf("Can't SET_SREGS to VCPU\n");
    exit(1);
  }
}

void* vmStopper(void* args)
{
  sleep(1);
  printf("Sending interrupt to VCPU\n");
  vminfo *vm1 = (vminfo*) args;

  struct kvm_interrupt interruptStuff;
  interruptStuff.irq = 3;
  int res = ioctl(vm1->vcpufd, KVM_INTERRUPT, &interruptStuff);
  if(res == 0) {
    printf("Interrupted successfully\n");
  } else {
    printf("KVM_INTERRUPT failed!\n");
    exit(1);
  }

  return 0;
}

int main()
{
  int res, i, j;
  int randomiseProgram = 1;
  pthread_t threadInfo;
  pthread_attr_t threadAttr;
  unsigned char* buffer;
  void *returnStruct;

  int fd = open("/dev/kvm", O_RDWR);
  if(fd < 0) {
    printf("Can't open /dev/kvm\n");
    exit(1);
  }
  printf("/dev/kvm open with FD %d\n",fd);
 
  int apiVersion = ioctl(fd, KVM_GET_API_VERSION, 0);
  printf("KVM API version is %d\n",apiVersion);

  buffer = mmap(0, MEMSIZE, PROT_READ|PROT_WRITE|PROT_EXEC,
                MAP_SHARED | MAP_ANONYMOUS, 0, 0);
  if(buffer==MAP_FAILED) {
    printf("mmap of memory failed.\n");
    exit(2);
  }
  memset(buffer, 0x90, MEMSIZE); // Set all to NOP
  
  vminfo vm1;
  memset(&vm1, 0, sizeof(vminfo));
  vm1.devfd = fd;
  initvm(&vm1);
  initcpu(&vm1);
  
  // Randomise memory
  if(randomiseProgram) {
    for(i=0;i<MEMSIZE;i++) {
      *(buffer + i) = rand() & 0xFF;
     }
  }

  // Make a simple loop program
  int programOffset = 0xFFFF0;
  buffer[programOffset+0] = 0xFF; // INC EAX
  buffer[programOffset+1] = 0xC0;
  buffer[programOffset+2] = 0xEB; // JMP -2
  buffer[programOffset+3] = 0xFC;
    
  configureMemory(&vm1, buffer);
  setRegs(&vm1);

  pthread_attr_init(&threadAttr);
  int stopThread = pthread_create(&threadInfo, &threadAttr,
				  vmStopper, (void*) &vm1);
  
  int stopRes = ioctl(vm1.vcpufd, KVM_RUN, 0);
  printf("Stopped with code %d\n",stopRes);
  unsigned int stopReason = vm1.runData->exit_reason;
  printf("exit_reason %u\n",stopReason);
  
  if(stopReason == KVM_EXIT_INTERNAL_ERROR) {
    printf("Suberror code %d\n",vm1.runData->internal.suberror);
  }
  
  struct kvm_regs regs;
  getRegs(vm1.vcpufd, &regs);
  printf("Finishing IP: 0x%llx RAX: 0x%llx\n",regs.rip, regs.rax);

  munmap(vm1.runData, vm1.runDataSize);
  close(vm1.vcpufd);
  close(vm1.vmfd);
  pthread_join(threadInfo, &returnStruct);
  pthread_attr_destroy(&threadAttr);

  munmap(buffer, MEMSIZE);
  close(fd);
}
