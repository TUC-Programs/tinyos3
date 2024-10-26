
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
  PTCB* ptcb = cur_thread()->ptcb;

  // Change the current PTCB to exit status
  ptcb->exitval = exitval;
  ptcb->exited = 1;

  // Broadcast a signal to sleeping 
  kernel_broadcast(&(ptcb->exit_cv));

  // Decrease the amount of running threads by one
  PCB* process = CURPROC;
  process->thread_count = process->thread_count - 1;

  //
  if(process->thread_count == 0){
    if(get_pid(process) != 1){
      PCB* initpcb = get_pcb(1);
      while(!is_rlist_empty(&CURPROC->children_list)) {
        rlnode* child = rlist_pop_front(&CURPROC->children_list);
        child->pcb->parent = initpcb;
        rlist_push_front(&initpcb->children_list, child);
      }
    /* Add exited children to the initial task's exited list 
       and signal the initial task */
      if(!is_rlist_empty(&CURPROC->exited_list)) {
        rlist_append(&initpcb->exited_list, &CURPROC->exited_list);
        kernel_broadcast(&initpcb->child_exit);
      }

    /* Put me into my parent's exited list */
      rlist_push_front(&CURPROC->parent->exited_list, &CURPROC->exited_node);
      kernel_broadcast(&CURPROC->parent->child_exit);
    }
    assert(is_rlist_empty(&CURPROC->children_list));
    assert(is_rlist_empty(&CURPROC->exited_list));

    // Clean the PTCB 
    while(is_rlist_empty(&process->list_ptcb) != 0){
      rlnode* ptcb_list_node;
      ptcb_list_node = rlist_pop_front(&process->list_ptcb);
      free(ptcb_list_node->ptcb);
    }

    // Set FIDT to NULL
    for(int i = 0; i < MAX_FILEID; i++){
      if(process->FIDT[i] != NULL){
        FCB_decref(process->FIDT[i]);
        process->FIDT[i] = NULL;
      }
    }

    // Set args data to NULL
    if(process->args){
      free(process->args);
      process->args = NULL;
    }

    // Set main thread to NULL
    process->main_thread = NULL;

    // Make the exited processes as ZOMBIEs
    process->pstate = ZOMBIE;
  }
  kernel_sleep(EXITED,SCHED_USER);
}

