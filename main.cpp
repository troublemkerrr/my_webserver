#include <iostream>
#include <libgen.h> // 包含basename()
#include <stdlib.h> // 包含atoi()
#include <sys/types.h> // 定义套接字描述符
#include <sys/socket.h>
#include <arpa/inet.h> // 字节序转换
#include <signal.h> // 信号相关
#include <assert.h> // 断言

#include "./threadpool/threadpool.h"
#include "./http/http_conn.h"

#define MAX_FD 1024 //最大文件描述符
#define MAX_EVENT_NUMBER 1000 // 最大事件数

extern void addfd(int epollfd,int fd,bool one_shot,bool et_mode); // 外部声明
/*
    int sigaction(int signum, const struct sigaction *act,
                     struct sigaction *oldact);
    - signum：需要捕捉处理的信号量

    struct sigaction {
        void     (*sa_handler)(int); // 自定义信号处理函数，或者特殊值SIG_IGN（忽略）、SIG_DFL（默认操作）
        void     (*sa_sigaction)(int, siginfo_t *, void *); // 一般不用
        sigset_t   sa_mask; // 信号处理函数执行期间需要阻塞的其他信号
        int        sa_flags; // 标志，包括 SA_RESTART（在信号处理函数返回后自动重新启动系统调用）和 SA_SIGINFO（使用 sa_sigaction 而不是 sa_handler）
        void     (*sa_restorer)(void); // 通常用于系统在信号处理函数执行后恢复程序状态
    };
*/
// 默认信号处理函数
// void sig_handler(int sig){

// }

// 捕捉信号并处理
void sig_ctl(int sig,void(sig_handler)(int),bool restart=true){
    struct sigaction sig_act; // 必须加struct，因为结构体和函数同名
    sig_act.sa_handler=sig_handler;
    if(restart){
        sig_act.sa_flags=SA_RESTART;
    }
    sigfillset(&sig_act.sa_mask); // 填充信号集，将所有信号添加到该集合中
    assert(sigaction(sig,&sig_act,NULL)!=-1); // 如果条件为加，断言触发，程序会终止，并在标准错误流中输出相关信息
}

int main(int argc,char *argv[]){
    sig_ctl(SIGTERM, SIG_DFL);

    if(argc==1){
        // basename()用于从路径中获取文件名部分
        std::cerr << "usage: " << basename(argv[0]) << " ip_address port_number" << std::endl;
        return 1;
    }
    int port=atoi(argv[1]); // 字符串转整数

    // 创建线程池
    threadpool<http_conn> *pool;
    try{
        pool=new threadpool<http_conn>(); // 线程池对象在整个程序的生命周期内都存在，创建在堆上
    }catch(const std::exception& e){ //?????????????????
        // 捕获并处理异常
        std::cerr << "Caught exception: " << e.what() << std::endl;
        return 1;
    }
    
    http_conn * conns=new http_conn[MAX_FD];

/*
    int socket(int domain, int type, int protocol); 
    - domain: 协议族
        AF_INET : ipv4
        AF_INET6 : ipv6
        AF_UNIX, AF_LOCAL : 本地套接字通信（进程间通信）
    - type: 通信过程中使用的协议类型
        SOCK_STREAM : 流式协议
        SOCK_DGRAM : 报式协议
    - protocol : 具体的一个协议。一般写0，对应TCP/UDP
    - 返回值：
        - 成功：返回文件描述符，操作的就是内核缓冲区。
        - 失败：-1
*/
    // 1.创建监听套接字
    int listenfd=socket(AF_INET,SOCK_STREAM,0); 
    if(listenfd==-1){
        perror("socket");
        return 1;
    }

/*
    int setsockopt(int sockfd, int level, int optname,
                      const void *optval, socklen_t optlen);
    -level：选项所属的协议层。通常为 SOL_SOCKET，表示套接字选项
    -optname：要设置的选项的名称。例如，SO_REUSEADDR 表示地址复用选项
    -optval：一个指向存储选项值的缓冲区的指针
    -optlen：optval 缓冲区的大小，即选项值的长度
    返回 0 表示成功，返回 -1 表示失败，并设置相应的错误码（通过全局变量 errno 获取）                  
*/
    // 设置端口复用，允许多个套接字绑定到相同的IP地址和端口号
    int reuse = 1;
    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == -1) {
        perror("set socket option");
        return 1;
    }
