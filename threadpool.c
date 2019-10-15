
#include "threadpool.h"

#include <stdio.h>
#include <assert.h>

#define FOREVER  -1
//#define FOREVER   ((unsigned long)~((unsigned long)0))
#define threadpool_malloc(l) malloc(l)
#define threadpool_realooc(p, l) realloc(p, l)
#define threadpool_free(p) free(p)

#ifdef _WIN32
#include <process.h>
#include <Windows.h>

typedef HANDLE                   _thread_t;
typedef CONDITION_VARIABLE       _thread_cond_t;
typedef CRITICAL_SECTION         _thread_mutex_t;

#define threadpool_lock(pool) EnterCriticalSection(&(pool)->lock)
#define threadpool_unlock(pool) LeaveCriticalSection(&(pool)->lock)
#define threadpool_wake_one(cv) WakeConditionVariable(&(cv))
#define threadpool_wake_all(cv) WakeAllConditionVariable(&(cv))

#define get_current_thread_id  GetCurrentThreadId


#else
#include <stdlib.h>
#include <pthread.h>
#include <errno.h>

#define max(a,b)    (((a) > (b)) ? (a) : (b))
#define ERROR_TIMEOUT  ETIMEDOUT

typedef pthread_t                   _thread_t;
typedef pthread_cond_t       _thread_cond_t;
typedef pthread_mutex_t         _thread_mutex_t;

#define threadpool_lock(pool) pthread_mutex_lock(&(pool)->lock)
#define threadpool_unlock(pool) pthread_mutex_unlock(&(pool)->lock)
#define threadpool_wake_one(cv) pthread_cond_signal(&(cv))
#define threadpool_wake_all(cv) pthread_cond_broadcast(&(cv))


#define get_current_thread_id  pthread_self

#endif




typedef unsigned long thread_ulong_t;
typedef int thread_bool_t;


typedef struct __HEAPELEMENT
{
    time_t dwValue;
    thread_ulong_t * pdwIndex;
    void * pObject;
}HEAPELEMENT;

typedef struct __MINHEAP
{
    thread_ulong_t dwIncrement;//空间不足时,每次递增多少空间
    thread_ulong_t dwCount;//元素数量
    HEAPELEMENT * lpArrayElement;
}MINHEAP;

typedef struct __JOB
{
    thread_bool_t priority;//是否优先处理
    queue_hook_cb callback;//回调
    void * obj;//参数一，可以作为C++ this指针
    void * arg;//参数二，可以作为C++ class成员函数参数
    struct __JOB * next;
}JOB, *PJOB;

typedef struct __POOLTIMER
{
    thread_ulong_t index;//小根堆的位置
    thread_ulong_t ref;//引用计数

    thread_ulong_t threadid;

    //需要一个事件(HEVENT)或者条件变量来保证数据的线程安全,因为当timer执行时,会跳出锁
    //如果此时正好释放定时器的话,就会引起崩溃,所以当定时器开始时,先设置HEVENT
    //为无信号, 定时器执行完毕后,设置为有信号
    //void * signal;
    _thread_cond_t cv_kill;
    time_t timeout;//毫秒
    timer_hook_cb callback;
    long event;
    void * arg;
}POOLTIMER;

struct __THREADPOOL{
    int thread_num;//常驻线程数
    int thread_max_num;//最大线程数
    int thread_cur_num;//当前线程数
    int queue_cur_num;//当前队列消息数
    int queue_max_num;//最大队列消息数
    thread_bool_t thread_work;//线程是否继续工作
    thread_bool_t pool_close;//正在关闭线程池
    _thread_t * pthreads;//线程句柄数组
    thread_bool_t block_full;//默认TRUE 队列满时,阻塞等待. FALSE 失败退出

    _thread_mutex_t lock;//消息读写锁
    _thread_cond_t cv_not_empty;//队列消息非空
    _thread_cond_t cv_not_full;//队列消息未满
    _thread_cond_t cv_destroy;//注销通知
    _thread_cond_t cv_create_thread;//等待线程创建完成
    struct __JOB * queue_frist;//队列中第一个消息
    struct __JOB * queue_last;//队列中最后一个消息

    //优先队列中的最后一个消息位置,具有优先属性的消息,将从queue_frist开始插入
    struct __JOB * queue_priority_last;

    JOB * job_mempool;//工作内存池,避免重复申请内存
    struct __JOB * cache_frist;//缓存链第一个

