
#include <assert.h>
#include <sys/mman.h>

#include "kernel_cc.h"
#include "kernel_proc.h"
#include "kernel_sched.h"
#include "tinyos.h"

#ifndef NVALGRIND
#include <valgrind/valgrind.h>
#endif

#define YIELD_CALLS 500  // here is the maximum yield calls for the threads boosting
#define PRIORITY_QUEUES 10 // we have choose 50 queues for the MLFQ

int yield_calls;
/********************************************
	
	Core table and CCB-related declarations.

 *********************************************/

/* Core control blocks */
CCB cctx[MAX_CORES];


/* 
	The current core's CCB. This must only be used in a 
	non-preemtpive context.
 */
#define CURCORE (cctx[cpu_core_id])

/* 
	The current thread. This is a pointer to the TCB of the thread 
	currently executing on this core.

	This must only be used in non-preemptive context.
*/
#define CURTHREAD (CURCORE.current_thread)


/*
	This can be used in the preemptive context to
	obtain the current thread.
 */
TCB* cur_thread()
{
  int preempt = preempt_off;
  TCB* cur = CURTHREAD;
  if(preempt) preempt_on;
  return cur;
}



/*
   The thread layout.
  --------------------

  On the x86 (Pentium) architecture, the stack grows upward. Therefore, we
  can allocate the TCB at the top of the memory block used as the stack.

  +-------------+
  |   TCB       |
  +-------------+
  |             |
  |    stack    |
  |             |
  |      ^      |
  |      |      |
  +-------------+
  | first frame |
  +-------------+

  Advantages: (a) unified memory area for stack and TCB (b) stack overrun will
  crash own thread, before it affects other threads (which may make debugging
  easier).

  Disadvantages: The stack cannot grow unless we move the whole TCB. Of course,
  we do not support stack growth anyway!
 */

/*
  A counter for active threads. By "active", we mean 'existing',
  with the exception of idle threads (they don't count).
 */
volatile unsigned int active_threads = 0;
Mutex active_threads_spinlock = MUTEX_INIT;

/* This is specific to Intel Pentium! */
#define SYSTEM_PAGE_SIZE (1 << 12)

/* The memory allocated for the TCB must be a multiple of SYSTEM_PAGE_SIZE */
#define THREAD_TCB_SIZE \
	(((sizeof(TCB) + SYSTEM_PAGE_SIZE - 1) / SYSTEM_PAGE_SIZE) * SYSTEM_PAGE_SIZE)

#define THREAD_SIZE (THREAD_TCB_SIZE + THREAD_STACK_SIZE)

//#define MMAPPED_THREAD_MEM
#ifdef MMAPPED_THREAD_MEM

/*
  Use mmap to allocate a thread. A more detailed implementation can allocate a
  "sentinel page", and change access to PROT_NONE, so that a stack overflow
  is detected as seg.fault.
 */
void free_thread(void* ptr, size_t size) { CHECK(munmap(ptr, size)); }

void* allocate_thread(size_t size)
{
	void* ptr = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC,
		MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);

	CHECK((ptr == MAP_FAILED) ? -1 : 0);

	return ptr;
}
#else
/*
  Use malloc to allocate a thread. This is probably faster than  mmap, but
  cannot be made easily to 'detect' stack overflow.
 */
void free_thread(void* ptr, size_t size) { free(ptr); }

void* allocate_thread(size_t size)
{
	void* ptr = aligned_alloc(SYSTEM_PAGE_SIZE, size);
	CHECK((ptr == NULL) ? -1 : 0);
	return ptr;
}
#endif




/*
  This is the function that is used to start normal threads.
*/

void gain(int preempt); /* forward */

static void thread_start()
{
	gain(1);
	cur_thread()->thread_func();

	/* We are not supposed to get here! */
	assert(0);
}

/*
  Initialize and return a new TCB
*/

TCB* spawn_thread(PCB* pcb, void (*func)())
{
	/* The allocated thread size must be a multiple of page size */
	TCB* tcb = (TCB*)allocate_thread(THREAD_SIZE);

	/* Set the owner */
	tcb->owner_pcb = pcb;

	tcb->ptcb = NULL;
	tcb->priority = PRIORITY_QUEUES - 1;

	/* Initialize the other attributes */
	tcb->type = NORMAL_THREAD;
	tcb->state = INIT;
	tcb->phase = CTX_CLEAN;
	tcb->thread_func = func;
	tcb->wakeup_time = NO_TIMEOUT;
	rlnode_init(&tcb->sched_node, tcb); /* Intrusive list node */

	tcb->its = QUANTUM;
	tcb->rts = QUANTUM;
	tcb->last_cause = SCHED_IDLE;
	tcb->curr_cause = SCHED_IDLE;

	/* Compute the stack segment address and size */
	void* sp = ((void*)tcb) + THREAD_TCB_SIZE;

	/* Init the context */
	cpu_initialize_context(&tcb->context, sp, THREAD_STACK_SIZE, thread_start);

#ifndef NVALGRIND
	tcb->valgrind_stack_id = VALGRIND_STACK_REGISTER(sp, sp + THREAD_STACK_SIZE);
#endif

	/* increase the count of active threads */
	Mutex_Lock(&active_threads_spinlock);
	active_threads++;
	Mutex_Unlock(&active_threads_spinlock);

	return tcb;
}

