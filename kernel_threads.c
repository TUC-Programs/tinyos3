
#include "tinyos.h"
#include "kernel_sched.h"
#include "kernel_proc.h"
#include "kernel_cc.h"
#include "kernel_sched.c"

/** 
  @brief Create a new thread in the current process.
  */
Tid_t sys_CreateThread(Task task, int argl, void* args)
{
  PCB* PCBcurrent = CURPROC;
  TCB* tcb = spawn_thread(PCBcurrent, start_main_thread_ptcb);
  //aquire a PTCB
  acquire_PTCB(tcb,task,argl,args);

  //increase the counter of threads in the PCB
  PCBcurrent -> thread_count++;

  //start the thread
  wakeup(tcb);

	return (Tid_t) (tcb->ptcb);  
}

/**
  @brief Return the Tid of the current thread.
 */
Tid_t sys_ThreadSelf()
{
	return (Tid_t) cur_thread() -> ptcb;
}

/**
  @brief Join the given thread.
  */
int sys_ThreadJoin(Tid_t tid, int* exitval)
{
  PTCB* ptcb = (PTCB*) tid;
  PCB* curproc = CURPROC;

  // Check if a Thread with that tid exists in this process else return -1
  if(rlist_find(&curproc->list_ptcb, ptcb, NULL) == NULL){
    return -1;
  }

  // Check if the tid corresponds to the current thread
  if(cur_thread()->ptcb == ptcb){
    return -1;
  }

  // Check if the tid corresponds to a detached thread
  if(ptcb->detached == 1){
    return -1;
  }

  // Increase refcount of the amount of TCB waiting for the PTCB to finish
  ptcb->refcount = ptcb->refcount + 1;
  // Sleep the current thread until the PTCB to finish
  while(ptcb->exited == 0 && ptcb->detached == 0){
    kernel_wait(&(ptcb->exit_cv),SCHED_USER);
  }
  // Waited for the PTCB to finish and now we decrease the amount of TCB
  ptcb->refcount = ptcb->refcount + 1;

  // Check if the exitval is null then save the exit from PTCB to exitval
  if(exitval != NULL){
    *exitval = ptcb->exitval;
  }

  // Check if refcount = 1 then free the PTCB and clear the memory
  if(ptcb->refcount == 1){
    rlist_remove(&(ptcb->ptcb_list_node));
    free(ptcb);
  }

  return 0;
}

/**
  @brief Detach the given thread.
  */
int sys_ThreadDetach(Tid_t tid)
{
  PTCB* ptcb = (PTCB*) tid;

  if((rlist_find(&(CURPROC -> list_ptcb), ptcb, NULL)) == NULL){
    return -1;
  }
  else if( tid == NOTHREAD){
    return -1;
  }
  else if(ptcb -> exited == 1){
    return -1;
  }

  ptcb -> detached = 1;  //DO the flug = 1 (true) of the detached
  kernel_broadcast(&ptcb -> exit_cv); //use kernel_broadcast to broadcast all threads that waiting in threaJoin
  ptcb -> refcount = 1; //reset the refcount (=1)

  return 0;
}

/**
  @brief Terminate the current thread.
  */
void sys_ThreadExit(int exitval)
{
  
}