    MINHEAP * time_heap;//timer小根堆

#define MAX_TIMER_COUNT   16
    POOLTIMER thread_timer[MAX_TIMER_COUNT];//最多定义N个timer
    int timer_count;//定时器数量
    queue_hook_cb stable_cb;//固定的回调,保留
};

static void empty(void * p, int size)
{
    while (size--)
    {
        ((char*)p)[size] = '\0';
    }
}

static time_t get_time()
{
    time_t ti = 0;
#ifdef _WIN32

    QueryPerformanceCounter((LARGE_INTEGER*)&ti);
    ti = ti / 1000;

#else

    struct timeval tival = { 0 };
    gettimeofday(&tival, NULL);

    ti += tival.tv_sec * 1000;
    ti += tival.tv_usec / 1000;
#endif

    return ti;
}


/*
下面是timer
*/

#define SET_ELEM_INDEX(pElem, Value)  ((pElem)->pdwIndex ? *(pElem)->pdwIndex = Value : 0)
#define UPDATE_ELEM_INDEX(lpElemArray, Index)  ((lpElemArray)[Index].pdwIndex ? *(lpElemArray)[Index].pdwIndex = (Index) : 0)

static MINHEAP * min_heap_new_(thread_ulong_t dwIncrement)
{
    if (0 == dwIncrement) return NULL;
    MINHEAP * pMinHeap = (MINHEAP *)threadpool_malloc(sizeof(MINHEAP));
    empty(pMinHeap, sizeof(MINHEAP));
    pMinHeap->dwIncrement = dwIncrement;

    pMinHeap->lpArrayElement = (HEAPELEMENT *)threadpool_malloc(sizeof(HEAPELEMENT) * dwIncrement);

    if (NULL == pMinHeap->lpArrayElement)
    {
        threadpool_free(pMinHeap);
        pMinHeap = NULL;
    }
    return pMinHeap;
}

static void min_heap_free_(MINHEAP * pMinHeap)
{
    if (pMinHeap->lpArrayElement)
    {
        threadpool_free(pMinHeap->lpArrayElement);
    }
    threadpool_free(pMinHeap);
}

static __inline thread_ulong_t min_heap_get_count_(MINHEAP * pMinHeap)
{
    return pMinHeap->dwCount;
}

static void min_heap_clear_(MINHEAP * pMinHeap)
{
    if (pMinHeap->dwCount > pMinHeap->dwIncrement)
    {
        //需要缩小
        //由于是缩小, 这里是一定成功的
        pMinHeap->lpArrayElement = (HEAPELEMENT*)threadpool_realooc(pMinHeap->lpArrayElement, pMinHeap->dwIncrement * sizeof(HEAPELEMENT));
        assert(pMinHeap->lpArrayElement && "min_heap_clear 发生不可预料的情况!");
    }

    pMinHeap->dwCount = 0;
    empty(pMinHeap->lpArrayElement, pMinHeap->dwIncrement * sizeof(HEAPELEMENT));
}

static thread_bool_t min_heap_top_(MINHEAP * pMinHeap, HEAPELEMENT * pElem)
{
    if (NULL == pMinHeap || NULL == pElem)
    {
        assert(0 && "min_heap_top 传入无效的指针, 请检查!");
        return FALSE;
    }

    if (0 == pMinHeap->dwCount) return FALSE;

    *pElem = pMinHeap->lpArrayElement[0];

    return TRUE;
}

