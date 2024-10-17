#if defined(__APPLE__)
#include <AvailabilityMacros.h>
#else
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 500
#endif
#endif

#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>

#if defined(__linux__)
#include <sys/prctl.h>
#endif
#if defined(__FreeBSD__)||defined(__OpenBSD__)
#include <pthread_np.h>
#endif

#include "thpool.h"

#ifdef THPOOL_DEBUG
#define THPOOL_DEBUG 1
#else
#define THPOOL_DEBUG 0
#endif

#if !defined(DISABLE_PRINT) || defined(THPOOL_DEBUG)
#define err(str) fprintf(stderr, str)
#else
#define err(str)
#endif

#ifndef THPOOL_THREAD_NAME1
#define THPOOL_THREAD_NAME thpool
#endif

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

static volatile int threads_keepalive;  //线程池中的线程是否存活
static volatile int threads_on_hold;  //线程池中的线程是否暂停



/* ========================== STRUCTURES ============================ */


/* Binary semaphore 二值信号量 */
typedef struct bsem{
    pthread_mutex_t mutex;
    pthread_cond_t cond;  //条件变量，用于线程间通信
    int v;
}bsem;

/* Job */
typedef struct job{
    struct job* prev;
    void (*function)(void* arg);  //接受void* 的参数,返回void类型的值的函数指针
    void* arg;
}job;

/* Job queue */
typedef struct jobqueue{
    pthread_mutex_t rwmutex;  //工作队列为临界资源，需要互斥的访问
    job *front;
    job *rear;
    bsem *has_jobs;  //互斥的访问 工作队列中是否还有工作
    int len;
}jobqueue;

/* Thread */
typedef struct thread{
    int id;
    pthread_t pthread;
    struct thpool_* thpool_p;
}thread;

/* Threadpool */
typedef struct thpool_{
    thread** threads;
    volatile int num_threads_alive;
    volatile int num_threads_working;
    pthread_mutex_t thcount_lock;  //用于互斥的访问线程计数器
    pthread_cond_t threads_all_idle;  //所有线程都空闲，用于等待所有线程完成
    jobqueue jobqueue;
} thpool_;



/* ========================== PROTOTYPES 函数声明 ============================ */

static int thread_init(thpool_* thpool_p, struct thread** thread_p, int id);
static void* thread_do(struct thread* thread_p);  //每个线程执行的函数
static void thread_hold(int sig_id);
static void thread_destroy(struct thread* thread_p);

static int   jobqueue_init(jobqueue* jobqueue_p);
static void  jobqueue_clear(jobqueue* jobqueue_p);
static void  jobqueue_push(jobqueue* jobqueue_p, struct job* newjob_p);
static struct job* jobqueue_pull(jobqueue* jobqueue_p);
static void  jobqueue_destroy(jobqueue* jobqueue_p);

static void  bsem_init(struct bsem *bsem_p, int value);
static void  bsem_reset(struct bsem *bsem_p);
static void  bsem_post(struct bsem *bsem_p);
static void  bsem_post_all(struct bsem *bsem_p);
static void  bsem_wait(struct bsem *bsem_p);


/* ========================== THREADPOOL ============================ */


/* Initialise thread pool */
struct thpool_* thpool_init(int num_threads){
    threads_on_hold = 0;
    threads_keepalive = 1;
    
    if(num_threads<0){
        num_threads = 0;
    }

    /* Make new thread pool*/
    thpool_* thpool_p;
    thpool_p = (struct thpool_*)malloc(sizeof(struct thpool_));
    if(thpool_p==NULL){
        err("thpool_init(): Could not allocate memory for thread pool\n");
        return NULL;
    }
    thpool_p->num_threads_alive = 0;
    thpool_p->num_threads_working = 0;

    /* Initialise the job queue */
    if(jobqueue_init(&thpool_p->jobqueue)==-1){
        err("thpool_init(): Could not allocate memory for job queue\n");
        free(thpool_p);
        return NULL;
    }