/*
    int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen); 
    - sockfd : 通过socket函数创建得到的文件描述符
    - addr : 需要绑定的socket地址，这个地址封装了ip和端口号的信息
    - addrlen : 第二个参数结构体占的内存大小

    #include <netinet/in.h>
    struct sockaddr_in
    {
        sa_family_t sin_family; // 协议族
        in_port_t sin_port; // 端口
        struct in_addr sin_addr; // ip地址
        unsigned char sin_zero[sizeof (struct sockaddr) - __SOCKADDR_COMMON_SIZE -
        sizeof (in_port_t) - sizeof (struct in_addr)];
    };

    struct in_addr {
        in_addr_t s_addr; // 32 位的 IPv4 地址
    };

    用时强转为sockaddr
*/    
    // 2.将fd和本地的IP+端口进行绑定
    sockaddr_in server_addr;
    server_addr.sin_family=AF_INET;
    server_addr.sin_port=htons(port); // 字节序转换 host->network s
    server_addr.sin_addr.s_addr=htonl(INADDR_ANY); // 将监听所有可用的网络接口上的传入连接
    int ret =bind(listenfd,(sockaddr *)&server_addr,sizeof(server_addr)); // 先取地址再类型转换
    if(ret==-1){
        perror("bind");
        return -1;
    }

/*
    int listen(int sockfd, int backlog); 
    - sockfd : 通过socket()函数得到的文件描述符
    - backlog : 未连接的和已经连接的和的最大值
*/
    // 3.开始监听
    ret = listen(listenfd,8); 
    if(ret==-1){
        perror("listen");
        return -1;
    }

    // 4.创建一个epoll实例
    int epollfd= epoll_create(1); // 参数无意义，大于0即可
    http_conn::m_epollfd=epollfd;

    // 5.将监听套接字的文件描述符添加到epoll实例中
    epoll_event ev;
    ev.data.fd=listenfd;
    ev.events=EPOLLIN; 
    epoll_ctl(epollfd,EPOLL_CTL_ADD,listenfd,&ev);
    
/*
    int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout);
    - epfd : epoll实例对应的文件描述符
    - events : 传出参数，保存了发送了变化的文件描述符的信息，是一个指向数组的指针
    - maxevents : 第二个参数结构体数组的大小，告诉内核在一次调用中最多返回多少个触发的事件
    - timeout : 阻塞时间
        0 : 不阻塞
        -1 : 阻塞，直到检测到fd数据发生变化，解除阻塞
        > 0 : 阻塞的时长（毫秒）
    - 返回值：
        成功，返回发送变化的文件描述符的个数 > 0
        失败 -1  
    
    struct epoll_event {
        uint32_t     events;   
        epoll_data_t data;      
    }; 

    typedef union epoll_data { // 联合体
        void    *ptr;
        int      fd;
        uint32_t u32;
        uint64_t u64;
    } epoll_data_t;
*/
    epoll_event changed_events[MAX_EVENT_NUMBER];
    // 6.委托内核监听多个文件描述符
    while(1){
        int num=epoll_wait(epollfd,changed_events,MAX_EVENT_NUMBER,-1); // 阻塞
        if(num == -1){
            perror("epoll wait");
            return -1;
        }
        for(int i=0;i<num;i++){ // 变化的文件描述符的信息存储在数组中
            ev=changed_events[i];
            if(ev.data.fd==listenfd){ // 有新的客户端连接
/*
    int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen); 阻塞函数
    - sockfd : 用于监听的文件描述符
    - addr : 传出参数，记录了连接成功后客户端的地址信息（ip，port）
    - addrlen : 指定第二个参数的对应的内存大小
    - 返回值：
        成功 ：用于通信的文件描述符
        -1 ： 失败
*/
                sockaddr_in client_addr;
                socklen_t len=sizeof(client_addr);
                int connfd=accept(listenfd,(sockaddr *)&client_addr,&len); 
                if(connfd==-1){
                    perror("accept");
                    return -1;
                }
                if(http_conn::m_conn_count>=MAX_FD){ // ????????????
                    continue;
                }
                conns[connfd].init(connfd,client_addr); // 初始化连接
                addfd(epollfd,connfd,true,true); // 监听这个连接
            }else if(ev.events & EPOLLIN){ // 客户端发来请求
                if(conns[ev.data.fd].read()){ // 将请求读入缓冲区
                    pool->append(conns+ev.data.fd); // 让子线程处理请求
                }else{
                    conns[ev.data.fd].close_conn();
                } 
            }else if(ev.events & EPOLLOUT){
                conns[ev.data.fd].write();
            }
        }
    }

    close(epollfd);
    close(listenfd);
    delete[] conns;
    delete pool;


    return 0;
}