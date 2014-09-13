/********************************** 
 * @author      Johan Hanssen Seferidis
 * @date        12/08/2011
 * Last update: 09/13/2014
 *              Transfer to multiple job queue structure.
 *              by elxy
 * License:     LGPL
 * 
 **********************************/

/* Description: Library providing a threading pool where you can add work on the fly. The number
 *              of threads in the pool is adjustable when creating the pool. In most cases
 *              this should equal the number of threads supported by your cpu.
 *          
 *              For an example on how to use the threadpool, check the main.c file or just read
 *              the documentation.
 * 
 *              In this header file a detailed overview of the functions and the threadpool logical
 *              scheme is present in case tweaking of the pool is needed. 
 * */

/* 
 * Fast reminders:
 * 
 * tp           = threadpool 
 * thpool       = threadpool
 * thpool_t     = threadpool type
 * tp_p         = threadpool pointer
 * tag          = job's tag
 * sem          = semaphore
 * h_lock       = lock of job queue head
 * t_lock       = lock of job queue tail
 * xN           = x can be any string. N stands for amount
 * 
 * */
                  
/*              _____________________________________________________________        
 *             /                                                             \
 *             |   JOB QUEUEs      | queue 1 | job | job | job | job | ..    |
 *             |                   | queue 2 | job | job | job | job | ..    |
 *             |                   | queue 3 | job | job | job | job | ..    |
 *             |                      ...                                    |
 *             |                                                             |
 *             |   threadpool      | thread1 | thread2 | thread3 | ..        |
 *             \_____________________________________________________________/
 * 
 *    Description:       Jobs are added to the job queues. Which queue it wiil be added
 *                       to depends on tag (or use hash func). Once a thread in the pool
 *                       is idle, it is assigned with the first job from its queue (and
 *                       erased from the queue). It's each thread's job to read from the
 *                       queue and executing each job until the queue is empty. All
 *                       threads could add jobs to job queues. We use lock to guarantee
 *                       adding serial.
 * 
 * 
 *    Scheme:
 * 
 *    thpool______                jobqueue1___                      ______ 
 *    |           |               |           |       .----------->|_job0_| Job for thread to take
 *    | jobqueue1---------------->|  head------------'             |_job1_|
 *    |           |               |           |                    |_job2_|
 *    | jobqueue2 |               |  tail------------.             |__..__| 
 *    |    ...    |               |___________|       '----------->|_jobn_| Newly added job
 *    |___________|
 * 
 * 
 *    job0________ 
 *    |           |
 *    | function---->
 *    |           |
 *    |   arg------->
 *    |           |         job1________ 
 *    |  next-------------->|           |
 *    |___________|         |           |..
 */

#ifndef _THPOOL_

#define _THPOOL_

#include <pthread.h>
#include <semaphore.h>



/* ================================= STRUCTURES ================================================ */


/* Individual job */
typedef struct thpool_job_t{
	struct job_value {
	void*  (*function)(void* arg);                     /**< function pointer         */
	void*                     arg;                     /**< function's argument      */
	}                       value;
	struct thpool_job_t*     next;                     /**< pointer to next job      */
}thpool_job_t;


/* Job queue as doubly linked list */
/* We use the Two-Lock Concurrent Queue Algorithm to maintain job queue.
 * Ref: http://www.cs.rochester.edu/research/synchronization/pseudocode/queues.html
 */
typedef struct thpool_jobqueue{
	thpool_job_t *head;                                /**< pointer to head of queue */
	thpool_job_t *tail;                                /**< pointer to tail of queue */
	sem_t          *queueSem;                          /**< semaphore(this is probably just holding the same as jobsN) */
	pthread_mutex_t q_head_lock;                       /**< lock of head */
	pthread_mutex_t q_tail_lock;                       /**< lock of tail */
}thpool_jobqueue;