static thread_bool_t min_heap_push_(MINHEAP * pMinHeap, const HEAPELEMENT * pElem)
{
    thread_ulong_t dwBytes = 0;
    HEAPELEMENT * lpArray = NULL;
    thread_ulong_t nIndex = 0;
    thread_ulong_t j = 0;
    if (NULL == pMinHeap || NULL == pElem)
    {
        assert(0 && "min_heap_push 传入无效的指针, 请检查!");
        return FALSE;
    }

    if (pElem->pdwIndex && -1 != *pElem->pdwIndex)
    {
        assert(0 && "min_heap_push 元素可能已经存在于堆中, 请检查!");
        return FALSE;
    }

    if (pMinHeap->dwCount > 0 && 0 == pMinHeap->dwCount % pMinHeap->dwIncrement)
    {
        //内存刚好够用, 现在还要插入一个, 那就需要重新分配空间
        dwBytes = sizeof(HEAPELEMENT) * (pMinHeap->dwCount + pMinHeap->dwIncrement);
        if (NULL == (lpArray = (HEAPELEMENT*)threadpool_realooc(pMinHeap->lpArrayElement, dwBytes)))
        {
            return FALSE;
        }

        pMinHeap->lpArrayElement = lpArray;
        empty(pMinHeap->lpArrayElement + pMinHeap->dwCount, sizeof(HEAPELEMENT) * pMinHeap->dwIncrement);
    }

    //内存问题已处理好, 下面开始正式插入

    lpArray = pMinHeap->lpArrayElement;
    nIndex = pMinHeap->dwCount;//nIndex指向待调整元素的位置，即其数组下标，初始指向新元素所在的堆尾位置

    lpArray[pMinHeap->dwCount++] = *pElem;//向堆尾添加新元素, 并增加数组长度

    while (0 != nIndex)
    {
        j = (nIndex - 1) / 2; //j指向下标为nIndex的元素的双亲
        if (pElem->dwValue >= lpArray[j].dwValue) //若新元素大于待调整元素的双亲，则比较调整结束，退出循环
            break;

        lpArray[nIndex] = lpArray[j]; //将双亲元素下移到待调整元素的位置

        UPDATE_ELEM_INDEX(lpArray, nIndex);
        nIndex = j; //使待调整位置变为其双亲位置，进行下一次循环
    }
    lpArray[nIndex] = *pElem;//把新元素调整到最终位置
    UPDATE_ELEM_INDEX(lpArray, nIndex);

    return TRUE;
}

static thread_bool_t min_heap_erase_(MINHEAP * pMinHeap, thread_ulong_t uIndex)
{
    thread_ulong_t dwBytes = 0;
    HEAPELEMENT * lpArray = NULL;
    //unsigned long uIndex = 0;//用nIndex指向待调整元素的位置，初始指向堆顶位置
    unsigned long j = 0;//j是默认指向左边, 如果左边比右边大,那么指向右边
    HEAPELEMENT Element;
    thread_bool_t bReAlloc = FALSE;

    if (NULL == pMinHeap)
    {
        assert(0 && "min_heap_erase 传入无效的指针, 请检查!");
        return FALSE;
    }

    if (uIndex > pMinHeap->dwCount) return FALSE;

    lpArray = pMinHeap->lpArrayElement;
    SET_ELEM_INDEX(&lpArray[uIndex], -1);//已经被移除

    if (0 == --pMinHeap->dwCount) return TRUE;

    Element = lpArray[pMinHeap->dwCount]; //将待调整的原堆尾元素暂存temp中，以便放入最终位置
    j = 2 * uIndex + 1;//用j指向nIndex的左孩子位置，初始指向下标为1的位置

    while (pMinHeap->dwCount - 1 >= j)//寻找待调整元素的最终位置，每次使孩子元素上移一层，调整到孩子为空时止
    {
        //若存在右孩子且较小，使j指向右孩子
        if (pMinHeap->dwCount - 1 > j && lpArray[j].dwValue > lpArray[j + 1].dwValue)//左比右大
            j++;//指向右边, 指向小的位置

        if (lpArray[j].dwValue >= Element.dwValue) //若temp比其较小的孩子还小，则调整结束，退出循环
            break;

        lpArray[uIndex] = lpArray[j];//否则，将孩子元素移到双亲位置
        UPDATE_ELEM_INDEX(lpArray, uIndex);
        uIndex = j; //将待调整位置变为其较小的孩子位置
        j = 2 * uIndex + 1;//将j变为新的待调整位置的左孩子位置，继续下一次循环
    }

    lpArray[uIndex] = Element;
    UPDATE_ELEM_INDEX(lpArray, uIndex);

    if (pMinHeap->dwCount >= pMinHeap->dwIncrement && 0 == pMinHeap->dwCount % pMinHeap->dwIncrement)
    {
        //此时内存刚好多出一个, 现在还移除一个, 那就重新分配空间, 将空间压缩到最小
        dwBytes = sizeof(HEAPELEMENT) * (pMinHeap->dwCount);

        //因为是缩小空间, 所以realloc是不会失败的
        pMinHeap->lpArrayElement = (HEAPELEMENT*)threadpool_realooc(pMinHeap->lpArrayElement, dwBytes);

        assert(lpArray == pMinHeap->lpArrayElement && "min_heap_erase 发生不可预料的情况!");
    }

    return TRUE;
}

static thread_bool_t min_heap_pop_(MINHEAP * pMinHeap, HEAPELEMENT * pElem)
{
    if (TRUE == min_heap_top_(pMinHeap, pElem))
    {
        return min_heap_erase_(pMinHeap, 0);
    }

    return FALSE;
}