/*
  This is called with sched_spinlock locked !
 */
void release_TCB(TCB* tcb)
{
#ifndef NVALGRIND
	VALGRIND_STACK_DEREGISTER(tcb->valgrind_stack_id);
#endif

	free_thread(tcb, THREAD_SIZE);

	Mutex_Lock(&active_threads_spinlock);
	active_threads--;
	Mutex_Unlock(&active_threads_spinlock);
}

/*
 *
 * Scheduler
 *
 */

/*
 *  Note: the scheduler routines are all in the non-preemptive domain.
 */

/*
  The scheduler queue is implemented as a doubly linked list. The
  head and tail of this list are stored in  SCHED.

  Also, the scheduler contains a linked list of all the sleeping
  threads with a timeout.

  Both of these structures are protected by @c sched_spinlock.
*/

rlnode SCHED[PRIORITY_QUEUES]; /* The scheduler queue */
rlnode TIMEOUT_LIST; /* The list of threads with a timeout */
Mutex sched_spinlock = MUTEX_INIT; /* spinlock for scheduler queue */

/* Interrupt handler for ALARM */
void yield_handler() { yield(SCHED_QUANTUM); }

/* Interrupt handle for inter-core interrupts */
void ici_handler()
{ /* noop for now... */
}

/*
  Possibly add TCB to the scheduler timeout list.

  *** MUST BE CALLED WITH sched_spinlock HELD ***
*/
static void sched_register_timeout(TCB* tcb, TimerDuration timeout)
{
	if (timeout != NO_TIMEOUT) {
		/* set the wakeup time */
		TimerDuration curtime = bios_clock();
		tcb->wakeup_time = (timeout == NO_TIMEOUT) ? NO_TIMEOUT : curtime + timeout;

		/* add to the TIMEOUT_LIST in sorted order */
		rlnode* n = TIMEOUT_LIST.next;
		for (; n != &TIMEOUT_LIST; n = n->next)
			/* skip earlier entries */
			if (tcb->wakeup_time < n->tcb->wakeup_time)
				break;
		/* insert before n */
		rl_splice(n->prev, &tcb->sched_node);
	}
}

/*
  Add TCB to the end of the scheduler list.

  *** MUST BE CALLED WITH sched_spinlock HELD ***
*/
static void sched_queue_add(TCB* tcb)
{
	/* Insert at the end of the scheduling list */
	rlist_push_back(&SCHED[tcb->priority], &tcb->sched_node);

	/* Restart possibly halted cores */
	cpu_core_restart_one();
}

/*
	Adjust the state of a thread to make it READY.

	*** MUST BE CALLED WITH sched_spinlock HELD ***
 */
static void sched_make_ready(TCB* tcb)
{
	assert(tcb->state == STOPPED || tcb->state == INIT);

	/* Possibly remove from TIMEOUT_LIST */
	if (tcb->wakeup_time != NO_TIMEOUT) {
		/* tcb is in TIMEOUT_LIST, fix it */
		assert(tcb->sched_node.next != &(tcb->sched_node) && tcb->state == STOPPED);
		rlist_remove(&tcb->sched_node);
		tcb->wakeup_time = NO_TIMEOUT;
	}

	/* Mark as ready */
	tcb->state = READY;

	/* Possibly add to the scheduler queue */
	if (tcb->phase == CTX_CLEAN)
		sched_queue_add(tcb);
}

/*
  Scan the \c TIMEOUT_LIST for threads whose timeout has expired, and
  wake them up.

  *** MUST BE CALLED WITH sched_spinlock HELD ***
*/
static void sched_wakeup_expired_timeouts()
{
	/* Empty the timeout list up to the current time and wake up each thread */
	TimerDuration curtime = bios_clock();

	while (!is_rlist_empty(&TIMEOUT_LIST)) {
		TCB* tcb = TIMEOUT_LIST.next->tcb;
		if (tcb->wakeup_time > curtime)
			break;
		sched_make_ready(tcb);
	}
}

