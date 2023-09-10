#include <sys/epoll.h> // epoll
#include <unistd.h> // 创建新进程/管道，获取进程id，exec()系列，读写...
#include <fcntl.h> // 与文件相关的操作
#include <iostream>
#include <cstring> // 包含memset()
#include <netinet/in.h> // 包含sockaddr_in结构体
#include <regex> //正则表达式
#include <vector>
#include <sys/stat.h> // 获取文件的相关的状态信息stat
#include <sys/mman.h> // 内存映射mmap
#include <stdarg.h> // 处理可变参数
#include <sys/uio.h> // writev() 从多个缓冲区写入

class http_conn{
    public:
        static int m_epollfd; // 监听套接字的文件描述符
        static int m_conn_count; // http连接数

        enum METHOD {GET,POST}; // 请求类型
        enum HTTP_CODE { // ？？？？？？解析请求报文所得的结果 给每个都写个注释吧
            NO_REQUEST, 
            GET_REQUEST, // ???????为什么没有post
            BAD_REQUEST,
            NO_RESOURCE,
            FORBIDDEN_REQUEST,
            FILE_REQUEST,
            INTERNAL_ERROR, 
            CLOSED_CONNECTION //?????
        };
        enum PARSE_STATE
        {
            PARSE_STATE_LINE,
            PARSE_STATE_HEADER,
            PARSE_STATE_BODY
        };
        enum LINE_STATUS{
            LINE_OK,
            LINE_OPEN,
            LINE_BAD
        };

        void init(int sockfd, const sockaddr_in &addr);
        void init();

        bool read(); // 将http请求内容读入缓冲区
        bool write(); // 将http响应内容写入文件描述符

        // http的任务：解析请求报文 整合响应资源
        void process();

        void close_conn(); // 关闭这个http连接

    private:
        int m_sockfd; // 该http连接的socket文件描述符
        sockaddr_in m_address; // 客户端的ip地址+端口号信息
        bool m_et_mode; // 是否设置为边缘触发模式

        PARSE_STATE m_parse_state;

        METHOD m_method; // http请求类型
        std::string m_url; // 请求的url
        std::string m_version; // http版本
        bool m_linger; // 是否保持连接
        int m_body_len; // 请求体长度
        struct stat m_file_info; // 文件的相关的状态信息
        char *m_file_address; // 内存映射后目标文件在内存中的起始地址

        // 限定读写缓冲区的大小
        static const int READ_BUFFER_SIZE=2048;
        static const int WRITE_BUFFER_SIZE=1024;
        
        char m_read_buf[READ_BUFFER_SIZE]; // 读缓冲区
        int m_read_idx; // 读缓冲区中0～m_read_idx-1有读到的数据
        int m_check_idx; // 找下一行时，此时检查的位置
        int m_start_line; // 每次解析报文时，一行的起始位置（一行一行解析
    
        char m_write_buf[WRITE_BUFFER_SIZE];
        int m_write_idx;
/*
    struct iovec {
        void  *iov_base; // 指向数据缓冲区的指针
        size_t iov_len;  // 缓冲区的大小（以字节为单位）
    };
    iovec 结构体允许你构建一个数组，其中每个元素描述了一个不同的数据缓冲区及其大小。
    可以使用readv和writev一次性操作多个不连续的内存区域，而无需将它们合并成单个连续的缓冲区。
*/
        struct iovec m_iov[2]; // 两个缓冲区，分别是写缓冲区和文件的内存映射区（不一定有
        int m_iov_count; // 实际用到几个缓冲区
        int bytes_to_send; // 需要发送多少字节的数据
        int bytes_have_send; // 已经发送多少字节的数据

        HTTP_CODE process_read(); // 利用有限状态机解析整个请求报文，并请求资源

        char *get_line() { return m_read_buf + m_start_line; }; // 得到即将被解析的一行
        LINE_STATUS find_next_line(); // 根据换行符\r\n找到下一行

        // 解析http请求
        HTTP_CODE parse_request_line(char *text); // 解析请求行
        HTTP_CODE parse_request_headers(char *text); // 请求头部
        HTTP_CODE parse_request_body(char *text); // 请求体

        HTTP_CODE do_request(); // 请求资源

        bool process_write(HTTP_CODE ret); // 拼接http响应

        bool format_write(const char *format,...); // 按照传入格式写数据到写缓冲区
        // 响应行
        bool add_response_line(int status, const char *title);
        // 响应头部和空行
        bool add_response_headers(int content_length); 
        bool add_content_type(); // 内容类型
        bool add_content_length(int len); // 内容长度
        bool add_connection(); // 是否保持连接
        bool add_blank_line(); // 空行
        // 响应正文
        bool add_response_body(const char* body);
};