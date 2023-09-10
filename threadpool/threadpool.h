#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <pthread.h>
#include <queue>
#include "../lock/locker.h"

template <typename T> // T是请求队列中的任务类型
class threadpool{
    public:
        threadpool(int pool_size=8,int queue_max_size=100000);
        ~threadpool();
        bool append(T *request); // 往请求队列中添加任务

    private:
        pthread_t *m_threads; // 描述线程池的数组，pthread_t是线程标识符
        int m_pool_size; // 线程池大小，线程池中的线程数

        std::queue<T *> m_request_queue; // 请求队列
        int m_queue_max_size; // 请求队列大小，允许的最大请求数

        locker m_lock; // 对请求队列加互斥锁
        sem m_queue_stat; // 请求队列有无任务需要处理

        bool m_stop; // 是否结束线程池工作

        /*
            work()是线程所执行的函数，但实际工作在run()中处理

            必须是静态函数，静态成员函数没有this指针。线程调用的时候，
            限制了只能有一个参数void* arg，如果不设置成静态在调用的时候会出现this和arg都给work()导致错误
        */
        static void *work(void *arg);
        void run();
};

template <typename T>
threadpool<T>::threadpool(int pool_size,int queue_max_size):m_pool_size(pool_size),m_queue_max_size(queue_max_size){
    if(pool_size<=0 || queue_max_size<=0){ 
        throw std::exception();
    }
    
    // 开辟描述线程池的数组空间
    m_threads = new pthread_t[m_pool_size];
    if(!m_threads){
        throw std::exception();
    }
    
    // 创建线程并设置线程分离
    for(int i=0;i<m_pool_size;i++){
        // int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start_routine) (void *), void *arg);
        // pthread_t是线程标识符，attr表示新线程的属性，start_routine是一个指向函数的指针，arg表示传递给start_routine函数的参数
        if(pthread_create(m_threads+i,NULL,work,this)!=0){ // 要写!=0
            delete[] m_threads;
            throw std::exception(); 
        }
        if(pthread_detach(m_threads[i])!=0){
            delete[] m_threads;
            throw std::exception(); 
        }
    }

    m_stop=false;
}

template <typename T>
threadpool<T>::~threadpool(){
    delete[] m_threads;
    m_stop=true; // 标记线程可以结束
}

// 往请求队列中添加任务
template <typename T>
bool threadpool<T>::append(T *request){
    m_lock.lock();
    if(m_request_queue.size()>=m_queue_max_size){ // 不能超过最大请求数
        m_lock.unlock();
        return false;
    }
    m_request_queue.push(request);
    m_lock.unlock();
    m_queue_stat.post(); // 使信号量计数+1
    return true;
}

template <typename T>
void *threadpool<T>::work(void *arg){
    threadpool *self_pool=(threadpool *)arg; // 需要类型转换
    self_pool->run();
    return NULL;
}

// 每个线程实际所做的工作：在请求队列中取出任务并处理。单独写一个run()函数避免频繁写self_pool->...
template <typename T>
void threadpool<T>::run(){
    while(!m_stop){ // m_stop为false就循环执行下面代码
        m_queue_stat.wait();
        m_lock.lock();
        T *request=m_request_queue.front(); // 从请求队列中取出一个任务
        m_request_queue.pop();
        m_lock.unlock();
        request->process(); // 处理任务
    }
}

#endif