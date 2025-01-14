/* See COPYRIGHT for copyright information. */

#include "env.h"
#include <inc/x86.h>
#include <inc/error.h>
#include <inc/string.h>
#include <inc/assert.h>
#include <inc/mmu.h>
#include <kern/env.h>
#include <kern/pmap.h>
#include <kern/trap.h>
#include <kern/syscall.h>
#include <kern/console.h>
#include <kern/sched.h>
#include <kern/env.h>

// Print a string to the system console.
// The string is exactly 'len' characters long.
// Destroys the environment on memory errors. 消除环境的操作在 pmap.c中
static void
sys_cputs(const char *s, size_t len)
{
	// Check that the user has permission to read memory [s, s+len).
	// Destroy the environment if not.

	// LAB 3: Your code here.
	user_mem_assert(curenv,s,len,PTE_U); // PTE_U写不写都一样

	// Print the string supplied by the user.
	cprintf("%.*s", len, s);
}

// Read a character from the system console without blocking.
// Returns the character, or 0 if there is no input waiting.
static int
sys_cgetc(void)
{
	return cons_getc();
}

// Returns the current environment's envid.
static envid_t
sys_getenvid(void)
{
	return curenv->env_id;
}

// Destroy a given environment (possibly the currently running environment).
//
// Returns 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
static int
sys_env_destroy(envid_t envid)
{
	// 不知道为什么 lab5 合并 lab4后，两句cprintf被注释掉了，然后一个lab4的测试都通不过了
	int r;
	struct Env *e;

	if ((r = envid2env(envid, &e, 1)) < 0)
		return r;
	if (e == curenv)
		cprintf("[%08x] exiting gracefully\n", curenv->env_id);
	else
		cprintf("[%08x] destroying %08x\n", curenv->env_id, e->env_id);
	env_destroy(e);
	return 0;
}

// Deschedule current environment and pick a different one to run.
static void
sys_yield(void)
{
	sched_yield();
}

// Allocate a new environment.
// Returns envid of new environment, or < 0 on error.  Errors are:
//	-E_NO_FREE_ENV if no free environment is available.
//	-E_NO_MEM on memory exhaustion.
static envid_t
sys_exofork(void)
{
	// Create the new environment with env_alloc(), from kern/env.c.
	// It should be left as env_alloc created it, except that
	// status is set to ENV_NOT_RUNNABLE, and the register set is copied
	// from the current environment -- but tweaked so sys_exofork
	// will appear to return 0.

	// LAB 4: Your code here.
	// panic("sys_exofork not implemented");
	struct Env* new_env;
	int env_alloc_ret = env_alloc(&new_env, curenv->env_id);
	if (env_alloc_ret < 0) {
		// cprintf("syscall.c : sys_exofork() , env_aclloc_failed\n");
		return env_alloc_ret;
	}
	new_env->env_status = ENV_NOT_RUNNABLE;
	memmove(&new_env->env_tf,&curenv->env_tf,sizeof(new_env->env_tf));// 我靠，这忘了写了。 导致在 va = 0时pagefault，为什么返回0虚拟地址，就是因为trapframe中的 eip没有设置
	new_env->env_tf.tf_regs.reg_eax = 0; // fork出来的新env的返回值是0
	return new_env->env_id; // 父进程(env)的返回值是子进程的env_id

}

// Set envid's env_status to status, which must be ENV_RUNNABLE
// or ENV_NOT_RUNNABLE.
//
// Returns 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
//	-E_INVAL if status is not a valid status for an environment.
static int
sys_env_set_status(envid_t envid, int status)
{
	// Hint: Use the 'envid2env' function from kern/env.c to translate an
	// envid to a struct Env.
	// You should set envid2env's third argument to 1, which will
	// check whether the current environment has permission to set
	// envid's status.

	// LAB 4: Your code here.
	struct Env *e;
	int r  =envid2env(envid, &e, 1);
	// environment envid doesn't currently exist,, or the caller doesn't have permission to change envid
	if(r != 0)
		return r;
	// status is not a valid status for an environment
	if(e->env_status != ENV_RUNNABLE && e->env_status != ENV_NOT_RUNNABLE)
		return -E_INVAL;
	e->env_status = status;
	return 0;
	// panic("sys_env_set_status not implemented");
}

// Set envid's trap frame to 'tf'.
// tf is modified to make sure that user environments always run at code
// protection level 3 (CPL 3), interrupts enabled, and IOPL of 0.
//
// Returns 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
static int
sys_env_set_trapframe(envid_t envid, struct Trapframe *tf)
{
	// LAB 5: Your code here.
	// Remember to check whether the user has supplied us with a good
	// address!
	struct Env *e;
	int r;

	if ((r = envid2env(envid, &e, true)) < 0)
			return -E_BAD_ENV;
	// check whether the user has supplied us with a good address
	user_mem_assert(e, tf, sizeof(struct Trapframe), PTE_U);
	e->env_tf = *tf;
	e->env_tf.tf_cs |= 3;
	e->env_tf.tf_eflags |= FL_IF;
	e->env_tf.tf_eflags &= ~FL_IOPL_MASK; // 这个不能忽略，不然桶不过 IO test测试
	return 0;
}