/*
  Remove the head of the scheduler list, if any, and
  return it. Return NULL if the list is empty.

  *** MUST BE CALLED WITH sched_spinlock HELD ***
*/
static TCB* sched_queue_select(TCB* current)
{
	int i;
	int heighestQueue = PRIORITY_QUEUES -1;

	//here traverse all queues to find the highest priority thread 
	//Check if we have empty queues
	for( i = heighestQueue; i >= 0; i--){
		if(!is_rlist_empty(&SCHED[i])){
			heighestQueue = i;
			break;
		}
		
	}

	/* Get the head of the SCHED list */
	rlnode* sel = rlist_pop_front(&SCHED[heighestQueue]);

	TCB* next_thread = sel->tcb; /* When the list is empty, this is NULL */

	if (next_thread == NULL)
		next_thread = (current->state == READY) ? current : &CURCORE.idle_thread;

	next_thread->its = QUANTUM;

	return next_thread;

}

/*
  Make the process ready.
 */
int wakeup(TCB* tcb)
{
	int ret = 0;

	/* Preemption off */
	int oldpre = preempt_off;

	/* To touch tcb->state, we must get the spinlock. */
	Mutex_Lock(&sched_spinlock);

	if (tcb->state == STOPPED || tcb->state == INIT) {
		sched_make_ready(tcb);
		ret = 1;
	}

	Mutex_Unlock(&sched_spinlock);

	/* Restore preemption state */
	if (oldpre)
		preempt_on;

	return ret;
}

/*
  Atomically put the current process to sleep, after unlocking mx.
 */
void sleep_releasing(Thread_state state, Mutex* mx, enum SCHED_CAUSE cause,
	TimerDuration timeout)
{
	assert(state == STOPPED || state == EXITED);


	int preempt = preempt_off;
	TCB* tcb = CURTHREAD;
	Mutex_Lock(&sched_spinlock);

	/* mark the thread as stopped or exited */
	tcb->state = state;

	/* register the timeout (if any) for the sleeping thread */
	if (state != EXITED)
		sched_register_timeout(tcb, timeout);

	/* Release mx */
	if (mx != NULL)
		Mutex_Unlock(mx);

	/* Release the schduler spinlock before calling yield() !!! */
	Mutex_Unlock(&sched_spinlock);

	/* call this to schedule someone else */
	yield(cause);

	/* Restore preemption state */
	if (preempt)
		preempt_on;
}

/* This function is the entry point to the scheduler's context switching */

void yield(enum SCHED_CAUSE cause)
{

	yield_calls++; //increase the counter of yield calls as needed

	/* Reset the timer, so that we are not interrupted by ALARM */
	TimerDuration remaining = bios_cancel_timer();

	/* We must stop preemption but save it! */
	int preempt = preempt_off;

	TCB* current = CURTHREAD; /* Make a local copy of current process, for speed */

	Mutex_Lock(&sched_spinlock);

	/* Update CURTHREAD state */
	if (current->state == RUNNING){
		current->state = READY;
	}

	/* Update CURTHREAD scheduler data */
	current->rts = remaining;
	current->last_cause = current->curr_cause;
	current->curr_cause = cause;

	/* Wake up threads whose sleep timeout has expired */
	sched_wakeup_expired_timeouts();

	//
	if(yield_calls > YIELD_CALLS){
		thread_boost();
		yield_calls = 0;
	}

	/*
	We have consider that lowest priority queue is 0 and 
	highest priority queue is N ( PRIORITY_QUEUES - 1 )
	*/

	switch(cause){

	case SCHED_QUANTUM:
		if( current -> priority > 0){
			current -> priority--;
		}
		break;

	case SCHED_IO:

		//check if we already are in max priority (N)
		if( current -> priority == PRIORITY_QUEUES -1 ){
			current -> priority = PRIORITY_QUEUES - 1;
		}
		//increase the priority by 1 of thread
		else{
			current -> priority++;
		}

		break;

	case SCHED_MUTEX:

		//here check if both current and last cause is SCHED_MUTEX 
		//If it is correct decrase the highest priority thread
		if( current -> curr_cause == SCHED_MUTEX && current -> last_cause == SCHED_MUTEX && current -> priority > 0){
			//decrease the priority by 1 
			current -> priority--;
		}
		break;

	default: //other cases not need the above implementation
		
		break;
		
	}


	/* Get next */
	TCB* next = sched_queue_select(current);
	assert(next != NULL);

	/* Save the current TCB for the gain phase */
	CURCORE.previous_thread = current;

	Mutex_Unlock(&sched_spinlock);

	/* Switch contexts */
	if (current != next) {
		CURTHREAD = next;
		cpu_swap_context(&current->context, &next->context);
	}

	/* This is where we get after we are switched back on! A long time
	   may have passed. Start a new timeslice...
	  */
	gain(preempt);
}

