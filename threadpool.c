
#include "threadpool.h"

#include <assert.h>

#define FOREVER  -1

#define threadpool_malloc(l) malloc(l)
#define threadpool_realooc(p, l) realloc(p, l)
#define threadpool_free(p) free(p)
typedef long long       _time_t;

#define threadpool_wait_cond(pool, cv, tival) threadpool_wait_cond_((pool), &(cv), (tival))

#ifdef _WIN32
#include <process.h>
#include <Windows.h>
#define EWAITTIMEOUT  ERROR_TIMEOUT

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
#include <sys/time.h>
#define max(a,b)    (((a) > (b)) ? (a) : (b))
#define EWAITTIMEOUT  ETIMEDOUT

typedef pthread_t                   _thread_t;
typedef pthread_cond_t       _thread_cond_t;
typedef pthread_mutex_t         _thread_mutex_t;

#define threadpool_lock(pool) pthread_mutex_lock(&(pool)->lock)
#define threadpool_unlock(pool) pthread_mutex_unlock(&(pool)->lock)
#define threadpool_wake_one(cv) pthread_cond_signal(&(cv))
#define threadpool_wake_all(cv) pthread_cond_broadcast(&(cv))


#define get_current_thread_id  pthread_self

#endif


typedef struct __HEAPELEMENT
{
    _time_t tiValue;
    thread_ulong_t * pdwIndex;
    void * pObject;
}HEAPELEMENT;

typedef struct __MINHEAP
{
    thread_ulong_t dwIncrement;//�ռ䲻��ʱ,ÿ�ε������ٿռ�
    thread_ulong_t dwCount;//Ԫ������
    HEAPELEMENT * lpArrayElement;
}MINHEAP;

typedef struct __JOB
{
    thread_bool_t priority;//�Ƿ����ȴ���
    queue_hook_cb callback;//�ص�
    void * obj;//����һ��������ΪC++ thisָ��
    void * arg;//��������������ΪC++ class��Ա��������
    thread_ulong_t index;//�ڴ��������
    struct __JOB * next;
}JOB, *PJOB;

typedef struct __POOLTIMER
{
    thread_ulong_t index;//С���ѵ�λ��
    thread_ulong_t ref;//���ü���

    thread_ulong_t threadid;

    //��Ҫһ���¼�(HEVENT)����������������֤���ݵ��̰߳�ȫ,��Ϊ��timerִ��ʱ,��������
    //�����ʱ�����ͷŶ�ʱ���Ļ�,�ͻ��������,���Ե���ʱ����ʼʱ,������HEVENT
    //Ϊ���ź�, ��ʱ��ִ����Ϻ�,����Ϊ���ź�
    //void * signal;
    _thread_cond_t cv_kill;
    _time_t timeout;//����
    timer_hook_cb callback;
    long event;
    void * arg;
}POOLTIMER;

struct __THREADPOOL{
    int thread_num;//��פ�߳���
    int thread_max_num;//����߳���
    int thread_cur_num;//��ǰ�߳���
    int queue_cur_num;//��ǰ������Ϣ��
    int queue_max_num;//��������Ϣ��
    thread_bool_t pool_close;//���ڹر��̳߳�
    _thread_t * pthreads;//�߳̾������
    thread_bool_t block_full;//Ĭ��TRUE ������ʱ,�����ȴ�. FALSE ʧ���˳�

    _thread_mutex_t lock;//��Ϣ��д��
    _thread_cond_t cv_not_empty;//������Ϣ�ǿ�
    _thread_cond_t cv_not_full;//������Ϣδ��
    _thread_cond_t cv_destroy;//ע��֪ͨ
    _thread_cond_t cv_create_thread;//�ȴ��̴߳������
    struct __JOB * queue_frist;//�����е�һ����Ϣ
    struct __JOB * queue_last;//���������һ����Ϣ