// Set the page fault upcall for 'envid' by modifying the corresponding struct
// Env's 'env_pgfault_upcall' field.  When 'envid' causes a page fault, the
// kernel will push a fault record onto the exception stack, then branch to
// 'func'.
//
// Returns 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
static int
sys_env_set_pgfault_upcall(envid_t envid, void *func)
{
	// LAB 4: Your code here.
	struct Env *e;  
    if (envid2env(envid, &e, 1) <0)  
        return -E_BAD_ENV;   
    e->env_pgfault_upcall = func;  
    return 0;
	// panic("sys_env_set_pgfault_upcall not implemented");
}

// Allocate a page of memory and map it at 'va' with permission
// 'perm' in the address space of 'envid'.
// The page's contents are set to 0.
// If a page is already mapped at 'va', that page is unmapped as a
// side effect.
//
// perm -- PTE_U | PTE_P must be set, PTE_AVAIL | PTE_W may or may not be set,
//         but no other bits may be set.  See PTE_SYSCALL in inc/mmu.h.
//
// Return 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid. 在envid2env()已经封装好这个功能了
//	-E_INVAL if va >= UTOP, or va is not page-aligned.
//	-E_INVAL if perm is inappropriate (see above).
//	-E_NO_MEM if there's no memory to allocate the new page,
//		or to allocate any necessary page tables.
// 如果已经存在映射，那么额外的作用就是覆盖这个映射
static int
sys_page_alloc(envid_t envid, void *va, int perm)
{
	// Hint: This function is a wrapper around page_alloc() and
	//   page_insert() from kern/pmap.c.
	//   Most of the new code you write should be to check the
	//   parameters for correctness.
	//   If page_insert() fails, remember to free the page you
	//   allocated!
	// LAB 4: Your code here.
	struct Env* dstenv;
	int ret_id2env = envid2env(envid, &dstenv,1 );
	if (ret_id2env < 0) {
		return ret_id2env;
	}

    // va 不与页对齐 或者 va大于UTOP
	if((uintptr_t)va % PGSIZE !=0 || (uintptr_t)va >= UTOP) {
		// cprintf("sys_page_alloc failed :(uintptr_t)va % PGSIZE !=0 || (uintptr_t)va >= UTOP\n ");
		return -E_INVAL;
	}
		

	//PTE_U | PTE_P must be set
	if((perm & (PTE_U | PTE_P)) ==0) {
		// cprintf("sys_page_alloc failed :perm & (PTE_U | PTE_P)) ==0\n ");
		return -E_INVAL;
	}


	//PTE_AVAIL | PTE_W may or may not be set, but no other bits may be set
	if( perm  & ~PTE_SYSCALL) {
		// cprintf("sys_page_alloc failed :perm  & ~PTE_SYSCALL\n ");
		return -E_INVAL;
	}

	struct PageInfo *pp;
	pp = page_alloc(1); //参数为1就是初始化页面内容为0。
	if(!pp) {
		// cprintf("sys_page_alloc failed :page_alloc\n ");
		return -E_NO_MEM;
	}

	int ret_page_insert = page_insert(dstenv->env_pgdir, pp, va, perm);
	if (ret_page_insert < 0) {
		// cprintf("sys_page_alloc failed :page_insert\n ");
		// 释放之前分配的页面
		page_free(pp);
		return ret_page_insert;
	}
	return 0;

}

