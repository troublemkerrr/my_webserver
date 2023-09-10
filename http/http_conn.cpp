

#include "./http_conn.h"

const std::string doc_root="/home/parallels/Desktop/my_webserver/root"; // 资源路径
// 定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

// 静态成员变量必须在类外部进行定义，并且在类内部进行声明
int http_conn::m_epollfd=-1;
int http_conn::m_conn_count=0;

void http_conn::init(int sockfd, const sockaddr_in &addr){
    m_sockfd=sockfd;
    m_address=addr;
    m_conn_count++;
    m_et_mode=false;
    init();
}

void http_conn::init(){
    m_read_idx=0;
    m_write_idx=0;
    memset(m_read_buf,'\0',READ_BUFFER_SIZE);
    memset(m_write_buf,'\0',WRITE_BUFFER_SIZE);
    m_parse_state=PARSE_STATE_LINE;
    m_body_len=0;
    m_check_idx=0;
    m_start_line=0;
    m_iov_count=0;
    bytes_to_send=0;
    bytes_have_send=0;
}

// 对文件描述符设置非阻塞
void setnonblocking(int fd){
    /* int fcntl(int fd, int cmd, arg... );
        cmd：表示要执行的操作命令，可以是以下之一：
            F_GETFL：获取文件状态标志。
            F_SETFL：设置文件状态标志...*/
    int option=fcntl(fd,F_GETFL);
    option |= O_NONBLOCK;
    fcntl(fd,F_SETFL,option);         
}

// 添加需要监听的文件描述符
void addfd(int epollfd,int fd,bool one_shot,bool et_mode){
    setnonblocking(fd); // 对文件描述符设置非阻塞
/*
    对epoll实例进行管理：添加/删除/修改文件描述符信息
    int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
    - epfd : epoll实例对应的文件描述符
    - op : 要进行什么操作
            EPOLL_CTL_ADD: 添加
            EPOLL_CTL_MOD: 修改
            EPOLL_CTL_DEL: 删除
    - fd : 要检测的文件描述符
    - event : 检测文件描述符什么事情

    struct epoll_event {
        uint32_t     events;      Epoll events 
        epoll_data_t data;        User data variable 
    };

    typedef union epoll_data {
        void        *ptr;
        int          fd;
        uint32_t     u32;
        uint64_t     u64;
    } epoll_data_t;
*/ 
    epoll_event ev;
    ev.data.fd=fd;
    ev.events=EPOLLIN | EPOLLRDHUP; // 检测：数据可读 ｜ 对方关闭连接的写半部分
    if(one_shot){
        /* 设置某个文件描述符的事件为一次性的。
        这意味着一旦这个文件描述符上的事件被触发，它仍被保留在epoll集合中，但需要修改设置才能继续监视。
        通常在多线程环境下使用，用于确保每个线程只处理一次某个文件描述符上的事件，可以有效地避免竞态条件。 */
        ev.events |= EPOLLONESHOT;
    }
    if(et_mode){
        ev.events |= EPOLLET;
    }
    epoll_ctl(epollfd,EPOLL_CTL_ADD,fd,&ev);
}

// 删除在监听的文件描述符，并关闭
void delfd(int epollfd,int fd){
    epoll_ctl(epollfd,EPOLL_CTL_DEL,fd,0);
    close(fd); 
}

// 将事件重置为EPOLLONESHOT，继续监听。flag：EPOLLIN or EPOLLOUT
void modfd(int epollfd,int fd,int flag,bool et_mode){
    epoll_event ev;
    ev.data.fd=fd;
    ev.events=flag | EPOLLRDHUP | EPOLLONESHOT; // 读/写、对方关闭连接的写半部分、设置事件为一次性的
    if(et_mode){
        ev.events |= EPOLLET;
    }
    epoll_ctl(epollfd,EPOLL_CTL_MOD,fd,&ev);
}