static int min_heap_popbat_(MINHEAP * pMinHeap, thread_ulong_t dwCutValue, HEAPELEMENT * pElem, int nCount)
{
    int nIndex = 0;
    if (NULL == pMinHeap || NULL == pElem)
    {
        assert(0 && "min_heap_popbat 传入无效的指针, 请检查!");
        return -1;
    }

    if (0 == pMinHeap->dwCount) return 0;

    //将符合触发条件的event都取出来
    for (nIndex = 0; nCount > nIndex; nIndex++)
    {
        if (pMinHeap->dwCount > 0 && dwCutValue >= pMinHeap->lpArrayElement[0].dwValue)
        {
            min_heap_pop_(pMinHeap, &pElem[nIndex]);
        }
        else
        {
            break;
        }
    }

    return nIndex;
}



/*
上面是timer的相关算法, 下面是线程池的执行流程
*/


static int threadpool_reset_timer_nolock(THREADPOOL pool, POOLTIMER * timer)
{
    int ret;
    HEAPELEMENT element = { 0 };

    if (TRUE == pool->pool_close)
    {
        return -1;
    }

    if (0 == timer->ref)
    {
        //定时器已经释放
        return 0;
    }

    if (-1 != timer->index)
    {
        //在回调线程释放timer后，其他线程重用了这个timer，就会发生这种情况
        return 0;
    }

    element.dwValue += timer->timeout + get_time();
    element.pObject = timer;
    element.pdwIndex = &timer->index;

    ret = min_heap_push_(pool->time_heap, &element);
    timer->threadid = 0;

    if (0 == timer->index)
    {
        threadpool_active(pool);
    }

    return ret;
}

static int threadpool_get_timer_nolock(THREADPOOL pool, HEAPELEMENT * timer_array, time_t ti_cache, thread_ulong_t threadid)
{
    int count = 0;
    POOLTIMER * timer = NULL;
    int i = 0;
    empty(timer_array, sizeof(HEAPELEMENT) * MAX_TIMER_COUNT);
    count = min_heap_popbat_(pool->time_heap, ti_cache, timer_array, MAX_TIMER_COUNT);

    //设置当前调用线程
    for (i = 0; count > i; i++)
    {
        timer = timer_array[i].pObject;
        timer->threadid = threadid;
    }

    return count;
}


static JOB * threadpool_alloc_job_nolock(THREADPOOL pool)
{
    JOB * job = NULL;

    assert(pool->cache_frist && "thread_pool 缓存队列不可能为空！");

    job = pool->cache_frist;
    pool->cache_frist = pool->cache_frist->next;

    return job;
}

static void threadpool_free_job_nolock(THREADPOOL pool, JOB * job)
{
    job->next = pool->cache_frist;
    pool->cache_frist = job;
}

static thread_ulong_t threadpool_wait_cond(THREADPOOL pool, _thread_cond_t * cond_t, thread_ulong_t tvwait)
{
    thread_ulong_t error = 0;
#ifdef _WIN32

    if (FALSE == SleepConditionVariableCS(cond_t, &(pool)->lock, (tvwait)))
    {
        error = GetLastError();
    }

#else

    struct timespec abstime = { 0 };
    abstime.tv_nsec = ((long)((tvwait) % 1000) * 1000000);
    abstime.tv_sec = ((time_t)(time(NULL) + (tvwait) / 1000));
    error =  pthread_cond_timedwait(cond_t, &pool->lock, &abstime);

#endif
    return error;
}

