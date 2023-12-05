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
#include "opt-A2.h"
#include <clock.h>
#include <mips/trapframe.h>
#include <vfs.h>
#include <kern/fcntl.h>
#include <test.h>

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


#if OPT_A2
  for (unsigned int i = 0; i < array_num(p->p_children); i++) {
    struct proc* temp_child = array_get(p->p_children, i);
    spinlock_acquire(&p->p_lock);
    array_remove(p->p_children, i);
    i--;

    if (temp_child->p_exitstatus == 1) {
      spinlock_release(&p->p_lock);
      proc_destroy(temp_child);
    } else {
      temp_child->p_parent = NULL;
      spinlock_release(&p->p_lock);
    }
  }
#endif

  /* detach this thread from its process */
  /* note: curproc cannot be used after this call */
  proc_remthread(curthread);

  /* if this is the last user process in the system, proc_destroy()
     will wake up the kernel menu thread */

#if OPT_A2
  spinlock_acquire(&p->p_lock);
  if ((p->p_parent != NULL) && (p->p_parent->p_exitstatus == 0)) {
      p->p_exitstatus = 1;
      p->p_exitcode = exitcode;
      spinlock_release(&p->p_lock);
  } else {
      spinlock_release(&p->p_lock);
      proc_destroy(p);
  }
#else
  proc_destroy(p);
#endif
  
  thread_exit();
  /* thread_exit() does not return, so we should never get here */
  panic("return from thread_exit in sys_exit\n");
}


/* stub handler for getpid() system call                */
int
sys_getpid(pid_t *retval)
{
  /* for now, this is just a stub that always returns a PID of 1 */
  /* you need to fix this to make it work properly */
#if OPT_A2
	*retval = curproc->p_pid;
#else
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
#if OPT_A2 
  struct proc* temp_child = NULL;
  for (unsigned int i = 0; i < array_num(curproc->p_children); i++) {
      struct proc* one = array_get(curproc->p_children, i);
      if (pid == one->p_pid) {
        temp_child = one;
        array_remove(curproc->p_children, i);
        break;
      }
  }
  if (temp_child == NULL) {
    return (ECHILD);
  } 

  spinlock_acquire(&temp_child->p_lock);
  while (temp_child ->p_exitstatus == 0) {
      spinlock_release(&temp_child->p_lock); 
      thread_yield();
      spinlock_acquire(&temp_child->p_lock);
  }
  exitstatus = _MKWAIT_EXIT(temp_child->p_exitcode);
  spinlock_release(&temp_child ->p_lock);
  proc_destroy(temp_child);

#else
  exitstatus = 0;
#endif
  result = copyout((void *)&exitstatus,status,sizeof(int));
  if (result) {
    return(result);
  }
  *retval = pid;
  return(0);
}


#if OPT_A2
int sys_fork(pid_t* retval, struct trapframe* tf) {
  // (a) create child proc
  struct proc* child = proc_create_runprogram("child");
  if (child == NULL) {
    panic("could not create child process");
    return 0;
  }

  // (b) create and copy the address space 
  int res = as_copy(curproc_getas(), &(child->p_addrspace));
  if (res != 0) {
    proc_destroy(child);
    return 0;
  }

  // (d) create a thread
  struct trapframe* tf_child = kmalloc(sizeof(struct trapframe));
  if (tf_child == NULL) {
    proc_destroy(child);
    panic("could not allocate trapframe");
    return 0;
  }
  memcpy(tf_child, tf, sizeof(struct trapframe));
  //*tf_child = *tf;
  //thread_fork("child_thread", child, (void *)&enter_forked_process, tf_child, 0);

  res = thread_fork("child_thread", child, (void *)&enter_forked_process, tf_child, 0);
  if (res != 0) {
    kfree(tf_child);
    proc_destroy(child);
    return 0;
  }
  
  // (e) assign a pid to the child proc
  *retval = child->p_pid;
  child->p_parent = curproc;
  array_add(curproc->p_children, child, NULL);
  
  return 0;
}

#endif


#if OPT_A2

void args_free(char **args, unsigned long argc){
  for (unsigned int i = 0; i < argc; i++) {
    kfree(args[i]);
  }
  kfree(args);
  return;
}

int sys_execv(char *progname, char **args){
  struct addrspace *as;
  struct vnode *v;
  vaddr_t entrypoint, stackptr;
  int result;

  // Count the number of args and copy them into the kernel
  unsigned long argc = 0;
  while (args[argc] != NULL) {
    argc++;
  }
  char** ker_args = kmalloc((argc + 1) * sizeof(char *));
  for (unsigned long i = 0; i < argc; i++) {
    int len = strlen(args[i]) + 1;
    ker_args[i] = kmalloc(len * sizeof(char));
    result = copyinstr((userptr_t)args[i], ker_args[i], len, NULL);
    if (result) {
      args_free(ker_args, argc);
      panic("can not copy args into the kernel");
      return result;
    }
  }
  ker_args[argc] = NULL;

  // Copy the program path into the kernel
  int path_len = strlen(progname) + 1;
  char* ker_prog = kmalloc(sizeof(char) * path_len);
  result = copyinstr((userptr_t)progname, ker_prog, path_len, NULL);
  if (result) {
    args_free(ker_args, argc);
    kfree(ker_prog);
    panic("can not copy the program path into the kernel");
    return result;
  }

  /* Open the file. */
  result = vfs_open(progname, O_RDONLY, 0, &v);
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
  struct addrspace *old_as = curproc_getas();
  //struct addrspace *old_as = curproc_setas(as);
  curproc_setas(as);
  as_activate();
  // destory
  as_destroy(old_as);

  /* Load the executable. */
  result = load_elf(v, &entrypoint);
  if (result) {
    /* p_addrspace will go away when curproc is destroyed */
    vfs_close(v);
    return result;
  }

  /* Done with the file now. */
  vfs_close(v);

  /* Define the user stack in the address space */
  result = as_define_stack(as, &stackptr);
  if (result) {
    /* p_addrspace will go away when curproc is destroyed */
    return result;
  }

  // Copy the arguments from the kernel space into the new address space
  char **curr_argv = kmalloc(sizeof(char *) * (argc + 1));
	stackptr = ROUNDUP(stackptr, 8);
	for (int i = (int) argc - 1; i >= 0; i--) {
	  int len = strlen(ker_args[i]) + 1;
	  len = ROUNDUP(len, 4);
	  stackptr -= len;
	  result = copyoutstr(ker_args[i], (userptr_t)stackptr, (size_t)len, NULL);
    if (result) { 
      panic("can not copy arg from kernel to stack"); 
    }
	  curr_argv[i] = (char*)stackptr;
	}
	curr_argv[argc] = NULL;

	stackptr -= sizeof(vaddr_t);
  copyout((void *)NULL, (userptr_t)stackptr, (size_t)4);

	for (int i = (int) argc - 1; i >= 0; i--) {
	  stackptr -= sizeof(vaddr_t);
	  copyout(&curr_argv[i], (userptr_t)stackptr, sizeof(vaddr_t));
  }
  args_free(ker_args, argc);
  kfree(curr_argv);
  kfree(ker_prog);

  /* Warp to user mode. */
	enter_new_process(argc, (userptr_t)stackptr, stackptr, entrypoint);

  /* enter_new_process does not return. */
  panic("enter_new_process returned\n");
  return EINVAL;
}


#endif

