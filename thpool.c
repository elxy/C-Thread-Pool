/* ********************************
 * 
 * Author:  Johan Hanssen Seferidis
 * Date:    12/08/2011
 * Update:  09/13/2014
 *          Transfer to multiple job queue structure.
 *          by elxy
 * License: LGPL
 * 
 * 
 *//** @file thpool.h *//*
 ********************************/

/* Library providing a threading pool where you can add work. For an example on 
 * usage you refer to the main file found in the same package */

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

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <errno.h>
#include <unistd.h>

#include "thpool.h"      /* here you can also find the interface to each function */


static int thpool_keepalive=1;


/* Initialise thread pool */
thpool_t* thpool_init(int threadsN){
	thpool_t* tp_p;

	if (!threadsN || threadsN<1) threadsN=1;

	/* Make new thread pool */
	tp_p=(thpool_t*)malloc(sizeof(thpool_t));                              /* MALLOC thread pool */
	if (tp_p==NULL){
		fprintf(stderr, "thpool_init(): Could not allocate memory for thread pool\n");
		return NULL;
	}
	tp_p->threads=(pthread_t*)malloc(threadsN*sizeof(pthread_t));          /* MALLOC thread IDs */
	if (tp_p->threads==NULL){
		fprintf(stderr, "thpool_init(): Could not allocate memory for thread IDs\n");
		return NULL;
	}
	tp_p->threadsN=threadsN;

	/* Initialise each job queue */
	tp_p->jobqueue = (thpool_jobqueue **) malloc(tp_p->threadsN * sizeof(thpool_jobqueue *));

	int t;
	printf("tp_p->jobqueue's address is %p\n", tp_p->jobqueue);
	/* Make threads in pool */
	for (t = 0; t < threadsN; t++){
		if (thpool_jobqueue_init(& (tp_p->jobqueue[t])) == -1){
			fprintf(stderr, "thpool_init(): Could not allocate memory for job queues.\n"); /* MALLOCS INSIDE PTHREAD HERE */
			return NULL;
		}
		/* printf("tp_p->jobqueue[%d]'s address is %p\n", t, tp_p->jobqueue[t]); */
		/* printf("tp_p->jobqueue[%d] points %p\n", t, tp_p->jobqueue[t]); */
	}

	/* Make threads in pool */
	for (t=0; t<threadsN; t++){
		printf("Created thread %d in pool \n", t);
		pthread_create(&(tp_p->threads[t]), NULL, (void *)thpool_thread_do, (void *)tp_p->jobqueue[t]); /* MALLOCS INSIDE PTHREAD HERE */
	}

	return tp_p;
}


/* What each individual thread is doing 
 * */
/* There are two scenarios here. One is everything works as it should and second if
 * the thpool is to be killed. In that manner we try to BYPASS sem_wait and end each thread. */
void thpool_thread_do(thpool_jobqueue *jobqueue){

	while(thpool_keepalive){

		if (sem_wait(jobqueue->queueSem)){/* WAITING until there is work in the queue */
			perror("thpool_thread_do(): Waiting for semaphore");
			exit(1);
		}

		if (thpool_keepalive){
			/* Read job from queue and execute it */
			struct job_value job;

			thpool_jobqueue_get(jobqueue, &job);

			/* run function */
			job.function(job.arg);
		}else{
			return; /* EXIT thread*/
		}
	}

	return;
}


/* Add work to the thread pool */
int thpool_add_work(thpool_t *tp_p, int tag, void * (*function_p)(void *), void *arg_p){
	thpool_job_t *newJob = (thpool_job_t *) malloc(sizeof(thpool_job_t));         /* MALLOC job */
	/* printf("newJob's address is %p\n", newJob); */

	if (newJob==NULL){
		fprintf(stderr, "thpool_add_work(): Could not allocate memory for new job\n");
		exit(1);
	}

	/* add function and argument */
	newJob->value.function = function_p;
	newJob->value.arg = arg_p;

	/* choose a queue to add */
	/* To apply advanced task schedule, you need to change your way
	 * to produce tag. */
	int q_num = tag % tp_p->threadsN;

	/* add job to queue */
	thpool_jobqueue_add(tp_p->jobqueue[q_num], newJob);

	return 0;
}