#ifdef _WIN32 
static unsigned int TPSTDCALL threadpool_worker(void * arg)
#else
static void * TPSTDCALL threadpool_worker(void * arg)
#endif
{
    THREADPOOL pool = (THREADPOOL)arg;

    JOB * job = NULL;
    POOLTIMER * timer = NULL;
    HEAPELEMENT * timer_array = (HEAPELEMENT*)threadpool_malloc(sizeof(HEAPELEMENT) * MAX_TIMER_COUNT);
    assert(timer_array && "threadpool_function 致命的内存分配错误!");

    int timer_count = 0;//触发的timer个数

    HEAPELEMENT element = { 0 };
    time_t ti_cache = 0;
    thread_ulong_t timeout = FOREVER;
    thread_ulong_t last_error = 0;
    int handle_count = 0;//任务累计

    thread_ulong_t threadid = get_current_thread_id();
    thread_bool_t is_permanent_thread = TRUE;//是否常驻线程

    threadpool_lock(pool);
    if (pool->thread_num)
    {
        is_permanent_thread = pool->thread_num > pool->thread_cur_num ? TRUE : FALSE;
        threadpool_wake_one(pool->cv_create_thread);
    }
    threadpool_unlock(pool);

    while (pool->thread_work)
    {
    BEGIN:
        timeout = FOREVER;
        timer_count = 0;

        threadpool_lock(pool);
        //通过iotime.key.data是否为NULL, 来得知上一个定时器是否被处理
        //被处理的话就提取一个新的

        if (pool->timer_count && handle_count > pool->thread_cur_num && pool->queue_cur_num)
        {
            //定时器优先级很低，当任务很多的时候我们每处理大于线程数或以上的任务才行检查一次定时器
            if (TRUE == min_heap_top_(pool->time_heap, &element))
            {
                ti_cache = get_time();
                if (ti_cache >= element.dwValue)//时间到
                {
                    if (timer_count = threadpool_get_timer_nolock(pool, timer_array, ti_cache, threadid))
                    {
                        goto FINISH;
                    }
                }
            }
        }

        while (0 == pool->queue_cur_num)
        {
            if (FALSE == is_permanent_thread || FALSE == pool->thread_work)
            {
                pool->thread_cur_num--;
                threadpool_unlock(pool);
                goto BYEBYE;
            }
            else if (TRUE == min_heap_top_(pool->time_heap, &element))
            {
                ti_cache = get_time();
                timeout = (thread_ulong_t)max(0, element.dwValue - ti_cache);

                if (0 == timeout)//时间到
                {
                    if (timer_count = threadpool_get_timer_nolock(pool, timer_array, ti_cache, threadid))
                    {
                        goto FINISH;
                    }
                }
            }

            if (last_error = threadpool_wait_cond(pool, &pool->cv_not_empty, timeout))
            {
                if (ERROR_TIMEOUT == last_error)
                {
                    ti_cache = get_time();
                    if (timer_count = threadpool_get_timer_nolock(pool, timer_array, ti_cache, threadid))
                    {
                        goto FINISH;
                    }
                }
                threadpool_unlock(pool);
                fprintf(stderr, "threadpool_function unkown last error = %d ERROR_TIMEOUT = %d\n", last_error, ERROR_TIMEOUT);
                goto BEGIN;
            }
        }

        job = pool->queue_frist;

        if (pool->queue_frist == pool->queue_priority_last)
        {
            //最后一个优先任务被取出
            pool->queue_priority_last = NULL;
        }

        pool->queue_frist = job->next;
        pool->queue_cur_num--;

        if (0 == pool->queue_cur_num)
        {
            pool->queue_frist = NULL;
            pool->queue_last = NULL;

            //任务都执行完了， 再退出
            if (TRUE == pool->pool_close)
            {
                pool->thread_work = FALSE;
                threadpool_wake_one(pool->cv_not_empty);
            }
        }
        else if ((pool->queue_cur_num + 1) == pool->queue_max_num)
        {
            threadpool_wake_one(pool->cv_not_empty);
            threadpool_wake_all(pool->cv_not_full);
        }
        else/*not full and not empty*/
        {
            threadpool_wake_one(pool->cv_not_empty);//通知另外一条线程
        }

    FINISH:
        threadpool_unlock(pool);

        if (timer_count)
        {
            //触发的是timer
            while (timer_count--)//反过来执行
            {
                timer = timer_array[timer_count].pObject;

                //第一个参数要用arg，能兼容更多情况，比如this指针
                timer->callback(timer->arg, timer->event);

                //通过threadidId 或者 key 来判断对象是否已删除,这里用threadid判断最简单
                //如果timer被删除或重置为新的定时器, threadid可以是NULL, 也可以是其他线程, 
                //但绝不会是本线程, 因为本线程正在运行

                if (timer->threadid == threadid)//即使脏读也能判断正确
                {
                    threadpool_wake_one(timer->cv_kill);

                    threadpool_lock(pool);
                    threadpool_reset_timer_nolock(pool, timer);
                    threadpool_unlock(pool);
                }

                handle_count = 0;//重置
            }
        }
        else
        {
            //触发的是任务
            handle_count++;//任务累计
            job->callback(job->obj, job->arg);
            threadpool_lock(pool);
            threadpool_free_job_nolock(pool, job);
            threadpool_unlock(pool);
        }
    }/*while(pool->thread_work)*/

BYEBYE:
    threadpool_free(timer_array);
    if (0 == pool->thread_cur_num)//此处不需要互斥
    {
        //这是最后一个线程
        threadpool_wake_one(pool->cv_destroy);//通知关闭
    }
    else
    {
        //还有线程未关闭
        threadpool_wake_one(pool->cv_not_empty);
    }

    return 0;
}