/* The threadpool */
typedef struct thpool_t{
	pthread_t*       threads;                          /**< pointer to threads' ID   */
	int              threadsN;                         /**< amount of threads        */
	/* Multiple job queues have following benefits:
	 * 1. avoid performance loss when many threads race for one lock.
	 * 2. enable complex job scheduling, making it possible that specific
	 *    thread handle specific job. */
	thpool_jobqueue   **jobqueue;                      /**< pointer to the job queues */
}thpool_t;



/* =========================== FUNCTIONS ================================================ */


/* ----------------------- Threadpool specific --------------------------- */

/**
 * @brief  Initialize threadpool
 * 
 * Allocates memory for the threadpool, jobqueue, semaphore and fixes 
 * pointers in jobqueue.
 * 
 * @param  number of threads to be used
 * @return threadpool struct on success,
 *         NULL on error
 */
thpool_t* thpool_init(int threadsN);


/**
 * @brief What each thread is doing
 * 
 * In principle this is an endless loop. The only time this loop gets interuppted is once
 * thpool_destroy() is invoked.
 * 
 * @param  job queue to use
 * @return nothing
 */
void thpool_thread_do(thpool_jobqueue *jobqueue);


/**
 * @brief Add work to the job queue
 * 
 * Takes an action and its argument and adds it to the threadpool's job queue.
 * If you want to add to work a function with more than one arguments then
 * a way to implement this is by passing a pointer to a structure.
 * 
 * ATTENTION: You have to cast both the function and argument to not get warnings.
 * 
 * @param  threadpool to where the work will be added to
 * @param  tag to determine which job queue this work should be added
 * @param  function to add as work
 * @param  argument to the above function
 * @return int
 */
int thpool_add_work(thpool_t *tp_p, int tag, void * (*function_p)(void *), void *arg_p);


/**
 * @brief Destroy the threadpool
 * 
 * This will 'kill' the threadpool and free up memory. If threads are active when this
 * is called, they will finish what they are doing and then they will get destroyied.
 * 
 * @param threadpool a pointer to the threadpool structure you want to destroy
 */
void thpool_destroy(thpool_t* tp_p);



/* ------------------------- Queue specific ------------------------------ */


/**
 * @brief Initialize queue
 * @param  pointer to job queue's pointer
 * @return 0 on success,
 *        -1 on memory allocation error
 */
int thpool_jobqueue_init(thpool_jobqueue **jobqueue);


/**
 * @brief Add job to queue
 * 
 * A new job will be added to the queue. The new job MUST be allocated
 * before passed to this function or else other functions like thpool_jobqueue_empty()
 * will be broken.
 * 
 * @param pointer to job queue
 * @param pointer to the new job(MUST BE ALLOCATED)
 * @return nothing 
 */
void thpool_jobqueue_add(thpool_jobqueue *jobqueue, thpool_job_t *newjob_p);


/**
 * @brief Get value of the first job from queue and remove it.
 * 
 * This does not free allocated memory so be sure to have peeked() \n
 * before invoking this as else there will result lost memory pointers.
 * 
 * @param pointer to job queue
 * @return 0 on success,
 *         -1 if queue is empty
 */
int thpool_jobqueue_get(thpool_jobqueue *jobqueue, struct job_value *pvalue);


/** 
 * @brief Get last job in queue (tail)
 * 
 * Gets the last job that is inside the queue. This will work even if the queue
 * is empty.
 * 
 * @param pointer to job queue
 * @return job a pointer to the last job in queue,
 *         a pointer to NULL if the queue is empty
 */
thpool_job_t *thpool_jobqueue_peek(thpool_jobqueue *jobqueue);


/**
 * @brief Remove and deallocate all jobs in queue
 * 
 * This function will deallocate all jobs in the queue and set the
 * jobqueue to its initialization values, thus tail and head pointing
 * to NULL and amount of jobs equal to 0.
 * 
 * @param pointer to job queue
 * */
void thpool_jobqueue_empty(thpool_jobqueue *jobqueue);



#endif