/*
	This function is called to boost thread when a thread is low on the priority queue 
	and it has yielded enough times to pass the limit of YIELD_CALLS.
*/
void thread_boost()
{
	for(int i=PRIORITY_QUEUES-2; i>=0; i--){
		while(!is_rlist_empty(&SCHED[i])){
			rlnode* headQueue = rlist_pop_front(&SCHED[i]);
			headQueue->tcb->priority = headQueue->tcb->priority + 1;
			rlist_push_back(&SCHED[i+1], headQueue);
		}
	}
}


/*
  This function must be called at the beginning of each new timeslice.
  This is done mostly from inside yield().
  However, for threads that are executed for the first time, this
  has to happen in thread_start.

  The 'preempt' argument determines whether preemption is turned on
  in the new timeslice. When returning to threads in the non-preemptive
  domain (e.g., waiting at some driver), we need to not turn preemption
  on!
*/

void gain(int preempt)
{
	Mutex_Lock(&sched_spinlock);

	TCB* current = CURTHREAD;

	/* Mark current state */
	current->state = RUNNING;
	current->phase = CTX_DIRTY;
	current->rts = current->its;

	/* Take care of the previous thread */
	TCB* prev = CURCORE.previous_thread;
	if (current != prev) {
		prev->phase = CTX_CLEAN;
		switch (prev->state) {
		case READY:
			if (prev->type != IDLE_THREAD)
				sched_queue_add(prev);
			break;
		case EXITED:
			release_TCB(prev);
			break;
		case STOPPED:
			break;
		default:
			assert(0); /* prev->state should not be INIT or RUNNING ! */
		}
	}

	Mutex_Unlock(&sched_spinlock);

	/* Reset preemption as needed */
	if (preempt)
		preempt_on;

	/* Set a 1-quantum alarm */
	bios_set_timer(current->rts);
}

static void idle_thread()
{
	/* When we first start the idle thread */
	yield(SCHED_IDLE);

	/* We come here whenever we cannot find a ready thread for our core */
	while (active_threads > 0) {
		cpu_core_halt();
		yield(SCHED_IDLE);
	}

	/* If the idle thread exits here, we are leaving the scheduler! */
	bios_cancel_timer();
	cpu_core_restart_all();
}

/*
  Initialize the scheduler queue
 */
void initialize_scheduler()
{
	/* Initialize the priority queue of MLFQ */
	for(int i = 0; i < PRIORITY_QUEUES; i++){
		rlnode_init(&SCHED[i], NULL);
	}

	rlnode_init(&TIMEOUT_LIST, NULL);

    /* here reset the counter number of yield calls */
	yield_calls = 0;
}

void run_scheduler()
{
	CCB* curcore = &CURCORE;

	/* Initialize current CCB */
	curcore->id = cpu_core_id;

	curcore->current_thread = &curcore->idle_thread;

	curcore->idle_thread.owner_pcb = get_pcb(0);
	curcore->idle_thread.type = IDLE_THREAD;
	curcore->idle_thread.state = RUNNING;
	curcore->idle_thread.phase = CTX_DIRTY;
	curcore->idle_thread.wakeup_time = NO_TIMEOUT;
	rlnode_init(&curcore->idle_thread.sched_node, &curcore->idle_thread);

	curcore->idle_thread.its = QUANTUM;
	curcore->idle_thread.rts = QUANTUM;

	curcore->idle_thread.curr_cause = SCHED_IDLE;
	curcore->idle_thread.last_cause = SCHED_IDLE;

	/* Initialize interrupt handler */
	cpu_interrupt_handler(ALARM, yield_handler);
	cpu_interrupt_handler(ICI, ici_handler);

	/* Run idle thread */
	preempt_on;
	idle_thread();

	/* Finished scheduling */
	assert(CURTHREAD == &CURCORE.idle_thread);
	cpu_interrupt_handler(ALARM, NULL);
	cpu_interrupt_handler(ICI, NULL);
}




TCB* thread_init(TCB* tcb, PCB* proc, void(*func)(), Task task, int argl,void*args){
	tcb = spawn_thread(proc,func);
	
	// Acquire PTCB
	PTCB* ptcb = (PTCB*)xmalloc(sizeof(PTCB));

	ptcb->tcb = tcb;
	tcb->ptcb = ptcb;

	ptcb->task = task;
	ptcb->argl = argl;
	ptcb->args = args;

	ptcb->exited = 0; //this is a flug can be 0 or 1 we choose 0
	ptcb->detached = 0; 
	ptcb->exitval = 0;

	ptcb->exit_cv = COND_INIT;
	ptcb->refcount = 1;

	rlnode_init(&ptcb->ptcb_list_node, ptcb);
	rlist_push_back(&tcb->owner_pcb->list_ptcb, &ptcb->ptcb_list_node);   


	tcb->owner_pcb->thread_count++;
	return tcb;
}