/*
下面是提供给外部程序的接口
*/

THREADPOOL threadpool_create(unsigned int thread_num, unsigned int max_thread_num, unsigned int queue_max_num)
{
    THREADPOOL pool = NULL;
    int i = 0;
    if (thread_num > (((unsigned int)~((unsigned int)0)) >> 1)
        || max_thread_num > (((unsigned int)~((unsigned int)0)) >> 1)
        || queue_max_num > (((unsigned int)~((unsigned int)0)) >> 1)) return NULL;

    pool = (THREADPOOL)threadpool_malloc(sizeof(struct __THREADPOOL));
    if (NULL == pool) return NULL;
    empty(pool, sizeof(struct __THREADPOOL));

    pool->thread_num = thread_num;
    pool->thread_max_num = (0 == thread_num ? 0 : max(thread_num, max_thread_num));
    pool->thread_cur_num = 0;
    pool->queue_cur_num = 0;
    pool->queue_max_num = queue_max_num;
    pool->queue_frist = NULL;
    pool->queue_last = NULL;
    pool->thread_work = TRUE;
    pool->pool_close = FALSE;
    pool->block_full = TRUE;

    for (i = 0; MAX_TIMER_COUNT > i; i++)
    {
#ifdef _WIN32
        InitializeConditionVariable(&pool->thread_timer[i].cv_kill);
#else
        pthread_cond_init(&pool->thread_timer[i].cv_kill, NULL);
#endif
    }

    pool->time_heap = min_heap_new_(MAX_TIMER_COUNT + 1);//没啥，就是预留一个空位而已

    if (NULL == pool->time_heap)
    {
        threadpool_free(pool);
        return NULL;
    }

    pool->job_mempool = (JOB*)threadpool_malloc(sizeof(JOB) * (pool->queue_max_num + pool->thread_max_num + 1));

    if (NULL == pool->job_mempool)
    {
        min_heap_free_(pool->time_heap);
        threadpool_free(pool);
        return NULL;
    }

    //加 1 是为了兼容queue方法, 因为queue的时候pool没有记录线程数, 
    //但是确实queue运行在某个线程, 并占用了一个内存空间
    empty(pool->job_mempool, sizeof(JOB) * (pool->queue_max_num + pool->thread_max_num + 1));

    int queue_count = pool->queue_max_num + pool->thread_max_num;
    pool->cache_frist = pool->job_mempool;
    for (i = 0; queue_count > i; i++)
    {
        pool->job_mempool[i].next = &pool->job_mempool[i + 1];
    }

#ifdef _WIN32

    InitializeCriticalSection(&pool->lock);//临界区
    InitializeConditionVariable(&pool->cv_not_empty);
    InitializeConditionVariable(&pool->cv_not_full);
    InitializeConditionVariable(&pool->cv_destroy);
    InitializeConditionVariable(&pool->cv_create_thread);
    if (thread_num)
    {
        pool->pthreads = (_thread_t*)threadpool_malloc(sizeof(_thread_t) * pool->thread_num);

        for (i = 0; pool->thread_num > i; i++)
        {
            threadpool_lock(pool);
            //_beginthread会自动关闭句柄， ex不会

            pool->pthreads[i] = (_thread_t)_beginthreadex(NULL, 0, threadpool_worker, (LPVOID)pool, 0, 0);

            if (INVALID_HANDLE_VALUE != pool->pthreads[i])
            {
                pool->thread_cur_num++;
                threadpool_wait_cond(pool, &pool->cv_create_thread, FOREVER);
            }

            threadpool_unlock(pool);
        }
    }

#else
    pthread_mutex_init(&pool->lock, NULL);
    pthread_cond_init(&pool->cv_not_empty, NULL);
    pthread_cond_init(&pool->cv_not_full, NULL);
    pthread_cond_init(&pool->cv_destroy, NULL);
    pthread_cond_init(&pool->cv_create_thread, NULL);
    if (thread_num)
    {
        pool->pthreads = (_thread_t*)threadpool_malloc(sizeof(_thread_t) * pool->thread_num);

        for (i = 0; pool->thread_num > i; i++)
        {
            threadpool_lock(pool);
            if (0 == pthread_create(&pool->pthreads[i], NULL, threadpool_worker, (void *)pool))
            {
                pool->thread_cur_num++;
                threadpool_wait_cond(pool, &pool->cv_create_thread, FOREVER);
            }

            threadpool_unlock(pool);
        }
    }

#endif

    //用自旋锁等待线程创建完成, 其实很快的
    return pool;
}