    /* Make threads in pool */
    thpool_p->threads = (struct thread**)malloc(num_threads*sizeof(struct thread*));  //申请num_threads个线程指针的空间
    if(thpool_p->threads==NULL){
        err("thpool_init(): Could not allocate memory for threads\n");
        jobqueue_destroy(&thpool_p->jobqueue);
        free(thpool_p);
        return NULL;
    }

    pthread_mutex_init(&(thpool_p->thcount_lock), NULL);
	pthread_cond_init(&thpool_p->threads_all_idle, NULL);

    /* Thread init */
    for(int i = 0;i<num_threads;i++){
        thread_init(thpool_p, &thpool_p->threads[i], i);  //初始化每个线程
#if THPOOL_DEBUG
			printf("THPOOL_DEBUG: Created thread %d in pool \n", i);
#endif
    }

    /* Wait for threads to initialize*/
    while(thpool_p->num_threads_alive!=num_threads){}  //等待所有线程申请完毕

    return thpool_p;
}

/* Add work to the thread pool */
int thpool_add_work(thpool_* thpool_p, void(*fuction_p)(void*), void* arg_p){
    job* newjob;

    newjob=(struct job*)malloc(sizeof(struct job));
    if(newjob==NULL){
        err("thpool_add_work(): Could not allocate memory for new job\n");
        return -1;
    }

    /* add function and argument*/
    newjob->function = fuction_p;
    newjob->arg = arg_p;

    /* add job to queue */
    jobqueue_push(&thpool_p->jobqueue, newjob);

    return 0;
}

/* Wait until all jobs have finished */
void thpool_wait(thpool_* thpool_p){
    pthread_mutex_lock(&thpool_p->thcount_lock);
    while(thpool_p->jobqueue.len||thpool_p->num_threads_working){
        pthread_cond_wait(&thpool_p->threads_all_idle, &thpool_p->thcount_lock);
    }
    pthread_mutex_unlock(&thpool_p->thcount_lock);
}

/* Destroy the threadpool */
void thpool_destroy(thpool_* thpool_p){
    /* No need to destroy if it's NULL */
    if(thpool_p==NULL) return;

    volatile int threads_total = thpool_p->num_threads_alive;

    /* End each thread's infinite loop */
    threads_keepalive = 0;   //每一个线程的循环条件都是 while(threads_keepalive)

    /* Give one second to kill idle threads */
    double TIMEOUT = 1.0;
    time_t start, end;
    double tpassed = 0;
    time(&start);
    while(tpassed<TIMEOUT&&thpool_p->num_threads_alive){
        bsem_post_all(thpool_p->jobqueue.has_jobs);  //这是因为所有空闲线程都阻塞在bsem_wait处等待唤醒（这里是唤醒所有）
        time(&end);
        tpassed = difftime(end, start);
    }

    /* Poll remaining threads */
    while (thpool_p->num_threads_alive)
    {
        bsem_post_all(thpool_p->jobqueue.has_jobs);
        sleep(1);
    }
    
    /* Job queue cleanup */
    jobqueue_destroy(&thpool_p->jobqueue);

    /* Deallocs 释放内存*/
    for(int i = 0;i<threads_total;i++){
        thread_destroy(thpool_p->threads[i]);
    }
    free(thpool_p->threads);
    free(thpool_p);

}


/* Pause all threads in threadpool */
void thpool_pause(thpool_* thpool_p){
    for(int i = 0;i<thpool_p->num_threads_alive;i++){
        pthread_kill(thpool_p->threads[i]->pthread, SIGUSR1);  //向线程发送终止(暂停)信号
    }
}

/* Resume all threads in threadpool */
void thpool_resume(thpool_* thpool_p){
    // resuming a single threadpool hasn't been
    // implemented yet, meanwhile this suppresses
    // the warnings
    (void)thpool_p;

	threads_on_hold = 0;
}

int thpool_num_threads_working(thpool_* thpool_p){
    return thpool_p->num_threads_working;
}



/* ============================ THREAD ============================== */

