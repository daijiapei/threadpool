
#ifndef __THREAD_POOL_H__
#define __THREAD_POOL_H__


#ifdef _WIN32
#include <time.h>
#define  TPSTDCALL __stdcall
#else
#include <sys/time.h>
//#define TPSTDCALL __attribute__((__stdcall__))
#define TPSTDCALL
#endif

#ifndef TRUE
#define TRUE 1
#endif

#ifndef FALSE
#define FALSE 0
#endif

typedef unsigned long thread_ulong_t;
typedef int thread_bool_t;

typedef void * (TPSTDCALL * queue_hook_cb)(void *, void *);
typedef void(TPSTDCALL * timer_hook_cb)(void * arg, long event);

struct __THREADPOOL;

typedef struct __THREADPOOL * THREADPOOL;
typedef unsigned int THREADTIMER;

typedef struct __HQUEUE__
{
    void * unused;
} *HQUEUE;

typedef unsigned int HQTIMER;



#ifdef __cplusplus
extern "C" {
#endif

    //one loop per thread
    THREADPOOL threadpool_create(unsigned int thread_num, unsigned int max_thread_num, unsigned int queue_max_num);
    thread_bool_t threadpool_destroy(THREADPOOL pool);
    int threadpool_add_job(THREADPOOL pool, thread_bool_t priority, queue_hook_cb callback, void * obj, void *arg);

    //the current queue equal to max queue count, should block?
    thread_bool_t threadpool_block_full(THREADPOOL pool, thread_bool_t block);
    thread_bool_t threadpool_active(THREADPOOL pool);//only wake one thread , not thing todo
    THREADTIMER threadpool_set_timer(THREADPOOL pool, time_t timeout, timer_hook_cb callback, void * arg, long event);
    thread_bool_t threadpool_kill_timer(THREADPOOL pool, THREADTIMER timer);


    //one loop per thread
    HQUEUE queue_create(unsigned int max_message);
    unsigned int queue_dispatch(HQUEUE queue);
    int queue_post(HQUEUE queue, queue_hook_cb callback, void * obj, void *arg);
    thread_bool_t queue_break(HQUEUE queue);
    thread_bool_t queue_destroy(HQUEUE queue);

    HQTIMER queue_set_timer(HQUEUE queue, time_t timeout, timer_hook_cb callback, void * arg, long event);
    thread_bool_t queue_kill_timer(HQUEUE queue, HQTIMER timer);

#ifdef __cplusplus
};
#endif


#endif