// Map the page of memory at 'srcva' in srcenvid's address space
// at 'dstva' in dstenvid's address space with permission 'perm'.
// Perm has the same restrictions as in sys_page_alloc, except
// that it also must not grant write access to a read-only
// page.
//
// Return 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if srcenvid and/or dstenvid doesn't currently exist,
//		or the caller doesn't have permission to change one of them.
//	-E_INVAL if srcva >= UTOP or srcva is not page-aligned,
//		or dstva >= UTOP or dstva is not page-aligned.
//	-E_INVAL is srcva is not mapped in srcenvid's address space.
//	-E_INVAL if perm is inappropriate (see sys_page_alloc).
//	-E_INVAL if (perm & PTE_W), but srcva is read-only in srcenvid's
//		address space.
//	-E_NO_MEM if there's no memory to allocate any necessary page tables.
static int
sys_page_map(envid_t srcenvid, void *srcva,
	     envid_t dstenvid, void *dstva, int perm)
{
	// Hint: This function is a wrapper around page_lookup() and
	//   page_insert() from kern/pmap.c.
	//   Again, most of the new code you write should be to check the
	//   parameters for correctness.
	//   Use the third argument to page_lookup() to
	//   check the current permissions on the page.

	// LAB 4: Your code here.
	//envid doesn't currently exist
	struct Env *srce, *dste;
	int src  =envid2env(srcenvid, &srce, 1);
	if(src != 0) 
		return src;
	int dst  =envid2env(dstenvid, &dste, 1);
	if(dst != 0) 
		return dst;

	//va >= UTOP, or va is not page-aligned
	if((uintptr_t)srcva % PGSIZE !=0 || (uintptr_t)srcva >= UTOP)
		return -E_INVAL;
	if((uintptr_t)dstva % PGSIZE !=0 || (uintptr_t)dstva >= UTOP)
		return -E_INVAL;

	//PTE_U | PTE_P must be set
	if((perm & (PTE_U | PTE_P)) ==0)
		return -E_INVAL;

	//PTE_AVAIL | PTE_W may or may not be set, but no other bits may be set
	if( perm  & ~PTE_SYSCALL )
		return -E_INVAL;

	
	struct PageInfo *srcpp;
	pte_t *srcpte;
	srcpp = page_lookup(srce->env_pgdir, srcva, &srcpte);
	//if (perm & PTE_W), but srcva is read-only
	if(!(*srcpte & PTE_W) && (perm & PTE_W))
		return -E_INVAL;

	int r = page_insert(dste->env_pgdir, srcpp, dstva, perm);
	if(r != 0)
		return r;
	return 0;
}

// Unmap the page of memory at 'va' in the address space of 'envid'.
// If no page is mapped, the function silently succeeds.
//
// Return 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
//	-E_INVAL if va >= UTOP, or va is not page-aligned.
static int
sys_page_unmap(envid_t envid, void *va)
{
	// Hint: This function is a wrapper around page_remove().

	// LAB 4: Your code here.
	struct Env *e;
	int r  =envid2env(envid, &e, 1);
	if(r != 0) 
		return r;

	//va >= UTOP, or va is not page-aligned
	if((uintptr_t)va % PGSIZE !=0 || (uintptr_t)va >= UTOP)
		return -E_INVAL;

	page_remove(e->env_pgdir, va);
	return 0;
	// panic("sys_page_unmap not implemented");
}

// Try to send 'value' to the target env 'envid'.
// If srcva < UTOP, then also send page currently mapped at 'srcva',
// so that receiver gets a duplicate mapping of the same page.
//
// The send fails with a return value of -E_IPC_NOT_RECV if the
// target is not blocked, waiting for an IPC.
//
// The send also can fail for the other reasons listed below.
//
// Otherwise, the send succeeds, and the target's ipc fields are
// updated as follows:
//    env_ipc_recving is set to 0 to block future sends;
//    env_ipc_from is set to the sending envid;
//    env_ipc_value is set to the 'value' parameter;
//    env_ipc_perm is set to 'perm' if a page was transferred, 0 otherwise.
// The target environment is marked runnable again, returning 0
// from the paused sys_ipc_recv system call.  (Hint: does the
// sys_ipc_recv function ever actually return?)
//
// If the sender wants to send a page but the receiver isn't asking for one,
// then no page mapping is transferred, but no error occurs.
// The ipc only happens when no errors occur.
//
// Returns 0 on success, < 0 on error.
// Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist.
//		(No need to check permissions.)
//	-E_IPC_NOT_RECV if envid is not currently blocked in sys_ipc_recv,
//		or another environment managed to send first.
//	-E_INVAL if srcva < UTOP but srcva is not page-aligned.
//	-E_INVAL if srcva < UTOP and perm is inappropriate
//		(see sys_page_alloc).
//	-E_INVAL if srcva < UTOP but srcva is not mapped in the caller's
//		address space.
//	-E_INVAL if (perm & PTE_W), but srcva is read-only in the
//		current environment's address space.
//	-E_NO_MEM if there's not enough memory to map srcva in envid's
//		address space.
// envid: 给envid进程发送消息
// value: 传输的32位整数
// srcva： 如果srcva不等于0，表示发送端通过页面传输消息
static int
sys_ipc_try_send(envid_t envid, uint32_t value, void *srcva, unsigned perm)
{
	int r;
	pte_t *pte;
	struct PageInfo *pp;
	struct Env *env;

	if ((r = envid2env(envid, &env, 0)) < 0)
			return -E_BAD_ENV;
	if (env->env_ipc_recving != true || env->env_ipc_from != 0) 
			// 接收端已经在接受别的进程的消息了，告知用户进程。
			return -E_IPC_NOT_RECV;
	if (srcva < (void *)UTOP && PGOFF(srcva)) 
			// srcva没有页对齐，错误
			return -E_INVAL;
	if (srcva < (void *)UTOP) {
			if ((perm & PTE_P) == 0 || (perm & PTE_U) == 0)
					return -E_INVAL;
			if ((perm & ~(PTE_P | PTE_U | PTE_W | PTE_AVAIL)) != 0)
					return -E_INVAL;
	}
	// 尝试到发送端进程的srcva对应的物理页面
	if (srcva < (void *)UTOP && (pp = page_lookup(curenv->env_pgdir, srcva, &pte)) == NULL)
			// 没找到，错误
			return -E_INVAL;
	// 找到了srcva的物理页
	if (srcva < (void *)UTOP && (perm & PTE_W) != 0 && (*pte & PTE_W) == 0)
			// 但是perm权限与被发送物理页的pte的权限不一致，即perm可写，但是被发送物理页对应的pte不可写，错误
			return -E_INVAL;
	if (srcva < (void *)UTOP && env->env_ipc_dstva != 0) {
		// 将这个物理页面插入到接收端进程指定的虚拟地址处
			if ((r = page_insert(env->env_pgdir, pp, env->env_ipc_dstva, perm)) < 0)
				// 但是物理空间不足，错误
					return -E_NO_MEM;
			env->env_ipc_perm = perm;
	}

	env->env_ipc_from = curenv->env_id;
	env->env_ipc_recving = false;
	env->env_ipc_value = value;
	env->env_status = ENV_RUNNABLE; // 唤醒接收端！
	env->env_tf.tf_regs.reg_eax = 0;
	return 0;
	// panic("sys_ipc_try_send not implemented");
}