/* 初始化线程池中的线程 */
static int thread_init(thpool_* thpool_p, struct thread** thread_p, int id){

    *thread_p = (struct thread*)malloc(sizeof(struct thread));
    if(*thread_p==NULL){
        err("thread_init(): Could not allocate memory for thread\n");
        return -1;
    }

    (*thread_p)->thpool_p = thpool_p;
    (*thread_p)->id = id;

    pthread_create(&(*thread_p)->pthread, NULL, (void*(*)(void*)) thread_do, (*thread_p));
    pthread_detach((*thread_p)->pthread);
    return 0;
}

/* Sets the calling thread on hold */
static void thread_hold(int sig_id) {
    (void)sig_id;
	threads_on_hold = 1;
	while (threads_on_hold){
        // printf("onHold\n");
		sleep(1);
	}
}

/* What each thread is doing */

static void* thread_do(struct thread* thread_p){
    /* Set thread name for profiling and debugging */
    /* 设置一个线程的名称，这在性能分析（profiling）和调试（debugging）过程中非常有用。*/
	char thread_name[16] = {0};

	snprintf(thread_name, 16, TOSTRING(THPOOL_THREAD_NAME) "-%d", thread_p->id);

/*在不同操作系统上设置线程名称的跨平台条件编译块*/
#if defined(__linux__)
	/* Use prctl instead to prevent using _GNU_SOURCE flag and implicit declaration */
	prctl(PR_SET_NAME, thread_name);
#elif defined(__APPLE__) && defined(__MACH__)
	pthread_setname_np(thread_name);
#elif defined(__FreeBSD__) || defined(__OpenBSD__)
    pthread_set_name_np(thread_p->pthread, thread_name);
#else
	err("thread_do(): pthread_setname_np is not supported on this system");
#endif

    /* Assure all threads have been created before starting serving */
	thpool_* thpool_p = thread_p->thpool_p;

    /* Register signal handler */
	struct sigaction act;
	sigemptyset(&act.sa_mask);
	act.sa_flags = SA_ONSTACK;
	act.sa_handler = thread_hold;
	if (sigaction(SIGUSR1, &act, NULL) == -1) {  //当发送暂停信号时，线程转而执行thread_hold函数
		err("thread_do(): cannot handle SIGUSR1");
	}

    /* Mark thread as alive (initialized) */
	pthread_mutex_lock(&thpool_p->thcount_lock);
	thpool_p->num_threads_alive += 1;
	pthread_mutex_unlock(&thpool_p->thcount_lock);


	while(threads_keepalive){

		bsem_wait(thpool_p->jobqueue.has_jobs);  //如果队列中没有工作就阻塞在这里等待

		if (threads_keepalive){

			pthread_mutex_lock(&thpool_p->thcount_lock);
			thpool_p->num_threads_working++;
			pthread_mutex_unlock(&thpool_p->thcount_lock);

			/* Read job from queue and execute it */
			void (*func_buff)(void*);
			void*  arg_buff;
			job* job_p = jobqueue_pull(&thpool_p->jobqueue);
			if (job_p) {
				func_buff = job_p->function;
				arg_buff  = job_p->arg;
				func_buff(arg_buff);
				free(job_p);
			}

			pthread_mutex_lock(&thpool_p->thcount_lock);
			thpool_p->num_threads_working--;
			if (!thpool_p->num_threads_working) {
				pthread_cond_signal(&thpool_p->threads_all_idle);
			}
			pthread_mutex_unlock(&thpool_p->thcount_lock);

		}
	}
	pthread_mutex_lock(&thpool_p->thcount_lock);
	thpool_p->num_threads_alive --;
	pthread_mutex_unlock(&thpool_p->thcount_lock);

	return NULL;
}

/* Frees a thread */
static void thread_destroy(thread* thread_p){
    free(thread_p);
}


/* ============================ JOB QUEUE =========================== */


/* Initialize queue */
static int jobqueue_init(jobqueue* jobqueue_p){
    jobqueue_p->len = 0;
    jobqueue_p->front = NULL;
    jobqueue_p->rear = NULL;

    jobqueue_p->has_jobs = (struct bsem*)malloc(sizeof(struct bsem));  
    if(jobqueue_p->has_jobs==NULL){
        return -1;
    }

    pthread_mutex_init(&(jobqueue_p->rwmutex), NULL);
    bsem_init(jobqueue_p->has_jobs, 0);

    return 0;
}

