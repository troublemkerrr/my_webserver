#ifndef LOCKER_H
#define LOCKER_H

#include <pthread.h> // 线程 锁 条件变量
#include <semaphore.h> // 信号量相关

/* 
    RAII思想：RAII的核心思想是将资源的生命周期与对象的生命周期绑定在一起。
    当对象被创建时，它的构造函数会自动获取资源，当对象被销毁时，它的析构函数会自动释放资源。
    这样可以确保资源在对象的作用域结束时被正确释放，无论是因为正常的控制流程退出还是因为异常的抛出。

    面向过程调用很麻烦，每次要将对象都传给函数，而封装后一些变成面向对象就可以直接对象.方法
*/

// 互斥锁
class locker{
    public:
        // int pthread_mutex_init(pthread_mutex_t *restrict mutex, const pthread_mutexattr_t *restrict attr);
        // restrict 告诉编译器，一个指针是唯一一个访问特定内存区域的途径，不会通过其他别名指针进行访问
        // attr: 初始化互斥锁时要使用的属性
        locker(){
            if(pthread_mutex_init(&m_mutex,NULL)!=0){
                throw std::exception();
            }
        }

        ~locker(){
            pthread_mutex_destroy(&m_mutex);
        }

        bool lock(){
            return pthread_mutex_lock(&m_mutex)==0; // 如果失败返回错误号
        }

        bool unlock(){
            return pthread_mutex_unlock(&m_mutex)==0;
        }

    private:
        pthread_mutex_t m_mutex;
};

// 信号量
class sem{
    public:
        sem(){
            // int sem_init(sem_t *sem, int pshared, unsigned int value);
            // pshared:0-线程同步 非0-进程同步
            if(sem_init(&m_sem,0,0)!=0){
                throw std::exception();
            }
        } 

        sem(int value){
            if(sem_init(&m_sem,0,value)!=0){
                throw std::exception();
            }
        }

        // P 操作（等待操作），减少信号量的计数（或占用一个资源）
        // 如果信号量计数为正，则减少计数并继续执行；如果计数为零，那么当前线程会被阻塞，直到信号量计数变为正
        bool wait(){
            return sem_wait(&m_sem)==0;
        }

        // V 操作（释放操作），增加信号量的计数
        bool post(){
            return sem_post(&m_sem)==0;
        }

        ~sem(){
            sem_destroy(&m_sem);
        }

    private:
        sem_t m_sem;
};

#endif