// Block until a value is ready.  Record that you want to receive
// using the env_ipc_recving and env_ipc_dstva fields of struct Env,
// mark yourself not runnable, and then give up the CPU.
//
// If 'dstva' is < UTOP, then you are willing to receive a page of data.
// 'dstva' is the virtual address at which the sent page should be mapped.
//
// This function only returns on error, but the system call will eventually
// return 0 on success.
// Return < 0 on error.  Errors are:
//	-E_INVAL if dstva < UTOP but dstva is not page-aligned.
static int
sys_ipc_recv(void *dstva)
{
	// 如果小于UTOP，那么表示两个进程通过映射同一个页进行通信
	// 此时dstva必须页对齐
	if (dstva < (void *)UTOP && PGOFF(dstva))
			return -E_INVAL;
	curenv->env_ipc_recving = true; // env_ipc_recving标识接收进程是否在等待接收消息，此时置为true，表示接收端在等待
	curenv->env_ipc_dstva = dstva;  // 表示本接收端如果要通过页通信，则通过dstva这个虚拟地址行通信
	curenv->env_ipc_from = 0;       // env_ipc_from这个标识表示本接收端对应的发送端进程号，但是现在将它初始化位0，表示还没有对应的发送端
	// 将本进程阻塞
	curenv->env_status = ENV_NOT_RUNNABLE;
	sched_yield();
	return 0;
}

// Dispatches to the correct kernel function, passing the arguments.
int32_t
syscall(uint32_t syscallno, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
	// Call the function corresponding to the 'syscallno' parameter.
	// Return any appropriate return value.
	// LAB 3: Your code here.

	// panic("syscall not implemented");

	switch (syscallno) {
		case SYS_cputs:
			sys_cputs((const char*)a1, (size_t)a2);
			return 0;
		case SYS_cgetc :
			return sys_cgetc();
		case SYS_getenvid:
			return sys_getenvid();
		case SYS_env_destroy:
			return sys_env_destroy((envid_t)a1);
		case SYS_yield:
			sys_yield();
			return 0;
		case SYS_exofork:
			return sys_exofork();
		case SYS_page_alloc:
			return sys_page_alloc((envid_t) a1, (void *)a2, (int) a3);
		case SYS_page_map :
			return sys_page_map((envid_t) a1, (void *)a2, (envid_t) a3, (void *)a4, (int)a5);
		case SYS_page_unmap :
		    return sys_page_unmap((envid_t) a1, (void *)a2);
		case SYS_env_set_status:
			return sys_env_set_status((envid_t) a1, (int) a2);
		case SYS_env_set_pgfault_upcall:
			return sys_env_set_pgfault_upcall((envid_t) a1, (void *)a2);
		case SYS_ipc_try_send:
			return sys_ipc_try_send((envid_t) a1, (uint32_t) a2, (void *)a3, (unsigned) a4);
		case SYS_ipc_recv:
			return sys_ipc_recv((void *)a1);
		case SYS_env_set_trapframe :
			return sys_env_set_trapframe((envid_t) a1, (struct Trapframe *)a2);
		default:
			return -E_INVAL;
	}
}

