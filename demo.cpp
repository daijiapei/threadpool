
#include "threadpool.h"

#include <stdio.h>


#ifdef _WIN32
#include <Windows.h>
#else
#include <time.h>
#include <pthread.h>
#endif
//#pragma warning (disable:4700);
#define __THREAD_POOL_DEMO__

#ifdef __THREAD_POOL_DEMO__
#define demo  main
#endif

void * TPSTDCALL work_cb(void *, void *);

void TPSTDCALL timer_cb(void *, long);

THREADPOOL pool = NULL;

HQUEUE queue = NULL;

int demo(int argc, char **argv)
{
    HQTIMER timerid = NULL;
#if 1
    pool = threadpool_create(2, 4, 2000);

    timerid = threadpool_set_timer(pool, 1000, timer_cb, 0, 0);

#ifdef _WIN32
    SleepEx(3000, TRUE);
#else
    getchar();
#endif

    threadpool_kill_timer(pool, timerid);

    threadpool_destroy(pool);

#else

    queue = queue_create(1024);

    HQTIMER timerid = queue_set_timer(queue, 1000, timer_cb, 0, 0);

    queue_dispatch(queue);

    queue_kill_timer(queue, timerid);
    queue_destroy(queue);

#endif
    return 0;

}

void show_time()
{
#ifdef _WIN32
    SYSTEMTIME time = { 0 };
    GetLocalTime(&time);
    printf("id=%ld time = %02d:%02d:%02d:%03d\t",GetCurrentThreadId(), time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);

#else

    time_t t;
    struct tm *lt;
    time(&t);
    lt = localtime(&t);

    printf("id=%d time = %02d:%02d:%02d:%03d\t",pthread_self(), lt->tm_hour, lt->tm_min, lt->tm_sec, lt->tm_isdst);

#endif
}

void * TPSTDCALL work_cb(void *, void *)
{
    show_time();

    //queue_break(queue);
    printf("hello world!\n");
    return 0;
}

void TPSTDCALL timer_cb(void *, long)
{
    show_time();
    printf("timer event callback!\n");

    for (int i = 0; 10 > i; i++)
    {
        threadpool_add_job(pool, 0, work_cb, 0, 0);

        //queue_post(queue, work_cb, 0, 0);
    }

    //Sleep(1000);
}