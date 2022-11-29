
#include "tinyos.h"
#include "kernel_sched.h"
#include "kernel_proc.h"
#include "util.h"
#include "kernel_cc.h"
#include "kernel_streams.h"


/** 
  @brief Create a new thread in the current process.
  */
Tid_t sys_CreateThread(Task task, int argl, void* args)
{
  /*acquire new ptcb */
  PTCB* ptcb = acquire_PTCB();
  /*initialize ptcb */
  initialize_ptcb(ptcb, argl, args, task);

  /* create new thread */
  
  ptcb->tcb = spawn_thread(CURPROC, start_multiThread);
  TCB* newtcb = ptcb->tcb;

  CURPROC->thread_count++;
  newtcb->owner_ptcb = ptcb;
 /* newtcb->owner_ptcb = ptcb;*/
  /*put the new ptcb in the list of the CURPROC*/
  rlist_append(&CURPROC->PTCB_list,&ptcb->PTCB_node);

  /*wake up the new tcb*/
  wakeup(newtcb);
  
	return(Tid_t)ptcb;
}

/**
  @brief Return the Tid of the current thread.
 */
Tid_t sys_ThreadSelf()
{

	return (Tid_t) (cur_thread()->owner_ptcb);
}

/**
  @brief Join the given thread.
  Possible errors are:
    - there is no thread with the given tid in this process.
    - the tid corresponds to the current thread.
    - the tid corresponds to a detached thread. 
    - the threads don't belong to the same process (PCB)
  */
int sys_ThreadJoin(Tid_t tid, int* exitval)
{
  rlnode *node = rlist_find(&CURPROC->PTCB_list, ((PTCB*)tid), NULL);
  PTCB *ptcb = (PTCB *)tid;
  //printf("%p %d\n", node, tid);
  /*- the threads don't belong to the same process (PCB)*/
  if(node == NULL){
    return -1;
  }

  /**
   * - there is no thread with the given tid in this process.
   * - the tid corresponds to the current thread.
   * - the tid corresponds to a detached thread.
   */
  if(tid == NOTHREAD || cur_thread()->owner_ptcb == ptcb || ptcb->detached == 1){
    return -1;
  }
  
  /* Now the thread is joinable 
   *  We increaze the number of the PTCBs that wait the (PTCB)tid to exit
   */
  ptcb->refcount++;

  /* Make the current thread wait until the (PTCB)tid exits */
  while(ptcb->detached == 0 && ptcb->exited == 0) {
    kernel_wait(ptcb->exit_cv, SCHED_USER);
  }

  /* If the (PTCB*)tid is detached while waiting return -1 */
  if(ptcb->detached == 1){
    return -1;
  }

  /* In this level the (PTCB*)tid has exited and the current thread does not have to wait any longer*/
  ptcb->refcount--;

  /**/
  if(exitval != NULL){
    *exitval = ptcb->exitval;
  }
  /**/
  if(ptcb->refcount == 0){
    rlist_remove(&ptcb->PTCB_node);
    free(ptcb);
  }
	return 0;
}

/**
  @brief Detach the given thread.
  */
 /**
  *  -there is no thread with the given tid in this process.
  *   the tid corresponds to a detached thread.
*/
int sys_ThreadDetach(Tid_t tid)
{ 
  printf("sys_ThreadDetach\n");
  printf("state = %d thread_count = %d\n", cur_thread()->state, CURPROC->thread_count);
  if(tid == NOTHREAD){
    printf("No Thread\n");
    return -1;
  }
  PTCB *ptcb = (PTCB *)tid;
  rlnode *node = rlist_find(&CURPROC->PTCB_list, ptcb, NULL);
  if(node == NULL) {
    return -1;
  }
  //printf("%d %d %p %d\n", ptcb->exited, ptcb->detached, node, tid);
  if(ptcb->detached == 0 && ptcb->exited == 0 && node != NULL && ptcb != NOTHREAD) {
    printf("thread detached\n");
    ptcb->detached = 1; 
    kernel_broadcast(ptcb->exit_cv);
    return 0;
  }
  return -1;
	
}

/**
  @brief Terminate the current thread.
  */

void sys_ThreadExit(int exitval)
{
  PCB* curproc = CURPROC;
  TCB* curthread = cur_thread();
  PTCB* curptcb = curthread->owner_ptcb;

  curptcb->exitval = exitval;
  curptcb->exited = 1;
  curptcb->tcb = NULL;

  curproc->thread_count --;

  assert((cur_thread()->owner_ptcb) != NULL);
  /* Send a signal to the waiting processes if there are  */

  if(curptcb->refcount != 0){
    kernel_broadcast(curptcb->exit_cv);
  }

  if(curproc->thread_count == 0){
      if(get_pid(curproc)==1) {

    while(sys_WaitChild(NOPROC,NULL)!=NOPROC);

  } else {

    /* Reparent any children of the exiting process to the 
       initial task */
    PCB* initpcb = get_pcb(1);
    while(!is_rlist_empty(& curproc->children_list)) {
      rlnode* child = rlist_pop_front(& curproc->children_list);
      child->pcb->parent = initpcb;
      rlist_push_front(& initpcb->children_list, child);
    }

    /* Add exited children to the initial task's exited list 
       and signal the initial task */
    if(!is_rlist_empty(& curproc->exited_list)) {
      rlist_append(& initpcb->exited_list, &curproc->exited_list);
      kernel_broadcast(& initpcb->child_exit);
    }

    /* Put me into my parent's exited list */
    rlist_push_front(& curproc->parent->exited_list, &curproc->exited_node);
    kernel_broadcast(& curproc->parent->child_exit);

  }

  assert(is_rlist_empty(& curproc->children_list));
  assert(is_rlist_empty(& curproc->exited_list));


  /* 
    Do all the other cleanup we want here, close files etc. 
   */

  /* Release the args data */
  if(curproc->args) {
    free(curproc->args);
    curproc->args = NULL;
  }

  /* Clean up FIDT */
  for(int i=0;i<MAX_FILEID;i++) {
    if(curproc->FIDT[i] != NULL) {
      FCB_decref(curproc->FIDT[i]);
      curproc->FIDT[i] = NULL;
    }
  }

  /* Disconnect my main_thread */
  curproc->main_thread = NULL;

  /* Now, mark the process as exited. */
  curproc->pstate = ZOMBIE;

  /* Bye-bye cruel world */
  
}
  kernel_sleep(EXITED, SCHED_USER);
}