    //���ȶ����е����һ����Ϣλ��,�����������Ե���Ϣ,����queue_frist��ʼ����
    struct __JOB * queue_priority_last;

    JOB * job_mempool;//�����ڴ��,�����ظ������ڴ�
    struct __JOB * cache_frist;//��������һ��

    MINHEAP * time_heap;//timerС����

#define MAX_TIMER_COUNT   16
#define TIMER_ARRAY_COUNT  (MAX_TIMER_COUNT/2)
    POOLTIMER thread_timer[MAX_TIMER_COUNT];//��ඨ��N��timer
    int timer_count;//��ʱ������
    queue_hook_cb stable_cb;//�̶��Ļص�,����
};

static void empty(void * p, int size)
{
    while (size--)
    {
        ((char*)p)[size] = '\0';
    }
}

static _time_t get_time()
{
    _time_t ti = 0;
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
������timer
*/

#define MHELEM_SET_INDEX(pElem, Value)  ((pElem)->pdwIndex ? *(pElem)->pdwIndex = Value : 0)
#define MHELEM_UPDATE_INDEX(lpElemArray, Index)  ((lpElemArray)[Index].pdwIndex ? *(lpElemArray)[Index].pdwIndex = (Index) : 0)

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
        //��Ҫ��С
        //��������С, ������һ���ɹ���
        pMinHeap->lpArrayElement = (HEAPELEMENT*)threadpool_realooc(pMinHeap->lpArrayElement, pMinHeap->dwIncrement * sizeof(HEAPELEMENT));
        assert(pMinHeap->lpArrayElement && "min_heap_clear ��������Ԥ�ϵ����!");
    }

    pMinHeap->dwCount = 0;
    empty(pMinHeap->lpArrayElement, pMinHeap->dwIncrement * sizeof(HEAPELEMENT));
}

static thread_bool_t min_heap_top_(MINHEAP * pMinHeap, HEAPELEMENT * pElem)
{
    if (NULL == pMinHeap || NULL == pElem)
    {
        assert(0 && "min_heap_top ������Ч��ָ��, ����!");
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
        assert(0 && "min_heap_push ������Ч��ָ��, ����!");
        return FALSE;
    }

    if (pElem->pdwIndex && -1 != *pElem->pdwIndex)
    {
        assert(0 && "min_heap_push Ԫ�ؿ����Ѿ������ڶ���, ����!");
        return FALSE;
    }

    if (pMinHeap->dwCount > 0 && 0 == pMinHeap->dwCount % pMinHeap->dwIncrement)
    {
        //�ڴ�պù���, ���ڻ�Ҫ����һ��, �Ǿ���Ҫ���·���ռ�
        dwBytes = sizeof(HEAPELEMENT) * (pMinHeap->dwCount + pMinHeap->dwIncrement);
        if (NULL == (lpArray = (HEAPELEMENT*)threadpool_realooc(pMinHeap->lpArrayElement, dwBytes)))
        {
            return FALSE;
        }

        pMinHeap->lpArrayElement = lpArray;
        empty(pMinHeap->lpArrayElement + pMinHeap->dwCount, sizeof(HEAPELEMENT) * pMinHeap->dwIncrement);
    }

    //�ڴ������Ѵ����, ���濪ʼ��ʽ����

    lpArray = pMinHeap->lpArrayElement;
    nIndex = pMinHeap->dwCount;//nIndexָ�������Ԫ�ص�λ�ã����������±꣬��ʼָ����Ԫ�����ڵĶ�βλ��

    lpArray[pMinHeap->dwCount++] = *pElem;//���β�����Ԫ��, ���������鳤��

    while (0 != nIndex)
    {
        j = (nIndex - 1) / 2; //jָ���±�ΪnIndex��Ԫ�ص�˫��
        if (pElem->tiValue >= lpArray[j].tiValue) //����Ԫ�ش��ڴ�����Ԫ�ص�˫�ף���Ƚϵ����������˳�ѭ��
            break;

        lpArray[nIndex] = lpArray[j]; //��˫��Ԫ�����Ƶ�������Ԫ�ص�λ��

        MHELEM_UPDATE_INDEX(lpArray, nIndex);
        nIndex = j; //ʹ������λ�ñ�Ϊ��˫��λ�ã�������һ��ѭ��
    }
    lpArray[nIndex] = *pElem;//����Ԫ�ص���������λ��
    MHELEM_UPDATE_INDEX(lpArray, nIndex);

    return TRUE;
}