// 循环读取客户数据，直到无数据可读或者对方关闭连接
bool http_conn::read() {
    if( m_read_idx >= READ_BUFFER_SIZE ) {
        return false;
    }
    int bytes_read = 0;
    while(1) {
/*
    ssize_t recv(int sockfd, void *buf, size_t len, int flags);
    -flags：用于控制接收操作的行为
        0（默认值）： 默认情况下，recv 函数会阻塞等待，直到有数据可用为止。
        MSG_PEEK： 接收数据的同时不会从输入队列中移除数据，即不会将数据删除。
        MSG_WAITALL： 该标志要求 recv 函数一直等待，直到 len 字节的数据被完全接收或出现错误。如果不使用此标志，在数据不足的情况下，recv 函数可能会返回接收到的部分数据。
        MSG_DONTWAIT： 非阻塞模式，如果没有数据可用，recv 函数会立即返回，而不是等待。
        MSG_TRUNC： 如果接收到的数据长度超过缓冲区长度，截断数据而不报告错误。
        ...
*/
        // 从m_read_buf + m_read_idx索引处开始保存数据，大小是READ_BUFFER_SIZE - m_read_idx
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0 ); // 为什么是0？？？？？？
        if (bytes_read == -1) {
            if( errno == EAGAIN || errno == EWOULDBLOCK ) { // 没有数据了，EWOULDBLOCK是EAGAIN的别名
                break;
            }
            return false;   
        } else if (bytes_read == 0) {   // 对方关闭连接
            return false;
        }
        m_read_idx += bytes_read;
    }
    printf("%s",m_read_buf);
    return true;
}