/* Destroy the threadpool */
void thpool_destroy(thpool_t* tp_p){
	int t;

	/* End each thread's infinite loop */
	thpool_keepalive=0; 

	/* Awake idle threads waiting at semaphore and mutex*/
	for (t=0; t<(tp_p->threadsN); t++){
		if (sem_post(tp_p->jobqueue[t]->queueSem)){
			fprintf(stderr, "thpool_destroy(): Could not bypass sem_wait()\n");
		}

	/* Kill semaphore and mutex */
	for (t = 0; t < tp_p->threadsN; t++){
		if (sem_destroy(tp_p->jobqueue[t]->queueSem) != 0){
		fprintf(stderr, "thpool_destroy(): Could not destroy semaphore\n");
		}

		if (pthread_mutex_destroy(&tp_p->jobqueue[t]->q_tail_lock) ||
			pthread_mutex_destroy(&tp_p->jobqueue[t]->q_head_lock)){
			fprintf(stderr, "thpool_destroy(): Could not destroy q_tail_lock or q_head_lock.\n");
		}
	}

	/* Wait for threads to finish */
	for (t=0; t<(tp_p->threadsN); t++){
		pthread_join(tp_p->threads[t], NULL);
	}

	/* There are 2 reasons that we need to "reflush" job queue:
	 * 1. Awaken threads may add work in job queue.
	 * 2. Job queue won't be real empty, even thread think it's emplty. */
	for (t = 0; t < tp_p->threadsN; t++){
		thpool_jobqueue_empty(tp_p->jobqueue[t]);
	}

	/* Dealloc */
	free(tp_p->threads);

	for (t = 0; t < tp_p->threadsN; t++){
		free(tp_p->jobqueue[t]);
	}

	free(tp_p);
}


/* =================== JOB QUEUE OPERATIONS ===================== */

/* Initialise queue */
int thpool_jobqueue_init(thpool_jobqueue **jobqueue){
	*jobqueue = malloc(sizeof(thpool_jobqueue));

	if (jobqueue == NULL){
		return -1;
}

	/* We need an empty node to avoid 2 tricky special situation:
	 * 1. add first node to empty queue.
	 * 2. del last node from queue. */
	thpool_job_t *empty_job = malloc(sizeof(thpool_job_t));

	if (empty_job == NULL){
		free(*jobqueue);
		return -1;
	}

	empty_job->next = NULL;

	/* printf("empty_job's address is %p\n", empty_job); */
	(*jobqueue)->tail = empty_job;
	(*jobqueue)->head = empty_job;

	(*jobqueue)->queueSem = malloc(sizeof(sem_t));

	if((*jobqueue)->queueSem == NULL){
		free(*jobqueue);
		free(empty_job);
		return -1;
	}

	if (sem_init((*jobqueue)->queueSem, 0, 0) == -1){ /* no shared, initial value */
		perror("sem_init(): ");
		goto JOBQUEUE_INIT_ERROR;
	}

	if (pthread_mutex_init(& (*jobqueue)->q_head_lock, NULL) == -1 ||
		pthread_mutex_init(& (*jobqueue)->q_tail_lock, NULL) == -1){
		perror("phtread_mutex_init(): ");
		goto JOBQUEUE_INIT_ERROR;
	}

	return 0;

JOBQUEUE_INIT_ERROR:
	free(*jobqueue);
	free(empty_job);
	free((*jobqueue)->queueSem);
	return -1;
}


/* Add job to queue */
void thpool_jobqueue_add(thpool_jobqueue *jobqueue, thpool_job_t *newjob_p){
	newjob_p->next = NULL;

	pthread_mutex_lock(&jobqueue->q_tail_lock);

	jobqueue->tail->next = newjob_p;
	jobqueue->tail = newjob_p;

	sem_post(jobqueue->queueSem);

	pthread_mutex_unlock(&jobqueue->q_tail_lock);
}


/* Get value of the first job from queue and remove it. */
int thpool_jobqueue_get(thpool_jobqueue *jobqueue, struct job_value *pvalue){
	pthread_mutex_lock(&jobqueue->q_head_lock);

	thpool_job_t *node = jobqueue->head;
	thpool_job_t *new_head = node->next;

	if (new_head == NULL){
		pthread_mutex_unlock(&jobqueue->q_head_lock);
		return -1;
	}

	/* We don't return value of real head, but value of head->next. */
	*pvalue = new_head->value;
	jobqueue->head = new_head;

	pthread_mutex_unlock(&jobqueue->q_head_lock);
	free(node);

	return 0;
}


/* Peek out job queue to see whether there are jobs. */
thpool_job_t *thpool_jobqueue_peek(thpool_jobqueue *jobqueue){
	return jobqueue->head->next;
}


/* Remove and deallocate all jobs in queue */
void thpool_jobqueue_empty(thpool_jobqueue *jobqueue){
	thpool_job_t* curjob;
	curjob = jobqueue->head;

	while (jobqueue->head){
		jobqueue->head = curjob->next;
		free(curjob);
		curjob = jobqueue->head;
	}

	/* Fix head and tail */
	jobqueue->tail = NULL;
	jobqueue->head = NULL;
}

