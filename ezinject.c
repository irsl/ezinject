#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <sys/shm.h>

#define CHECK(x) ({\
long _tmp = (x);\
DBG("%s = %lu", #x, _tmp);\
_tmp;})

#include "util.h"
#include "ezinject_injcode.h"

enum verbosity_level verbosity = V_DBG;

#if defined(__arm__)
#define regs user_regs
#define REG_PC uregs[15]
#define REG_NR uregs[7]
#define REG_RET uregs[0]
#define REG_ARG1 uregs[0]
#define REG_ARG2 uregs[1]
#define REG_ARG3 uregs[2]
#define REG_ARG4 uregs[3]
#define REG_ARG5 uregs[4]
#define REG_ARG6 uregs[5]
const char SYSCALL_INSN[] = {0x00, 0x00, 0x00, 0xef}; /* swi 0 */
const char RET_INSN[] = {0x04, 0xf0, 0x9d, 0xe4}; /* pop {pc} */
#elif defined(__i386__)
#define regs user_regs_struct
#define REG_PC eip
#define REG_NR eax
#define REG_RET eax
#define REG_ARG1 ebx
#define REG_ARG2 ecx
#define REG_ARG3 edx
#define REG_ARG4 esi
#define REG_ARG5 edi
#define REG_ARG6 ebp
const char SYSCALL_INSN[] = {0xcd, 0x80}; /* int 0x80 */
const char RET_INSN[] = {0xc3}; /* ret */
#elif defined(__amd64__)
#define regs user_regs_struct
#define REG_PC rip
#define REG_NR rax
#define REG_RET rax
#define REG_ARG1 rdi
#define REG_ARG2 rsi
#define REG_ARG3 rdx
#define REG_ARG4 r10
#define REG_ARG5 r8
#define REG_ARG6 r9
const char SYSCALL_INSN[] = {0x0f, 0x05}; /* syscall */
const char RET_INSN[] = {0xc3}; /* ret */
#else
#error "Unsupported architecture"
#endif

#ifndef __NR_mmap
#define __NR_mmap __NR_mmap2 /* Functionally equivalent for our use case. */
#endif

#define MAPPINGSIZE 4096
#define MEMALIGN 4 /* MUST be a power of 2 */
#define ALIGNMSK ~(MEMALIGN-1)

#define ALIGN(x) ((void *)(((uintptr_t)x + MEMALIGN) & ALIGNMSK))

#define CLONE_FLAGS (CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_SIGHAND|CLONE_PARENT|CLONE_THREAD|CLONE_IO)

typedef struct {
	uintptr_t base_remote;
	uintptr_t base_local;
} ez_addr;

#define EZ_LOCAL(ref, remote) (ref.base_local + (((uintptr_t)remote) - ref.base_remote))
#define EZ_REMOTE(ref, local) (ref.base_remote + (((uintptr_t)local) - ref.base_local))


uintptr_t remote_syscall(pid_t target, void *syscall_addr, int nr, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4)
{
	struct regs orig_regs, new_regs;
	ptrace(PTRACE_GETREGS, target, 0, &orig_regs);
	memcpy(&new_regs, &orig_regs, sizeof(struct regs));
	new_regs.REG_PC = (uintptr_t)syscall_addr;
	new_regs.REG_NR = nr;
	new_regs.REG_ARG1 = arg1;
	new_regs.REG_ARG2 = arg2;
	new_regs.REG_ARG3 = arg3;
	new_regs.REG_ARG4 = arg4;
	/*new_regs.REG_ARG5 = arg5;
	new_regs.REG_ARG6 = arg6;*/
	ptrace(PTRACE_SETREGS, target, 0, &new_regs);
	ptrace(PTRACE_SYSCALL, target, 0, 0); /* Run until syscall entry */
	waitpid(target, 0, 0);
	ptrace(PTRACE_SYSCALL, target, 0, 0); /* Run until syscall return */
	waitpid(target, 0, 0);
	ptrace(PTRACE_GETREGS, target, 0, &new_regs); /* Get return value */
	ptrace(PTRACE_SETREGS, target, 0, &orig_regs);
	DBG("remote_syscall(%d) = %zu", nr, (uintptr_t)new_regs.REG_RET);
	return new_regs.REG_RET;
}

void *locate_gadget(uint8_t *base, size_t limit, uint8_t *search, size_t searchSz){
	for(size_t i = 0; i < limit; ++i)
	{
		if(!memcmp(&base[i], search, searchSz))
		{
			return (void *)&base[i];
		}
	}
	return NULL;
}

