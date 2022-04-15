#ifndef THREADPOOL_H
#define THREADPOOL_H


#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"

template <typename T>
class threadpool
{
public:
    threadpool(connection_pool* connPool, int thread_number = 8, int max_request = 10000);
    ~threadpool();
    bool append(T *request);

private:
    // 工作线程运行的函数， 他不断的从工作队列中取出任务并执行
    static void* worker(void *arg);
    void run();

// 线程池的属性
private:
    int m_thread_number;        // 线程池中的线程数
    int m_max_requests;         // 请求队列中允许的最大请求数
    pthread_t* m_threads;       // 描述线程池的数组， 其大小为m_thread_number
    std::list<T *> m_workqueue; // 请求队列
    locker m_queuelocker;       // 保护请求队列的互斥锁
    sem m_queuestat;            // 是否有任务需要处理
    bool m_stop;                // 是否结束进程
    connection_pool *m_connPool;  //数据库
};

template <typename T>
threadpool<T>::threadpool(connection_pool* connPool, int thread_number, int max_request)
    :m_thread_number(thread_number), m_max_requests(max_request), m_stop(false),
    m_threads(NULL), m_connPool(connPool)
{
    if(thread_number <= 0 || max_request <= 0)
        throw std::exception();
    m_threads = new pthread_t[m_thread_number];

    for(int i = 0; i < thread_number; ++i)
    {     // 创建线程池数组中的每一个线程，创建完之后， 立即执行回调函数                               
        if(pthread_create(m_threads + i, NULL, worker, this) != 0) 
        {
            delete[] m_threads;
            throw std::exception();
        }
        // 将线程设置为线程分离后， 不用单独对工作线程进行回收
        if(pthread_detach(m_threads[i])){
            delete[] m_threads;
            throw std::exception();
        }
    }
}

template <typename T>
threadpool<T>::~threadpool()
{
    delete[] m_threads;
    m_stop = true;
}

// 将获得数据添加到请求队列中
template <typename T>
bool threadpool<T>::append(T* request)
{   
    // 在处理工作队列的时候都要先加锁， 再解锁
    m_queuelocker.lock();
    if(m_workqueue.size() > m_max_requests){
        m_queuelocker.unlock();
        return false;
    }
    // 添加到任务队列中
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    // 信号量提醒有任务要处理
    m_queuestat.post();
    return true;
}

template <typename T>
void* threadpool<T>::worker(void* arg)
{
    threadpool *pool = (threadpool *)arg;
    pool->run();
    return pool;
}

template <typename T>
void threadpool<T>::run()
{
    while(!m_stop)
    {   
        // 在创建线程池的时候,所有信号量都阻塞在任务队列的条件变量上
        m_queuestat.wait();
        // 使用任务队列前先加锁
        m_queuelocker.lock();
        if(m_workqueue.empty())
        {
            m_queuelocker.unlock();
            continue;
        }
        // 取出任务队列中的第一个
        T* request = m_workqueue.front();
        // 将任务队列中的第一个删除
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if(!request)
            continue;
        // 从连接池中取出一个数据库
        ConnectionRaII mysqlcon(&request->mysql, m_connPool);

        // 处理请求
        request->process();
    }
}



#endif