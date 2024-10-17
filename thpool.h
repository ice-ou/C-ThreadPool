#ifndef _THPOOL_  // avoid redefinition
#define _THPOOL_

#ifdef __cplusplus  //use c++ compiler
extern "C"{
#endif

/* =================================== API ======================================= */

typedef struct thpool_* threadpool;

//Initialize threadpool
threadpool thpool_init(int num_threads);

//add work to the job queue
int thpool_add_work(threadpool, void(*function_p)(void*), void* arg_p);  
//fuction_p为一个函数指针，指向将要执行的任务函数。这个函数必须接受一个 void* 类型的参数，并且没有返回值。

//wait for all queued jobs to finish
void thpool_wait(threadpool);

//pause all threads immediately
void thpool_pause(threadpool);

//Unpauses all threads if they are paused
void thpool_resume(threadpool);

//destory the threadpool
void thpool_destroy(threadpool);

//show currently working threads
int thpool_num_threads_working(threadpool);

#ifdef __cplusplus
}
#endif

#endif