static thread_bool_t min_heap_erase_(MINHEAP * pMinHeap, thread_ulong_t uIndex)
{
    thread_ulong_t dwBytes = 0;
    HEAPELEMENT * lpArray = NULL;
    //unsigned long uIndex = 0;//��nIndexָ�������Ԫ�ص�λ�ã���ʼָ��Ѷ�λ��
    unsigned long j = 0;//j��Ĭ��ָ�����, �����߱��ұߴ�,��ôָ���ұ�
    HEAPELEMENT Element;
    thread_bool_t bReAlloc = FALSE;

    if (NULL == pMinHeap)
    {
        assert(0 && "min_heap_erase ������Ч��ָ��, ����!");
        return FALSE;
    }

    if (uIndex > pMinHeap->dwCount) return FALSE;

    lpArray = pMinHeap->lpArrayElement;
    MHELEM_SET_INDEX(&lpArray[uIndex], -1);//�Ѿ����Ƴ�

    if (0 == --pMinHeap->dwCount) return TRUE;

    Element = lpArray[pMinHeap->dwCount]; //����������ԭ��βԪ���ݴ�temp�У��Ա��������λ��
    j = 2 * uIndex + 1;//��jָ��nIndex������λ�ã���ʼָ���±�Ϊ1��λ��

    while (pMinHeap->dwCount - 1 >= j)//Ѱ�Ҵ�����Ԫ�ص�����λ�ã�ÿ��ʹ����Ԫ������һ�㣬����������Ϊ��ʱֹ
    {
        //�������Һ����ҽ�С��ʹjָ���Һ���
        if (pMinHeap->dwCount - 1 > j && lpArray[j].tiValue > lpArray[j + 1].tiValue)//����Ҵ�
            j++;//ָ���ұ�, ָ��С��λ��

        if (lpArray[j].tiValue >= Element.tiValue) //��temp�����С�ĺ��ӻ�С��������������˳�ѭ��
            break;

        lpArray[uIndex] = lpArray[j];//���򣬽�����Ԫ���Ƶ�˫��λ��
        MHELEM_UPDATE_INDEX(lpArray, uIndex);
        uIndex = j; //��������λ�ñ�Ϊ���С�ĺ���λ��
        j = 2 * uIndex + 1;//��j��Ϊ�µĴ�����λ�õ�����λ�ã�������һ��ѭ��
    }

    lpArray[uIndex] = Element;
    MHELEM_UPDATE_INDEX(lpArray, uIndex);

    if (pMinHeap->dwCount >= pMinHeap->dwIncrement && 0 == pMinHeap->dwCount % pMinHeap->dwIncrement)
    {
        //��ʱ�ڴ�պö��һ��, ���ڻ��Ƴ�һ��, �Ǿ����·���ռ�, ���ռ�ѹ������С
        dwBytes = sizeof(HEAPELEMENT) * (pMinHeap->dwCount);

        //��Ϊ����С�ռ�, ����realloc�ǲ���ʧ�ܵ�
        pMinHeap->lpArrayElement = (HEAPELEMENT*)threadpool_realooc(pMinHeap->lpArrayElement, dwBytes);

        assert(lpArray == pMinHeap->lpArrayElement && "min_heap_erase ��������Ԥ�ϵ����!");
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

static int min_heap_popbat_(MINHEAP * pMinHeap, _time_t tiCutValue, HEAPELEMENT * pElem, int nCount)
{
    int nIndex = 0;
    if (NULL == pMinHeap || NULL == pElem)
    {
        assert(0 && "min_heap_popbat ������Ч��ָ��, ����!");
        return -1;
    }

    if (0 == pMinHeap->dwCount) return 0;

    //�����ϴ���������event��ȡ����
    for (nIndex = 0; nCount > nIndex; nIndex++)
    {
        if (pMinHeap->dwCount > 0 && tiCutValue >= pMinHeap->lpArrayElement[0].tiValue)
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
������timer������㷨, �������̳߳ص�ִ������
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
        //��ʱ���Ѿ��ͷ�
        return 0;
    }

    if (-1 != timer->index)
    {
        //�ڻص��߳��ͷ�timer�������߳����������timer���ͻᷢ���������
        return 0;
    }

    element.tiValue = timer->timeout + get_time();
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

static int threadpool_get_timer_nolock(THREADPOOL pool, HEAPELEMENT * timer_array, _time_t ti_cache, thread_ulong_t threadid)
{
    int count = 0;
    POOLTIMER * timer = NULL;
    int i = 0;
    empty(timer_array, sizeof(HEAPELEMENT) * TIMER_ARRAY_COUNT);
    count = min_heap_popbat_(pool->time_heap, ti_cache, timer_array, TIMER_ARRAY_COUNT);

    //���õ�ǰ�����߳�
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

    assert(pool->cache_frist && "thread_pool ������в�����Ϊ�գ�");

    job = pool->cache_frist;
    pool->cache_frist = pool->cache_frist->next;

    return job;
}

static void threadpool_free_job_nolock(THREADPOOL pool, JOB * job)
{
    job->next = pool->cache_frist;
    pool->cache_frist = job;
}

static thread_ulong_t threadpool_wait_cond_(THREADPOOL pool, _thread_cond_t * cond_t, thread_ulong_t tvwait)
{
    thread_ulong_t error = 0;

    if (0 == tvwait) return EWAITTIMEOUT;
#ifdef _WIN32

    if (FALSE == SleepConditionVariableCS(cond_t, &(pool)->lock, (tvwait)))
    {
        error = GetLastError();
    }

#else

    struct timespec abstime = { 0 };
    struct timeval tival = { 0 };

    if (FOREVER == tvwait)
    {
        error = pthread_cond_wait(cond_t, &pool->lock);
    }
    else
    {
        gettimeofday(&tival, NULL);
        abstime.tv_sec = tival.tv_sec + (tvwait) / 1000;
        if(0 == abstime.tv_sec)
        {
            //pthread_cond_timedwait������Ҫ����һ��, ���򷵻�EINVAL
            abstime.tv_sec = 1;
            abstime.tv_nsec = 0;
        }
        else
        {
            abstime.tv_nsec = (tival.tv_usec + (tvwait) % 1000) * 1000000;
        }
        
        error = pthread_cond_timedwait(cond_t, &pool->lock, &abstime);
    }

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
    HEAPELEMENT * timer_array = (HEAPELEMENT*)threadpool_malloc(sizeof(HEAPELEMENT) * TIMER_ARRAY_COUNT);
    assert(timer_array && "threadpool_worker �������ڴ�������!");

    int timer_count = 0;//������timer����

    HEAPELEMENT element = { 0 };
    _time_t ti_cache = 0;
    thread_ulong_t timeout = FOREVER;
    thread_ulong_t last_error = 0;
    int handle_count = 0;//�����ۼ�

    thread_ulong_t threadid = get_current_thread_id();
    thread_bool_t is_permanent_thread = TRUE;//�Ƿ�פ�߳�

    threadpool_lock(pool);
    if (pool->thread_num)
    {
        is_permanent_thread = pool->thread_num >= pool->thread_cur_num ? TRUE : FALSE;
        threadpool_wake_one(pool->cv_create_thread);
    }
    threadpool_unlock(pool);

    while (TRUE)
    {
    BEGIN:
        timeout = FOREVER;
        timer_count = 0;

        threadpool_lock(pool);
        //ͨ��iotime.key.data�Ƿ�ΪNULL, ����֪��һ����ʱ���Ƿ񱻴���
        //������Ļ�����ȡһ���µ�

        if (pool->timer_count && handle_count > pool->thread_cur_num 
            && pool->queue_cur_num && FALSE == pool->pool_close)
        {
            //��ʱ�����ȼ��ܵͣ�������ܶ��ʱ������ÿ��������߳��������ϵ�������м��һ�ζ�ʱ��
            if (TRUE == min_heap_top_(pool->time_heap, &element))
            {
                ti_cache = get_time();
                if (ti_cache >= element.tiValue)//ʱ�䵽
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
            if (FALSE == is_permanent_thread || TRUE == pool->pool_close)
            {
                pool->thread_cur_num--;
                threadpool_unlock(pool);
                goto BYEBYE;
            }

            if (TRUE == min_heap_top_(pool->time_heap, &element))
            {
                ti_cache = get_time();
                timeout = (thread_ulong_t)(element.tiValue > ti_cache ? element.tiValue - ti_cache : 0);
            }

            if (last_error = threadpool_wait_cond(pool, pool->cv_not_empty, timeout))
            {
                if (EWAITTIMEOUT == last_error)
                {
                    ti_cache = get_time();
                    if (timer_count = threadpool_get_timer_nolock(pool, timer_array, ti_cache, threadid))
                    {
                        goto FINISH;
                    }
                }
                threadpool_unlock(pool);

                goto BEGIN;
            }
        }

        job = pool->queue_frist;

        if (pool->queue_frist == pool->queue_priority_last)
        {
            //���һ����������ȡ��
            pool->queue_priority_last = NULL;
        }

        pool->queue_frist = job->next;
        pool->queue_cur_num--;

        if (0 == pool->queue_cur_num)
        {
            pool->queue_frist = NULL;
            pool->queue_last = NULL;

            //����ִ�����ˣ� ���˳�
            if (TRUE == pool->pool_close)
            {
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
            threadpool_wake_one(pool->cv_not_empty);//֪ͨ����һ���߳�
        }

    FINISH:
        threadpool_unlock(pool);

        if (timer_count)
        {
            //��������timer
            while (timer_count--)//������ִ��
            {
                timer = timer_array[timer_count].pObject;

                //��һ������Ҫ��arg���ܼ��ݸ������������thisָ��
                timer->callback(timer->arg, timer->event);

                //ͨ��threadidId ���� key ���ж϶����Ƿ���ɾ��,������threadid�ж����
                //���timer��ɾ��������Ϊ�µĶ�ʱ��, threadid������NULL, Ҳ�����������߳�, 
                //���������Ǳ��߳�, ��Ϊ���߳���������

                if (timer->threadid == threadid)//��ʹ���Ҳ���ж���ȷ
                {
                    threadpool_wake_one(timer->cv_kill);

                    threadpool_lock(pool);
                    threadpool_reset_timer_nolock(pool, timer);
                    threadpool_unlock(pool);
                }

                handle_count = 0;//����
            }
        }
        else
        {
            //������������
            handle_count++;//�����ۼ�
            job->callback(job->obj, job->arg);
            threadpool_lock(pool);
            threadpool_free_job_nolock(pool, job);
            threadpool_unlock(pool);
        }
    }/*while(TRUE)*/

BYEBYE:
    threadpool_free(timer_array);
    if (0 == pool->thread_cur_num)//�˴�����Ҫ����
    {
        //�������һ���߳�
        threadpool_wake_one(pool->cv_destroy);//֪ͨ�ر�
    }
    else
    {
        //�����߳�δ�ر�
        threadpool_wake_one(pool->cv_not_empty);
    }

    return 0;
}


/*
�������ṩ���ⲿ����Ľӿ�
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

    pool->time_heap = min_heap_new_(MAX_TIMER_COUNT + 1);//ûɶ������Ԥ��һ����λ����

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

    //�� 1 ��Ϊ�˼���queue����, ��Ϊqueue��ʱ��poolû�м�¼�߳���, 
    //����ȷʵqueue������ĳ���߳�, ��ռ����һ���ڴ�ռ�
    empty(pool->job_mempool, sizeof(JOB) * (pool->queue_max_num + pool->thread_max_num + 1));

    int queue_count = pool->queue_max_num + pool->thread_max_num;
    pool->cache_frist = pool->job_mempool;
    for (i = 0; queue_count > i; i++)
    {
        pool->job_mempool[i].index = i;
        pool->job_mempool[i].next = &pool->job_mempool[i + 1];
    }

#ifdef _WIN32

    InitializeCriticalSection(&pool->lock);//�ٽ���
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
            //_beginthread���Զ��رվ���� ex����

            pool->pthreads[i] = (_thread_t)_beginthreadex(NULL, 0, threadpool_worker, (LPVOID)pool, 0, 0);

            if (INVALID_HANDLE_VALUE == pool->pthreads[i])
            {
                goto FAILED;
            }

            pool->thread_cur_num++;
            threadpool_wait_cond(pool, pool->cv_create_thread, FOREVER);
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
                threadpool_wait_cond(pool, pool->cv_create_thread, FOREVER);
            }
            else
            {
                goto FAILED;
            }

            threadpool_unlock(pool);
        }
    }

#endif

    //���������ȴ��̴߳������, ��ʵ�ܿ��
    return pool;

FAILED:
    threadpool_destroy(pool);
    return NULL;
}

thread_bool_t threadpool_destroy(THREADPOOL pool)
{
    threadpool_lock(pool);

    if (TRUE == pool->pool_close)//��ֹ��ε���threadpool_destroy����
    {
        threadpool_unlock(pool);
        return FALSE;
    }

    pool->pool_close = TRUE;

    threadpool_wake_one(pool->cv_not_empty);

    while (pool->thread_cur_num)
    {
        threadpool_wait_cond(pool, pool->cv_destroy, FOREVER);
    }

    threadpool_unlock(pool);

#ifdef _WIN32
    if (pool->thread_num)
    {
        //WaitForMultipleObjects(pool->thread_num, pool->pthreads, TRUE, FROEVER);
        for (int i = 0; pool->pthreads && pool->thread_num > i; i++)
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
        threadpool_wait_cond(pool, pool->cv_not_full, FOREVER);
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
        if (TRUE == priority)//���ȴ���
        {
            pool->queue_priority_last = pool->queue_frist;
        }
        threadpool_wake_one(pool->cv_not_empty);
    }
    else
    {
        if (TRUE == priority)//���ȴ���ǰ������
        {
            if (NULL == pool->queue_priority_last)
            {
                newjob->next = pool->queue_frist;
                pool->queue_frist = newjob;
            }
            else
            {
                //pool->queue_priority_last->next ָ���������û�����ȼ���
                newjob->next = pool->queue_priority_last->next;//�嵽nextǰ��
                pool->queue_priority_last->next = newjob;//�嵽nextǰ��
            }

            if (pool->queue_last == pool->queue_priority_last)
            {
                //Ŀǰ���е����񶼾������ȼ�,
                //Ҳ����˵pool->queue_lastҲָ������������,������Ҫ�޸�����ֵ
                pool->queue_last = newjob;
            }

            pool->queue_priority_last = newjob;//lastָ���µ�����
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

        //fprintf(stderr, "threadpool_add_job ������������[%d], ����һ�����߳�", pool->queue_cur_num);
#ifdef _WIN32

        tid = (_thread_t)_beginthreadex(NULL, 0, threadpool_worker, (LPVOID)pool, 0, 0);

        if (INVALID_HANDLE_VALUE == tid)
        {
            pool->thread_cur_num++;
            CloseHandle(tid);
            threadpool_wait_cond(pool, pool->cv_create_thread, FOREVER);
        }

#else

        if (0 == pthread_create(&tid, NULL, threadpool_worker, (void *)pool))
        {
            pool->thread_cur_num++;
            threadpool_wait_cond(pool, pool->cv_create_thread, FOREVER);
        }
#endif
    }

END:
    threadpool_unlock(pool);
    return ret;
}

thread_bool_t threadpool_block_full(THREADPOOL pool, thread_bool_t block)
{
    //�������,������
    pool->block_full = block;
    return TRUE;
}

thread_bool_t threadpool_active(THREADPOOL pool)
{
    if (pool)
    {
        //���û���������ⲿ���ö�ʱ���󣬽��޷����е�һ�λ��ѣ����Լ���һ�£�ʹ��ʱ����Ч
        //��ʵһ�㲻̫����ʹ�øú�������Ȼ��Ҳ������loopǰ���ö�ʱ���������Ͳ���Ҫ�ú�����
        threadpool_wake_one(pool->cv_not_empty);
        return TRUE;
    }
    return FALSE;
}

THREADTIMER threadpool_set_timer(THREADPOOL pool, thread_ulong_t timeout, timer_hook_cb callback, void * arg, long event)
{
    POOLTIMER * timer = NULL;
    _thread_cond_t cv_kill = { 0 };
    int tid = 0;
    int i = 0;

    if (FOREVER == timeout)
    {
        assert(0 && "threadpool_set_timer ��ֹ�����ò���ʱ�Ķ�ʱ��");
        return tid;
    }

    threadpool_lock(pool);

    //��Ϊ�̳߳��ǲ���Ƶ������ɾ��ʱ����,����������forѭ��,����Ҫ̫��Ч���㷨
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
            //timer�ص��߳���ɾ����ʱ��,��ô�û�Ӧ��֪����α�֤�̰߳�ȫ
            //ֱ��ɾ���Ϳ�����, ����ǵ�ǰ�߳�, ��ôҪ��timer�ص�����,
            //����ɾ����ʱ��
            threadpool_wait_cond(pool, timer->cv_kill, FOREVER);
        }

        min_heap_erase_(pool->time_heap, timer->index);

        empty(timer, sizeof(POOLTIMER));
        timer->cv_kill = cv_kill;//��ԭһ��,����������

        pool->timer_count--;

        ret = TRUE;
    }

    threadpool_unlock(pool);

    return ret;
}


/*
��չ�Ķ��нӿ�
*/
HQUEUE queue_create(unsigned int max_message)
{
    return (HQUEUE)threadpool_create(0, 0, max_message);
}

unsigned int queue_dispatch(HQUEUE queue)
{
    unsigned int ret = (unsigned int)threadpool_worker((void*)queue);
    return ret;
}

int queue_post(HQUEUE queue, queue_hook_cb callback, void * obj, void *arg)
{
    return threadpool_add_job((THREADPOOL)queue, FALSE, callback, obj, arg);
}

thread_bool_t queue_break(HQUEUE queue)
{
    ((THREADPOOL)queue)->pool_close = TRUE;
    return TRUE;
}

thread_bool_t queue_destroy(HQUEUE queue)
{
    return threadpool_destroy((THREADPOOL)queue);
}

HQTIMER queue_set_timer(HQUEUE queue, thread_ulong_t timeout, timer_hook_cb callback, void * arg, long event)
{
    return (HQTIMER)threadpool_set_timer((THREADPOOL)queue, timeout, callback, arg, event);
}

thread_bool_t queue_kill_timer(HQUEUE queue, HQTIMER timer)
{
    return threadpool_kill_timer((THREADPOOL)queue, (THREADTIMER)timer);
}