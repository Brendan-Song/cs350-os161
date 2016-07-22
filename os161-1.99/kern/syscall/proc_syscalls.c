#include "opt-A3.h"
#include "opt-A2.h"
#include <types.h>
#include <kern/errno.h>
#include <kern/unistd.h>
#include <kern/wait.h>
#include <kern/fcntl.h>
#include <lib.h>
#include <syscall.h>
#include <current.h>
#include <proc.h>
#include <thread.h>
#include <addrspace.h>
#include <copyinout.h>
#include <machine/trapframe.h>
#include <vfs.h>
#include <synch.h>

#if OPT_A2
static struct lock *thread_fork_lock; // mutex for forking threads
static struct lock *proc_exit_lock; // mutex for exiting processes
#endif
  /* this implementation of sys__exit does not do anything with the exit code */
  /* this needs to be fixed to get exit() and waitpid() working properly */

void sys__exit(int exitcode) {

  struct addrspace *as;
  struct proc *p = curproc;
  /* for now, just include this to keep the compiler from complaining about
     an unused variable */
#if OPT_A2
#else
  (void)exitcode;
#endif

  if (proc_exit_lock == NULL) {
    proc_exit_lock = lock_create("proc_exit_lock");
  }

  DEBUG(DB_SYSCALL,"Syscall: _exit(%d)\n",exitcode);

#if OPT_A2
  lock_acquire(proc_exit_lock);
  p->p_exitcode = exitcode;
  p->p_exited = true;
  // destroy link to children
  pm_orphan_children(p->p_pid);
  // let anyone waiting know we are about to exit
  cv_broadcast(p->p_cv, proc_exit_lock);
  if (p->p_parentpid != 0) { 
    // don't destroy self until the parent process has exited
    struct proc *parent = pm_get_proc_by_pid((int)p->p_parentpid);
    cv_wait(parent->p_cv, proc_exit_lock);
  }
  
  lock_release(proc_exit_lock);
#endif

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

#if OPT_A2
  if (status == NULL) {
    return EFAULT;
  }

  struct proc *proc;
  // check that we're not waiting for ourself or an invalid process
  if (curproc->p_pid == pid || pid <= 0) {
    return(EINVAL);
  }

  if (proc_exit_lock == NULL) {
    proc_exit_lock = lock_create("proc_exit_lock");
  }
  lock_acquire(proc_exit_lock);

  proc = pm_get_proc_by_pid(pid);
  if (proc == NULL) {
    // make sure child is not null
    lock_release(proc_exit_lock);
    return ESRCH;
  } else if (proc->p_parentpid != curproc->p_pid) {
    // only parent can call waitpid on its children
    lock_release(proc_exit_lock);
    return ECHILD;
  } else if (!proc->p_exited) {
    // if child has not exited, wait for it
    cv_wait(proc->p_cv, proc_exit_lock);
    KASSERT(proc->p_exited);
  }
  exitstatus = proc->p_exitcode;
  proc->p_parentpid = 0;
  pm_remove_proc((int)pid);

  // store exitstatus
  exitstatus = _MKWAIT_EXIT(exitstatus);
  result = copyout((void *)&exitstatus,status,sizeof(int));
  *retval = pid;

  lock_release(proc_exit_lock);
  return result;

#else
  /* for now, just pretend the exitstatus is 0 */
  *exitstatus = 0;
  result = copyout((void *)&exitstatus,status,sizeof(int));
  if (result) {
    return(result);
  }
  *retval = pid;
  return(0);
#endif
}

#if OPT_A2
static
void
child_entrypoint(void *tf,
		 unsigned long unused)
{
  struct trapframe *tempTF;
  tempTF = (struct trapframe *)tf;
  tempTF->tf_v0 = 0; // set return value to 0 to indicate child
  tempTF->tf_a3 = 0;
  tempTF->tf_epc += 4; // increment PC
  // Copy modified trapframe to stack
  struct trapframe childTF;
  childTF = *tempTF;
  (void)unused; // avoid warning
  enter_forked_process(&childTF);
}

int
sys_fork(struct trapframe *tf, pid_t *retval) {
  int error = 0;

  // Initialize lock
  if (thread_fork_lock == NULL) {
    thread_fork_lock = lock_create("thread_fork_lock");
  }

  // Create new child process
  struct proc *child;
  child = proc_create_runprogram("childProc");
  if (child == NULL) {
    return ENOMEM;
    //panic("fork: could not create child process");
  }

  // Create and copy address space
  spinlock_acquire(&child->p_lock);
  error = as_copy(curproc_getas(), &child->p_addrspace);
  if (error) {
    return ENOMEM;
    //panic("fork: as_copy returned an error");
  }
  spinlock_release(&child->p_lock);

  // Create thread for child process
  lock_acquire(thread_fork_lock);
  struct trapframe* childTF = kmalloc(sizeof(struct trapframe));
  *childTF = *tf;
  error = thread_fork("childThread", 
		      child, 
		      child_entrypoint, 
		      childTF,
		      (int)retval);
  if (error) {
    return ENOMEM;
    //panic("fork: thread_fork returned an error");
  }
  lock_release(thread_fork_lock);
  
  *retval = child->p_pid;
  return(0);
}