thread_bool_t threadpool_destroy(THREADPOOL pool)
{
    threadpool_lock(pool);

    if (TRUE == pool->pool_close)//防止多次调用threadpool_destroy函数
    {
        threadpool_unlock(pool);
        return FALSE;
    }

    //if (pool->time_heap) time_heap_clear(pool->time_heap);
    pool->pool_close = TRUE;

    while (pool->thread_cur_num)
    {
        threadpool_wait_cond(pool, &pool->cv_destroy, FOREVER);
    }

    threadpool_unlock(pool);

#ifdef _WIN32
    if (pool->thread_num)
    {
        //WaitForMultipleObjects(pool->thread_num, pool->pthreads, TRUE, FROEVER);
        for (int i = 0; pool->thread_num > i; i++)
        {
            CloseHandle(pool->pthreads[i]);
        }
    }

    DeleteCriticalSection(&pool->lock);
#else

    pthread_mutex_destroy(&pool->lock);
    pthread_cond_destroy(&pool->cv_not_empty);
    pthread_cond_destroy(&pool->cv_not_full);
    pthread_cond_destroy(&pool->cv_destroy);

#endif

    if (pool->pthreads) threadpool_free(pool->pthreads);
    if (pool->time_heap) min_heap_free_(pool->time_heap);
    if (pool->job_mempool) threadpool_free(pool->job_mempool);
    if (pool) threadpool_free(pool);

    return TRUE;
}

int threadpool_add_job(THREADPOOL pool, thread_bool_t priority, queue_hook_cb callback, void * obj, void *arg)
{
    int ret = 0;
    JOB * newjob = NULL;
    _thread_t tid = 0;

    threadpool_lock(pool);

    if (FALSE == pool->block_full && pool->queue_cur_num == pool->queue_max_num)
    {
        goto END;
    }

    while (FALSE == pool->pool_close && pool->queue_cur_num == pool->queue_max_num)
    {
        //fprintf(stderr, "threadpool_add_job 任务数量达到峰顶[%d], 进入等待模式", pool->queue_max_num);
        threadpool_wait_cond(pool, &pool->cv_not_full, FOREVER);
        //fprintf(stderr, "threadpool_add_job 等待被唤醒, 当前任务数[%d]", pool->queue_cur_num);
    }

    if (TRUE == pool->pool_close)
    {
        ret = -1;
        goto END;
    }

    newjob = threadpool_alloc_job_nolock(pool);
    newjob->priority = priority;
    newjob->callback = callback;
    newjob->obj = obj;
    newjob->arg = arg;
    newjob->next = NULL;

    if (pool->queue_cur_num == 0)
    {
        pool->queue_frist = newjob;
        pool->queue_last = newjob;
        if (TRUE == priority)//优先处理
        {
            pool->queue_priority_last = pool->queue_frist;
        }
        threadpool_wake_one(pool->cv_not_empty);
    }
    else
    {
        if (TRUE == priority)//优先处理，前置任务
        {
            if (NULL == pool->queue_priority_last)
            {
                newjob->next = pool->queue_frist;
                pool->queue_frist = newjob;
            }
            else
            {
                //pool->queue_priority_last->next 指向的任务是没有优先级的
                newjob->next = pool->queue_priority_last->next;//插到next前面
                pool->queue_priority_last->next = newjob;//插到next前面
            }

            if (pool->queue_last == pool->queue_priority_last)
            {
                //目前所有的任务都具有优先级,
                //也就是说pool->queue_last也指向了优先任务,所以需要修改他的值
                pool->queue_last = newjob;
            }

            pool->queue_priority_last = newjob;//last指向新的任务
        }
        else
        {
            pool->queue_last->next = newjob;
            pool->queue_last = newjob;
        }

    }

    ret = ++pool->queue_cur_num;

    if (pool->queue_cur_num == pool->queue_max_num && pool->thread_max_num > pool->thread_cur_num)
    {

        //fprintf(stderr, "threadpool_add_job 任务数量已满[%d], 开启一个新线程", pool->queue_cur_num);
#ifdef _WIN32

        tid = (_thread_t)_beginthreadex(NULL, 0, threadpool_worker, (LPVOID)pool, 0, 0);

        if (INVALID_HANDLE_VALUE == tid)
        {
            pool->thread_cur_num++;
            CloseHandle(tid);
            threadpool_wait_cond(pool, &pool->cv_destroy, FOREVER);
        }

#else

        if (0 == pthread_create(&tid, NULL, threadpool_worker, (void *)pool))
        {
            pool->thread_cur_num++;
            threadpool_wait_cond(pool, &pool->cv_destroy, FOREVER);
        }
#endif
    }

END:
    threadpool_unlock(pool);
    return ret;
}