// 将http响应内容写入文件描述符
bool http_conn::write(){
    if (bytes_to_send == 0){
        modfd(m_epollfd, m_sockfd, EPOLLIN,m_et_mode); // 继续监听有无请求
        init();
        return true;
    }
    int ret;
    while(1){   
/*
    ssize_t writev(int fd, const struct iovec *iov, int iovcnt);
    fd：要写入的文件描述符
    iov：一个指向 iovec 结构体数组的指针，每个 iovec 结构体描述了一个要写入的内存块的位置和大小
    iovcnt：表示 iov 数组中的 iovec 结构体的数量
    返回成功写入的字节数，如果出现错误，则返回 -1 并设置 errno
*/
        // 把缓冲区中的数据写入文件描述符
        ret=writev(m_sockfd,m_iov,m_iov_count);
        if(ret==-1){
            if( errno == EAGAIN || errno == EWOULDBLOCK ){
                modfd(m_epollfd, m_sockfd, EPOLLOUT,m_et_mode); // 继续监听有无要继续发送的数据
                return true;
            }
            munmap(m_file_address, m_file_info.st_size); // 解除内存映射
            return false;
        }

        bytes_have_send+=ret;
        bytes_to_send-=ret;
        if(bytes_to_send==0){ // 全部发送完毕
            munmap(m_file_address, m_file_info.st_size);
            modfd(m_epollfd, m_sockfd, EPOLLIN,m_et_mode); 
            if (m_linger){
                init();
                return true;
            }else{
                return false;
            }
        }

        if (bytes_have_send >= m_iov[0].iov_len){ // m_iov[0]发送完了
            m_iov[0].iov_len = 0;
            m_iov[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iov[1].iov_len = bytes_to_send;
        }else{
            m_iov[0].iov_base = m_write_buf + bytes_have_send;
            m_iov[0].iov_len = m_iov[0].iov_len - bytes_have_send;
        }
        return true;
    }
}

// http的任务：解析请求报文 整合响应资源
void http_conn::process(){
    HTTP_CODE read_ret=process_read();
    if(read_ret==NO_REQUEST){
        modfd(m_epollfd,m_sockfd,EPOLLIN,m_et_mode); // 没接收到请求，继续监听
        return;
    }
    bool write_ret=process_write(read_ret);
    if(!write_ret){
        close_conn(); //?????????????
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_et_mode); // ?????????????????
}

// 利用有限状态机解析整个请求报文，并请求资源
http_conn::HTTP_CODE http_conn::process_read(){
    while(1){
        char *text=get_line();
        if(find_next_line()!=LINE_OK){ // 正常情况下m_check_idx定位到下一行起始位置
            break;
        }
        switch(m_parse_state){
            case PARSE_STATE_LINE:
                if(parse_request_line(text)==BAD_REQUEST){
                    return BAD_REQUEST;
                }
                break;
            case PARSE_STATE_HEADER:
                if(parse_request_headers(text)==GET_REQUEST){
                    return do_request();
                }
                break;
            case PARSE_STATE_BODY:
                if(parse_request_body(text)==GET_REQUEST){
                    return do_request();
                }
                break;
            default:
                return INTERNAL_ERROR; // 内部错误
        }
        
        m_start_line=m_check_idx;
    }
    return NO_REQUEST;
}
    
// 根据换行符\r\n找到下一行
http_conn::LINE_STATUS http_conn::find_next_line(){
    char ch;
    for(;m_check_idx<m_read_idx;m_check_idx++){
        ch=m_read_buf[m_check_idx];
        if(ch=='\r'){
            if(m_check_idx+1==m_read_idx){
                return LINE_OPEN; // 行不完整
            }
            if(m_read_buf[m_check_idx+1]=='\n'){
                m_read_buf[m_check_idx++] = '\0';
                m_read_buf[m_check_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        if(ch=='\n'){
            if(m_check_idx>1 && m_read_buf[m_check_idx-1]=='\r'){
                m_read_buf[m_check_idx - 1] = '\0';
                m_read_buf[m_check_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

/*
GET / HTTP/1.1
Host: 10.211.55.3:8888
Connection: keep-alive
Cache-Control: max-age=0
Upgrade-Insecure-Requests: 1
User-Agent: Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/115.0.0.0 Safari/537.36
Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,image/apng,*//*;q=0.8,application/signed-exchange;v=b3;q=0.7
Accept-Encoding: gzip, deflate
Accept-Language: zh-CN,zh;q=0.9

POST /submit HTTP/1.1
Host: www.example.com
User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:90.0) Gecko/20100101 Firefox/90.0
Content-Type: application/json
Content-Length: 25
Connection: keep-alive

{"key1": "value1", "key2": "value2"}
*/
// 解析请求行
http_conn::HTTP_CODE http_conn::parse_request_line(char *text){
    // GET / HTTP/1.1
    // 用正则表达式找到空白符，提取出每一个空白符前的词
    std::string str=text; // char *转string以\0作为分割依据
    str.push_back(' ');
    std::__cxx11::regex pattern("\\s"); // \s代表所有空白符，多加一个\转义
    std::vector<std::string> v;
    std::__cxx11::sregex_iterator it(str.begin(),str.end(),pattern);
    std::__cxx11::sregex_iterator end_it;
    for(;it!=end_it;it++ ){
        v.push_back(it->prefix().str());
    }
    
    // BAD_REQUEST:元素不全 or 不是get和post其一的请求方式 or 版本不是1.1
    if(v.size()!=3){
        return BAD_REQUEST;
    }

    if(v[0]=="GET"){
        m_method=GET;
    }else if(v[0]=="POST"){
        m_method=POST;
    }else{
        return BAD_REQUEST;
    }

    m_url=v[1];

    m_version=v[2];
    if(m_version!="HTTP/1.1"){
        return BAD_REQUEST;
    }

    m_parse_state=PARSE_STATE_HEADER; // 状态转移
    return NO_REQUEST;
}

// 解析请求头部
http_conn::HTTP_CODE http_conn::parse_request_headers(char *text){
/*
    Host: 10.211.55.3:8888 有必要解析出来吗？？？？？？？？？？？？
    Connection: keep-alive
    Content-Length: 25 // 如果请求体不为空，会有这一行
*/
    if(text[0]=='\0'){ // 到达空行，请求头部解析完了
        if(m_body_len!=0){
            m_parse_state=PARSE_STATE_BODY;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }

    // std::__cxx11::regex pattern("Host:\\s");
    // smatch res;
    // if(regex_search(text,res,pattern)){
    //     res.suffix().str();
    // }
    std::string s=text;
    std::__cxx11::regex pattern1("Connection:\\s");
    std::smatch res;
    if(regex_search(s,res,pattern1)){
        m_linger=res.suffix().str()=="keep-alive";
    }
    
    std::__cxx11::regex pattern2("Content-Length:\\s");
    if(regex_search(s,res,pattern2)){
        m_body_len=stoi(res.suffix().str()); // string->int:stoi  char *->int:atoi
    }
    return NO_REQUEST;
}

// 解析请求体，实际只判断是否完整读入
http_conn::HTTP_CODE http_conn::parse_request_body(char *text){
    if (m_read_idx >= (m_body_len /*+ m_check_idx*/)) { // ？？？？？？？？？？
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

// 请求资源
http_conn::HTTP_CODE http_conn::do_request(){
    if(m_url=="/"){
        m_url=doc_root+"/lingtang.html";
    }else{
        m_url=doc_root+m_url;
    }
    // m_url=doc_root+m_url; // 拼接出完整路径
    const char* file=m_url.c_str(); // str.c_str()将string转换为const char*
/*
    int stat(const char *pathname, struct stat *statbuf);
    pathname：一个字符串，表示要查询信息的文件路径或目录路径
    statbuf：一个指向 struct stat 结构的指针，用于存储获取到的文件或目录的详细信息

    struct stat {
        dev_t     st_dev;     // 设备 ID
        ino_t     st_ino;     // 文件的 inode
        mode_t    st_mode;    // 文件模式（权限和类型）
        nlink_t   st_nlink;   // 连接数
        uid_t     st_uid;     // 所有者的用户 ID
        gid_t     st_gid;     // 所有者的组 ID
        dev_t     st_rdev;    // 设备 ID（如果是特殊文件）
        off_t     st_size;    // 文件大小（字节数）
        blksize_t st_blksize; // 文件系统 I/O 块大小
        blkcnt_t  st_blocks;  // 分配的块数
        time_t    st_atime;   // 最后访问时间
        time_t    st_mtime;   // 最后修改时间
        time_t    st_ctime;   // 最后状态更改时间
    };
*/
    // 获取文件的相关的状态信息 
    if(stat(file,&m_file_info)!=0){ 
        return NO_RESOURCE;
    }
    // 判断访问权限
    if ( ! ( m_file_info.st_mode & S_IROTH ) ) { // others have read permission
        return FORBIDDEN_REQUEST;
    }
    // 判断是否是目录
    if ( m_file_info.st_mode & S_IFDIR ) {
        return BAD_REQUEST;
    }
    // 以只读方式打开文件
    int fd = open( file, O_RDONLY );
/*
    void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
    -addr：映射区域的首地址。通常设置为 NULL，让系统自动分配。
    -length：映射区域的大小（字节数）。
    -prot：映射区域的保护权限。可以是以下标志的组合：
        PROT_READ：可读
        PROT_WRITE：可写
        PROT_EXEC：可执行
        PROT_NONE：无权限
    -flags：控制映射区域的一些特性。可以是以下标志的组合：
        MAP_SHARED：映射区域可以被多个进程共享
        MAP_PRIVATE：映射区域是进程私有的
        MAP_FIXED：尝试在指定地址创建映射区域（仅当 addr 不为 NULL 时有效）
        fd：要映射的文件的文件描述符。如果不是映射文件，可以传递 -1。
    -offset：映射的文件在磁盘上的偏移量。通常设置为 0。
    -返回值：
        成功时，返回映射区域的起始地址。
        失败时，返回 MAP_FAILED（通常是 (void*)-1），并设置 errno。
*/
    // 创建内存映射 ？？？？？？？为什么要这样做
    m_file_address = ( char* )mmap( NULL, m_file_info.st_size, PROT_READ, MAP_PRIVATE, fd, 0 );
    close( fd );
    return FILE_REQUEST;
}

// 整合响应资源
bool http_conn::process_write(HTTP_CODE ret){
    switch(ret){
        case FILE_REQUEST:
            if(!add_response_line(200,ok_200_title)){
                return false;
            }
            if(m_file_info.st_size!=0){
                if(!add_response_headers(m_file_info.st_size)){
                    return false;
                }
                m_iov[0].iov_base=m_write_buf;
                m_iov[0].iov_len=m_write_idx;
                m_iov[1].iov_base = m_file_address;
                m_iov[1].iov_len = m_file_info.st_size;
                m_iov_count = 2;
                bytes_to_send = m_write_idx + m_file_info.st_size;
                return true;
            }else{
                const char *ok_string="<html><body></body></html>";
                if(!add_response_headers(strlen(ok_string))){
                    return false;
                }
                if(!add_response_body(ok_string)){
                    return false;
                }
            }
            break;
        case BAD_REQUEST: case NO_RESOURCE:
            if(!add_response_line(404, error_404_title)){
                return false;
            }
            if(!add_response_headers(strlen(error_404_form))){
                return false;
            }
            if (!add_response_body(error_404_form))
                return false;
            break;
        case FORBIDDEN_REQUEST:
            if(!add_response_line(403, error_403_title)){
                return false;
            }
            if(!add_response_headers(strlen(error_403_form))){
                return false;
            }
            if (!add_response_body(error_403_form))
                return false;
            break;
        case INTERNAL_ERROR:
            if(!add_response_line(500, error_500_title)){
                return false;
            }
            if(!add_response_headers(strlen(error_500_form))){
                return false;
            }
            if (!add_response_body(error_500_form)){
                return false;
            }  
            break;
        default:
            return false;
    }
    m_iov[0].iov_base = m_write_buf;
    m_iov[0].iov_len = m_write_idx;
    m_iov_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

// 按照传入格式写数据到写缓冲区
bool http_conn::format_write(const char *format,...){
    if(m_write_idx >= WRITE_BUFFER_SIZE){
        return false;
    }
    va_list arg_list; // 存储可变参数，通常是一个指向参数列表的指针
/*
    void va_start(va_list ap, last);
    -ap：va_start 函数将初始化这个对象，以便后续可以使用 va_arg 来逐个获取参数。
    -last：这是最后一个已知的参数。va_start 将根据这个参数的地址来定位可变参数的起始位置。
*/
    va_start(arg_list,format); // 把可变参数放到arg_list里
/*
    int vsnprintf(char *str, size_t size, const char *format, va_list ap);
    -str：vsnprintf 会将格式化后的结果写入该字符数组。
    -size：这是字符数组 str 的大小，也就是能够存储的最大字符数。函数会确保输出的字符不超过这个限制。
    -format：这是一个格式化字符串，类似于 printf 中的格式化字符串，用于指定输出的格式。
    -ap：这是一个 va_list 对象，用于存储可变参数的信息。vsnprintf 会根据 format 中的格式化指令和 ap 中的参数来生成格式化后的字符串。
    -返回值：
        函数的返回值是生成的字符数，但不包括末尾的 null 字符。
        如果生成的字符数（包括 null 字符）超过了 size，则函数会返回一个负数，表示缓冲区不足以存储结果。
*/
    int len=vsnprintf(m_write_buf+m_write_idx,WRITE_BUFFER_SIZE-m_write_idx,format,arg_list); // 按格式写入，注意第一个参数
    if(len<0){
        // void va_end(va_list ap); 用于释放 va_list 对象占用的资源
        va_end(arg_list);
        return false;
    }
    m_write_idx+=len;
    va_end(arg_list);
    return true;
}

/*
    HTTP/1.1 200 OK
    Content-Type: text/html
    Content-Length: 123
    Connection: keep-alive

    ...
*/
// 添加响应行
bool http_conn::add_response_line(int status, const char *title){
    return format_write("%s %d %s\r\n","HTTP/1.1",status,title);
}

// 添加响应头部和空行
bool http_conn::add_response_headers(int content_length){
    if(!add_content_type()){
        return false;
    }
    if(!add_content_length(content_length)){
        return false;
    }
    if(!add_connection()){
        return false;
    }
    if(!add_blank_line()){
        return false;
    }
    return true;
}

// 内容类型
bool http_conn::add_content_type(){
    return format_write("Content-Type: %s\r\n","text/html");
}
// 内容长度
bool http_conn::add_content_length(int len){
    return format_write("Content-Length: %d\r\n",len);
}
// 是否保持连接
bool http_conn::add_connection(){
    return format_write("Connection: %s\r\n",m_linger?"keep-alive":"close");
} 
// 空行
bool http_conn::add_blank_line(){
    return format_write("%s","\r\n");
}

// 响应正文
bool http_conn::add_response_body(const char* body){
    return format_write("%s",body);
}

// 关闭这个http连接
void http_conn::close_conn(){
    delfd(m_epollfd,m_sockfd);
    m_sockfd=-1; // 重置文件描述符
    m_conn_count--;
}