int main(int argc, char *argv[])
{
	char buf[128];
	char sopath[PATH_MAX];
	int err = 0;
	if(argc != 3)
	{
		ERR("Usage: %s pid library-to-inject", argv[0]);
		return 1;
	}
	pid_t target = atoi(argv[1]);
	snprintf(buf, 128, "/proc/%u/exe", target);

	if(!realpath(argv[2], sopath))
	{
		PERROR("realpath");
		return 1;
	}

	int shm_id;
	if((shm_id = shmget(target, MAPPINGSIZE, IPC_CREAT | IPC_EXCL | S_IRWXO)) < 0){
		PERROR("shmget");
		return 1;
	}

	ez_addr libc = {
		.base_local  = (uintptr_t) get_base(getpid(), "libc-"),
		.base_remote = (uintptr_t) get_base(target, "libc-")
	};

	DBGPTR(libc.base_remote);
	DBGPTR(libc.base_local);
	
	if(!libc.base_local || !libc.base_remote)
	{
		ERR("Failed to get libc base");
		return 1;
	}

	ez_addr libc_syscall = {
		.base_local  = (uintptr_t)&syscall,
		.base_remote = EZ_REMOTE(libc, &syscall)
	};

	ez_addr libc_syscall_insn = {
		.base_local = (uintptr_t)locate_gadget(
			(uint8_t *)libc_syscall.base_local, 0x1000,
			(uint8_t *)SYSCALL_INSN, sizeof(SYSCALL_INSN)
		),
	};
	libc_syscall_insn.base_remote = EZ_REMOTE(libc, libc_syscall_insn.base_local);

	if(!libc_syscall_insn.base_local)
	{
		ERR("Failed to find syscall instruction in libc");
		err = 1;
		return 1;
	}

	DBGPTR(libc_syscall_insn.base_local);
	
	CHECK(ptrace(PTRACE_ATTACH, target, 0, 0));

	usleep(100);

	#define REMOTE_SC(nr, arg0, arg1, arg2, arg3) remote_syscall(target, (void *)libc_syscall_insn.base_remote, nr, arg0, arg1, arg2, arg3)
	
	/* Verify that remote_syscall works correctly */
	pid_t remote_pid = REMOTE_SC(__NR_getpid, 0, 0, 0, 0);
	if(remote_pid != target)
	{
		ERR("Remote syscall returned incorrect result!");
		ERR("Expected: %u, actual: %u", target, remote_pid);
		err = 1;
		goto out_ptrace;
	}

	char *mapped_mem = shmat(shm_id, NULL, SHM_EXEC);
	if(mapped_mem == MAP_FAILED){
		PERROR("shmat");
		err = 1;
		goto out_ptrace;
	}
	
	size_t injected_size = (size_t)(injected_code_end - (uintptr_t)injected_code);
	
	DBG("injsize=%zu", injected_size);
	
	/* Copy code */
	memcpy(mapped_mem, injected_code, injected_size);
	
	/* Install syscall->ret gadget (will be used when creating thread) */
	char *syscall_ret_gadget = ALIGN(mapped_mem + injected_size);
	DBGPTR(syscall_ret_gadget);
	
	// copy 'syscall' insn
	memcpy(syscall_ret_gadget, (void*)SYSCALL_INSN, sizeof(SYSCALL_INSN));
	usleep(100);
	DBGPTR(syscall_ret_gadget + sizeof(SYSCALL_INSN));

	// copy 'ret' insn
	memcpy(syscall_ret_gadget + sizeof(SYSCALL_INSN), (void*)RET_INSN, sizeof(RET_INSN));
	char *syscall_ret_gadget_end = syscall_ret_gadget + sizeof(SYSCALL_INSN) + sizeof(RET_INSN);

	/* Help the new thread get its bearings */
	char *my_libc_dlopen_mode = dlsym(RTLD_DEFAULT, "__libc_dlopen_mode");
	void *target_libc_dlopen_mode = (void *)EZ_REMOTE(libc, my_libc_dlopen_mode);
	DBGPTR(target_libc_dlopen_mode);
	
	struct injcode_bearing br =
	{
		.libc_dlopen_mode = target_libc_dlopen_mode,
		.libc_syscall = (void *)libc_syscall.base_remote
	};
	strncpy(br.libname, sopath, sizeof(br.libname));
	char *target_bearing = ALIGN(syscall_ret_gadget_end);
	memcpy(target_bearing, &br, sizeof(struct injcode_bearing));
	
	DBGPTR(mapped_mem);
	DBGPTR(syscall_ret_gadget);
	DBGPTR(target_bearing);

	int remote_shm_id = (int)CHECK(REMOTE_SC(__NR_shmget, target, MAPPINGSIZE, S_IRWXO, 0));
	uintptr_t remote_shm_ptr = CHECK(REMOTE_SC(__NR_shmat, remote_shm_id, 0, SHM_EXEC, 0));

	#define TO_REMOTE(pl_addr) ((void *)(remote_shm_ptr + ((uintptr_t)(pl_addr) - (uintptr_t)mapped_mem)))

	uintptr_t *target_sp = (uintptr_t *)(mapped_mem + MAPPINGSIZE - (sizeof(void *) * 2));
	target_sp[0] = (uintptr_t)remote_shm_ptr; //code base
	target_sp[1] = (uintptr_t)TO_REMOTE(target_bearing);
	

	char *target_syscall_ret = TO_REMOTE(syscall_ret_gadget);
	#define REMOTE_SC_RET(nr, arg0, arg1, arg2, arg3) remote_syscall(target, (void *)target_syscall_ret, nr, arg0, arg1, arg2, arg3)
	

	DBGPTR(target_sp[0]);
	DBGPTR(target_sp[1]);

	if(shmdt(mapped_mem) < 0){
		PERROR("shmdt");
	}

	/* Make the call */
	/* !! VERY IMPORTANT !! */
	/* Use the syscall->ret gadget to make the new thread safely "return" to its entrypoint */
	pid_t tid = CHECK(REMOTE_SC_RET(__NR_clone, CLONE_FLAGS, (uintptr_t)TO_REMOTE(target_sp), 0, 0));
	/* Wait for new thread to exit before unmapping its memory */
	CHECK(tid);
	do
	{
		usleep(100);
	} while(kill(tid, 0) != -1); /* TODO this is vulnerable to a race condition */
	/* What if the new thread dies, and a new process spawns and takes its pid? */
	/* Unluckily it is impossible to waitpid() for a process you don't own. */

	shmctl(shm_id, IPC_RMID, NULL);

out_ptrace:
	CHECK(ptrace(PTRACE_DETACH, target, 0, 0));
	return err;
}
