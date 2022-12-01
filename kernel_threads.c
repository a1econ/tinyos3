
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

  /* put the new ptcb in the list of the CURPROC */
  rlist_push_back(&CURPROC->PTCB_list, &ptcb->PTCB_node);

  printf("sys_CreateThread: process %d, current thread %p new thread %p\n", get_pid(CURPROC), cur_thread()->owner_ptcb, ptcb);

  /* wake up the new tcb */
  wakeup(newtcb);
  //printf("sys_CreateThread: exiting\n");
  
	return(Tid_t)ptcb;
}

/**
  @brief Return the Tid of the current thread.
 */
Tid_t sys_ThreadSelf()
{
	return (Tid_t) cur_thread()->owner_ptcb;
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
  PTCB *ptcb = (PTCB *)tid;
  rlnode *node = rlist_find(&CURPROC->PTCB_list, ptcb, NULL);

  printf("sys_ThreadJoin: current thread: %p join thread %p\n", sys_ThreadSelf(), tid);

  /*- the tid thread doesn't belong to the same process (PCB)*/
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

  /* Make the current thread wait until the tid exits */
  while(ptcb->detached == 0 && ptcb->exited == 0) {
    kernel_wait(&ptcb->exit_cv, SCHED_USER);
  }

  printf("sys_ThreadJoin: thread %p continuing... \n", sys_ThreadSelf());

  /* If tid is detached while waiting return -1 */
  if(ptcb->detached) {
    return -1;
  }

  /* In this level the (PTCB*)tid has exited and the current thread does not have to wait any longer*/
  ptcb->refcount--;

  /**/
  if(exitval != NULL){
    *exitval = ptcb->exitval;
  }

  /**/
  if(ptcb->refcount <= 0){
    rlist_remove(&ptcb->PTCB_node);
    free(ptcb);
  }

  printf("sys_ThreadJoin: thread %p exiting... \n", sys_ThreadSelf());
	return 0;
}

/**
  @brief Detach the given thread.
  * 
  *  -there is no thread with the given tid in this process.
  *  -the tid corresponds to a detached thread.
*/
int sys_ThreadDetach(Tid_t tid)
{ 
  PTCB *ptcb = (PTCB *)tid;
  rlnode *node = rlist_find(&CURPROC->PTCB_list, ptcb, NULL);

  if(tid == NOTHREAD || node == NULL){
    return -1;
  }

  //if(ptcb->detached == 1 || ptcb->exited == 1) {
  /* Dont check for detached thread, a thread can be detched many times */
  if(ptcb->exited == 1) {
    return -1;
  }

  /* detach thread */
  ptcb->detached = 1; 
  kernel_broadcast(&ptcb->exit_cv);
  
  return 0;
}

/**
  @brief Terminate the current thread.
  */

void sys_ThreadExit(int exitval)
{
  PCB* curproc = CURPROC;
  TCB* curthread = cur_thread();
  PTCB* curptcb = curthread->owner_ptcb;

  //curptcb->exitval = exitval;
  //curptcb->exited = 1;
  //curptcb->tcb = NULL;

  //curproc->thread_count --;

  assert((cur_thread()->owner_ptcb) != NULL);

  /* Send a signal to the waiting threads */
  //if(curptcb->refcount != 0){
  //  kernel_broadcast(&curptcb->exit_cv);
  //}

  printf("sys_ThreadExit: process %d with state %d, thread %p\n", get_pid(curproc), curproc->pstate, curptcb);
  /* check if current thread is the main thread */
  if(curthread == curproc->main_thread){
    printf("sys_ThreadExit: this is the main thread... process %d with state:%d\n", get_pid(curproc), curproc->pstate);
#if 1
    if(get_pid(curproc)==1) {
      while(sys_WaitChild(NOPROC,NULL)!=NOPROC);
    }

    /* Do all the other cleanup we want here, close files etc. */

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

    /* Reparent any children of the exiting process to the initial task */
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
      kernel_broadcast(&initpcb->child_exit);
    }

    /* Put me into my parent's exited list */
    if(curproc->parent != NULL) {   /* Maybe this is init */
      rlist_push_front(&curproc->parent->exited_list, &curproc->exited_node);
      kernel_broadcast(&curproc->parent->child_exit);
    }

    assert(is_rlist_empty(& curproc->children_list));
    assert(is_rlist_empty(& curproc->exited_list));
    //curproc->pstate = ZOMBIE;
#endif
    curproc->main_thread = NULL;
    curproc->exitval = exitval;
    /*
    rlnode *temp;
    printf("sys_ExitThread: main thread, thread list length: %d\n", rlist_len(&CURPROC->PTCB_list));
    for(int i = 0; i < rlist_len(&CURPROC->PTCB_list); i++){
      temp = rlist_pop_front(&CURPROC->PTCB_list);
      if (temp->ptcb == curptcb) {
          continue;
      }
      printf("sys_ThreadExit: main thread waiting for thread %p\n", temp->ptcb);
      rlist_push_back(&CURPROC->PTCB_list, temp);

      temp->ptcb->refcount++;
      kernel_wait(&temp->ptcb->exit_cv, SCHED_USER);
      temp->ptcb->refcount--;
      exitval = temp->ptcb->exitval;
      printf("sys_ExitThread: main thread, thread list length: %d\n", rlist_len(&CURPROC->PTCB_list));
    }
    //kernel_sleep(STOPPED, SCHED_USER);
    printf("sys_ThreadExit: setting process to zombie state end exiting... with exit value:%d\n", exitval);
    */
   //sys_Exit(exitval);
  }
 
  printf("sys_ExitThread: thread %p refcount: %d\n", curptcb, curptcb->refcount);

  curptcb->exitval = exitval;
  curptcb->exited = 1;

  /* Send a signal to the waiting threads */
  if(curptcb->refcount != 0){
    kernel_broadcast(&curptcb->exit_cv);
  }

  curproc->thread_count--;

  /* clean up if no more threads */
  if(curproc->thread_count == 0){
    rlnode *temp;

    while(!is_rlist_empty(&CURPROC->PTCB_list)) {
      temp = rlist_pop_front(&CURPROC->PTCB_list);
      free(temp->ptcb);
    }
  } 
  //rlnode *tmp = rlist_remove(&curptcb->PTCB_node);
  //free(tmp->ptcb);
  //curptcb->tcb = NULL;
  printf("sys_ThreadExit: thread %p exiting with exit value:%d, thread count=%d\n", curptcb, exitval, curproc->thread_count);

  // Stop the thread
  kernel_sleep(EXITED, SCHED_USER);
}