int
sys_execv(char *progname, char **args, pid_t *retval) {
  // Replaces currently executing program with a newly loaded program image
  // pid remains unchanged
  // Process:
  // 1. Count # of arguments
  // 2. Copy args into the kernel
  // 3. Copy the program path into the kernel
  // 4. Open the program file using vfs_open(prog_name, ...)
  // 5. Create new addr space, set process to the new addr space and activate it
  // 6. Using the opened prog file, load the prog image using load_elf
  // 7. Need to copy the arguments into the new addr space
  // 8. Delete old addr space
  // 9. Call enter_new_process
  
  (void)retval; // avoid warning
  
  // 1. Count # of arguments
  int argc = 0;
  while (args[argc] != NULL) {
    argc++;
  }

  // 2. Copy args into the kernel
  size_t argsize;
  char **argv = (char **)kmalloc(sizeof(char *) * (argc + 1));
  for (int i = 0; i < argc; i++) {
    int size = strlen(args[i]) + 1;
    argv[i] = (char *)kmalloc(sizeof(char) * size);
    copyinstr((userptr_t)args[i], argv[i], size, &argsize);
  }
  
  // NULL terminate args array
  argv[argc] = NULL;

  // 3. Copy program path into kernel
  size_t psize;
  int pathsize = strlen(progname) + 1;
  char *path = (char *)kmalloc(sizeof(char *) * pathsize);
  copyinstr((userptr_t)progname, path, pathsize, &psize);

  // Keep current addrspace to be deleted
  struct addrspace *oldAS = curproc_getas();

  // This point forward is copy/pasted runprogram with a few minor changes
  struct addrspace *as;
  struct vnode *v;
  vaddr_t entrypoint, stackptr;
  int result;

  /* Open the file. */
  result = vfs_open(path, O_RDONLY, 0, &v);
  if (result) {
    return result;
  }

  /* We should be a new process. */
  //KASSERT(curproc_getas() == NULL);

  /* Create a new address space. */
  as = as_create();
  if (as ==NULL) {
    vfs_close(v);
    return ENOMEM;
  }

  /* Switch to it and activate it. */
  curproc_setas(as);
  as_activate();

  /* Load the executable. */
  result = load_elf(v, &entrypoint);
  if (result) {
    /* p_addrspace will go away when curproc is destroyed */
    vfs_close(v);
    return result;
  }

#if OPT_A3
  // load_elf completed
  // as->as_loaded = true;
  // TODO:
  // flush TLB
  // // load in TLBHI_INVALID(index)
  // // load in TLBLO_INVALID()
  // ensure text segments have TLBLO_DIRTY off
  // // use elo &= ~TLBLO_DIRTY;
  
#endif

  /* Done with the file now. */
  vfs_close(v);

  /* Define the user stack in the address space */
  result = as_define_stack(as, &stackptr);
  if (result) {
    /* p_addrspace will go away when curproc is destroyed */
    return result;
  }

  // copy arguments into new address space
  vaddr_t stack[argc + 1]; // + 1 for NULL terminator
  
  // 8 byte align the stack in prep for args
  stackptr = ROUNDUP(stackptr - 8, 8);
  
  // put actual strings onto the stack
  // strings don't have to be 4 or 8 byte aligned
  for (int i = argc - 1; i >= 0; i--) {
    int size = strlen(argv[i]) + 1; // + 1 for '\0' character
    stackptr -= size; // make space on the stack
    copyoutstr(argv[i], (userptr_t)stackptr, size, &argsize);
    stack[i] = stackptr;
  }
  
  // 4 byte align the stack in prep for string pointers
  stackptr = ROUNDUP(stackptr - 4, 4);
  stackptr -= 4; // create space for the NULL arg
  
  // NULL terminate arg array
  stack[argc] = (vaddr_t)NULL;
  
  // put string pointers onto the stack
  // pointers to strings must be 4 byte aligned
  for (int i = argc - 1; i >= 0; i--) {
    stackptr -= ROUNDUP(sizeof(stack[i]), 4); // make space on the stack (4 byte aligned)
    copyout(&stack[i], (userptr_t)stackptr, sizeof(stack[i]));
  }

  as_destroy(oldAS);
  
  enter_new_process(argc, (userptr_t)stackptr, stackptr, entrypoint);

  /* enter_new_process does not return. */
  panic("enter_new_process returned\n");
  return EINVAL;
}
#endif