thread_bool_t threadpool_block_full(THREADPOOL pool, thread_bool_t block)
{
    //允许脏读,不加锁
    pool->block_full = block;
    return TRUE;
}

thread_bool_t threadpool_active(THREADPOOL pool)
{
    if (pool)
    {
        //如果没有任务，在外部设置定时器后，将无法进行第一次唤醒，所以激活一下，使定时器生效
        //其实一般不太建议使用该函数，当然你也可以在loop前设置定时器，这样就不需要该函数了
        threadpool_wake_one(pool->cv_not_empty);
        return TRUE;
    }
    return FALSE;
}

THREADTIMER threadpool_set_timer(THREADPOOL pool, time_t timeout, timer_hook_cb callback, void * arg, long event)
{
    POOLTIMER * timer = NULL;
    _thread_cond_t cv_kill = { 0 };
    int tid = 0;
    int i = 0;
    threadpool_lock(pool);

    //因为线程池是不会频繁地增删定时器的,所以这里用for循环,不需要太高效的算法
    if (FALSE == pool->pool_close && sizeof(pool->thread_timer) > pool->timer_count)
    {
        for (i = 0; sizeof(pool->thread_timer) > i; i++)
        {
            if (0 == pool->thread_timer[i].ref)
            {
                timer = &pool->thread_timer[i];
                pool->timer_count++;
                tid = i + 1;
                break;
            }
        }
    }

    if (timer)
    {
        timer->timeout = timeout;
        timer->callback = callback;
        timer->event = event;
        timer->arg = arg;
        timer->ref = 1;
        timer->index = -1;
        threadpool_reset_timer_nolock(pool, timer);
    }

    threadpool_unlock(pool);

    return (THREADTIMER)tid;
}

thread_bool_t threadpool_kill_timer(THREADPOOL pool, THREADTIMER tid)
{
    int ret = FALSE;
    _thread_cond_t cv_kill;
    POOLTIMER * timer = NULL;

    if (tid > MAX_TIMER_COUNT) return FALSE;

    threadpool_lock(pool);

    timer = &pool->thread_timer[tid - 1];
    if (timer->ref)
    {
        cv_kill = timer->cv_kill;
        if (timer->threadid && timer->threadid != get_current_thread_id())
        {
            //timer回调线程中删除定时器,那么用户应该知道如何保证线程安全
            //直接删除就可以了, 如果非当前线程, 那么要等timer回调结束,
            //才能删除定时器
            threadpool_wait_cond(pool, &timer->cv_kill, FOREVER);
        }

        min_heap_erase_(pool->time_heap, timer->index);

        empty(timer, sizeof(POOLTIMER));
        timer->cv_kill = cv_kill;//还原一下,这个不能清除

        pool->timer_count--;

        ret = TRUE;
    }

    threadpool_unlock(pool);

    return ret;
}


/*
拓展的队列接口
*/
HQUEUE queue_create(unsigned int max_message)
{
    return (HQUEUE)threadpool_create(0, 0, max_message);
}

unsigned int queue_dispatch(HQUEUE queue)
{
    ((THREADPOOL)queue)->thread_work = TRUE;
    unsigned int ret = (unsigned int)threadpool_worker((void*)queue);
    return ret;
}

int queue_post(HQUEUE queue, queue_hook_cb callback, void * obj, void *arg)
{
    return threadpool_add_job((THREADPOOL)queue, FALSE, callback, obj, arg);
}

thread_bool_t queue_break(HQUEUE queue)
{
    ((THREADPOOL)queue)->thread_work = FALSE;
    return TRUE;
}

thread_bool_t queue_destroy(HQUEUE queue)
{
    return threadpool_destroy((THREADPOOL)queue);
}

HQTIMER queue_set_timer(HQUEUE queue, time_t timeout, timer_hook_cb callback, void * arg, long event)
{
    return (HQTIMER)threadpool_set_timer((THREADPOOL)queue, timeout, callback, arg, event);
}

thread_bool_t queue_kill_timer(HQUEUE queue, HQTIMER timer)
{
    return threadpool_kill_timer((THREADPOOL)queue, (THREADTIMER)timer);
}