/* clear the queue */
static void jobqueue_clear(jobqueue* jobqueue_p){

    while(jobqueue_p->len){
        free(jobqueue_pull(jobqueue_p)); //取出工作进行释放
    }

    jobqueue_p->front = NULL;
    jobqueue_p->rear = NULL;
    bsem_reset(jobqueue_p->has_jobs);
    jobqueue_p->len = 0;
}

/* Add(allocated) job to queue*/
static void jobqueue_push(jobqueue* jobqueue_p, struct job* newjob){

    pthread_mutex_lock(&jobqueue_p->rwmutex);
    newjob->prev = NULL;
    switch(jobqueue_p->len){
        case 0:  //队列中没有工作
            jobqueue_p->front = newjob;
            jobqueue_p->rear = newjob;
            break;
        default:
            jobqueue_p->rear->prev = newjob;  //这里的prev指针是从front指向rear
            jobqueue_p->rear = newjob;

    }
    jobqueue_p->len++;

    bsem_post(jobqueue_p->has_jobs);
    pthread_mutex_unlock(&jobqueue_p->rwmutex);
}

/* Get first job from queue(removes it from queue)
 * Notice: Caller MUST hold a mutex
 */
static struct job* jobqueue_pull(jobqueue* jobqueue_p){

	pthread_mutex_lock(&jobqueue_p->rwmutex);
	job* job_p = jobqueue_p->front;

	switch(jobqueue_p->len){

		case 0:  /* if no jobs in queue */
		  			break;

		case 1:  /* if one job in queue */
					jobqueue_p->front = NULL;
					jobqueue_p->rear  = NULL;
					jobqueue_p->len = 0;
					break;

		default: /* if >1 jobs in queue */
					jobqueue_p->front = job_p->prev;
					jobqueue_p->len--;
					/* more than one job in queue -> post it */
					bsem_post(jobqueue_p->has_jobs);  //取出一个

	}

	pthread_mutex_unlock(&jobqueue_p->rwmutex);
	return job_p;
}

/* Free all queue resources back to the system */
static void jobqueue_destroy(jobqueue* jobqueue_p){
	jobqueue_clear(jobqueue_p);
	free(jobqueue_p->has_jobs);
}


/* ======================== SYNCHRONISATION ========================= */

/* Init semaphore to 1 or 0*/
static void bsem_init(bsem* bsem_p, int value){
    if(value<0||value>1){
        err("bsem_init(): Binary semaphore can take only values 1 or 0");
		exit(1);
    }
    pthread_mutex_init(&(bsem_p->mutex), NULL);
    pthread_cond_init(&(bsem_p->cond), NULL);
    bsem_p->v = value;
}

/* Reset semaphore to 0 */
static void bsem_reset(bsem *bsem_p) {
	pthread_mutex_destroy(&(bsem_p->mutex));
	pthread_cond_destroy(&(bsem_p->cond));
	bsem_init(bsem_p, 0);
}

/* Post to at least one thread */
static void bsem_post(bsem *bsem_p) {
	pthread_mutex_lock(&bsem_p->mutex);
	bsem_p->v = 1;
	pthread_cond_signal(&bsem_p->cond);
	pthread_mutex_unlock(&bsem_p->mutex);
}

/* Post to all threads */
static void bsem_post_all(bsem *bsem_p) {
	pthread_mutex_lock(&bsem_p->mutex);
	bsem_p->v = 1;
	pthread_cond_broadcast(&bsem_p->cond);
	pthread_mutex_unlock(&bsem_p->mutex);
}

/* Wait on semaphore until semaphore has value 0 */
static void bsem_wait(bsem* bsem_p) {
	pthread_mutex_lock(&bsem_p->mutex);
	while (bsem_p->v != 1) {
		pthread_cond_wait(&bsem_p->cond, &bsem_p->mutex);
	}
	bsem_p->v = 0;  //一旦信号量的值变为1（表示可用），等待的线程将继续执行并将信号量的值设置为0，表示信号量现在已被占用。
	pthread_mutex_unlock(&bsem_p->mutex);
}