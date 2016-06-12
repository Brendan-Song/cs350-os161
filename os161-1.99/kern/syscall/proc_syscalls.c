#include "opt-A2.h"
#include <types.h>
#include <kern/errno.h>
#include <kern/unistd.h>
#include <kern/wait.h>
#include <lib.h>
#include <syscall.h>
#include <current.h>
#include <proc.h>
#include <thread.h>
#include <addrspace.h>
#include <copyinout.h>
#include <machine/trapframe.h>

  /* this implementation of sys__exit does not do anything with the exit code */
  /* this needs to be fixed to get exit() and waitpid() working properly */

void sys__exit(int exitcode) {

  struct addrspace *as;
  struct proc *p = curproc;
  /* for now, just include this to keep the compiler from complaining about
     an unused variable */
  (void)exitcode;

  DEBUG(DB_SYSCALL,"Syscall: _exit(%d)\n",exitcode);

  KASSERT(curproc->p_addrspace != NULL);
  as_deactivate();
  /*
   * clear p_addrspace before calling as_destroy. Otherwise if
   * as_destroy sleeps (which is quite possible) when we
   * come back we'll be calling as_activate on a
   * half-destroyed address space. This tends to be
   * messily fatal.
   */
  as = curproc_setas(NULL);
  as_destroy(as);

  /* detach this thread from its process */
  /* note: curproc cannot be used after this call */
  proc_remthread(curthread);

  /* if this is the last user process in the system, proc_destroy()
     will wake up the kernel menu thread */
  proc_destroy(p);
  
  thread_exit();
  /* thread_exit() does not return, so we should never get here */
  panic("return from thread_exit in sys_exit\n");
}


/* stub handler for getpid() system call                */
int
sys_getpid(pid_t *retval)
{
#if OPT_A2
  *retval = curproc->p_pid;
#else
  /* for now, this is just a stub that always returns a PID of 1 */
  /* you need to fix this to make it work properly */
  *retval = 1;
#endif
  return(0);
}

/* stub handler for waitpid() system call                */

int
sys_waitpid(pid_t pid,
	    userptr_t status,
	    int options,
	    pid_t *retval)
{
  int exitstatus;
  int result;

  /* this is just a stub implementation that always reports an
     exit status of 0, regardless of the actual exit status of
     the specified process.   
     In fact, this will return 0 even if the specified process
     is still running, and even if it never existed in the first place.

     Fix this!
  */

  if (options != 0) {
    return(EINVAL);
  }
  /* for now, just pretend the exitstatus is 0 */
  exitstatus = 0;
  result = copyout((void *)&exitstatus,status,sizeof(int));
  if (result) {
    return(result);
  }
  *retval = pid;
  return(0);
}

#if OPT_A2
/*int
tf_copy(struct trapframe *src, struct trapframe *tf)
{
  tf->tf_vaddr = src->tf_vaddr;
  tf->tf_status = src->tf_status;
  tf->tf_cause = src->tf_cause;
  tf->tf_lo = src->tf_lo;
  tf->tf_hi = src->tf_hi;
  tf->tf_ra = src->tf_ra;
  tf->tf_at = src->tf_at;
  tf->tf_v0 = src->tf_v0;
  tf->tf_v1 = src->tf_v1;
  tf->tf_a0 = src->tf_a0;
  tf->tf_a1 = src->tf_a1;
  tf->tf_a2 = src->tf_a2;
  tf->tf_a3 = src->tf_a3;
  tf->tf_t0 = src->tf_t0;
  tf->tf_t1 = src->tf_t1;
  tf->tf_t2 = src->tf_t2;
  tf->tf_t3 = src->tf_t3;
  tf->tf_t4 = src->tf_t4;
  tf->tf_t5 = src->tf_t5;
  tf->tf_t6 = src->tf_t6;
  tf->tf_t7 = src->tf_t7;
  tf->tf_s0 = src->tf_s0;
  tf->tf_s1 = src->tf_s1;
  tf->tf_s2 = src->tf_s2;
  tf->tf_s3 = src->tf_s3;
  tf->tf_s4 = src->tf_s4;
  tf->tf_s5 = src->tf_s5;
  tf->tf_s6 = src->tf_s6;
  tf->tf_s7 = src->tf_s7;
  tf->tf_t8 = src->tf_t8;
  tf->tf_t9 = src->tf_t9;
  tf->tf_k0 = src->tf_k0;
  tf->tf_k1 = src->tf_k1;
  tf->tf_gp = src->tf_gp;
  tf->tf_sp = src->tf_sp;
  tf->tf_s8 = src->tf_s8;
  tf->tf_epc = src->tf_epc;
  return 0;
}*/
static
void
child_entrypoint(void *tf,
		 unsigned long thread_pid)
{
  (void)thread_pid;
  struct trapframe childTF;
  struct trapframe *tempTF = tf;
  childTF = *tempTF;
  kfree(tempTF);
  //struct trapframe *childTF = (struct trapframe*)kmalloc(sizeof(struct trapframe));
  enter_forked_process(&childTF);
}

int
sys_fork(struct trapframe *tf, pid_t *retval) {
  int error = 0;

  // Create new child process
  struct proc *child;
  child = proc_create_runprogram("childProc");

  // Create and copy address space
  struct addrspace *as = as_create();
  if (as == NULL) {
    panic("fork: out of memory"); // out of memory
  }
  error = as_copy(curproc_getas(), &as);
  if (error) {
    panic("fork: as_copy returned an error"); // as_copy returned an error
  }

  // Set child addrspace to copied addrspace
  spinlock_acquire(&child->p_lock);
  child->p_addrspace = as;
  spinlock_release(&child->p_lock);

  // Set child PID
  error = proc_setPid(child);

  // Create thread for child process
  // disable interrupts
  struct trapframe* childTF = kmalloc(sizeof(struct trapframe));
  *childTF = *tf;
  error = thread_fork("childThread", 
		      child, 
		      child_entrypoint, 
		      childTF,
		      (int)retval);
  // enable interrupts
/*  struct proc *child = proc_create("childProc");
  struct addrspace *as = curproc_getas();
  struct thread *t = thread_create("childThread");
  child->p_addrspace = as;
  t->t_stack = tf;
  proc_addthread(child, t);
  struct addrspace *as;
  struct vnode *v;
  as = as_create();
  curproc_setas(as);
  as_activate();*/
  return(0);	// child
}
#endif
