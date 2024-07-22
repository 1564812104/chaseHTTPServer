#include "http_conn.h"

//定义HTTP响应的一些状态信息
const char * ok_200_title = "OK";
const char * error_400_title = "Bad Request";
const char * error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char * error_403_title = "Forbidden";
const char * error_403_form = "You do not have permission to get file from this server.\n";
const char * error_404_title = "Not Found";
const char * error_404_form = "The requested file was not fount on this server.\n";
const char * error_500_title = "INternal Error";
const char * error_500_form = "There was an unusual problem serving the requested file.\n";


// const char * doc_root = "/home/dir";//网站的根目录

//设置非阻塞
int setnonblocking(int fd){
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//把文件描述符加到epoll监听树上并设置为ET非阻塞模式
void addfd(int epollfd, int fd, bool one_shot)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    if(one_shot)
    {
        //使得当一个线程在处理某个socket时，其他线程操作该SOcket
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//把文件描述符从epoll监听树上拿下来并关闭
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

//修改文件描述符属性
void modfd(int epollfd, int fd, int ev)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

void http_conn::close_conn(bool real_close){
    if(real_close && (m_sockfd != -1)){
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        //关闭一个连接客户数减1；
        m_user_count--;
    }
}

void http_conn::init(int sockfd, const sockaddr_in& addr){
    m_sockfd = sockfd;
    m_address = addr;
    //信道复用
    int reuse = 1;
    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    addfd(m_epollfd, sockfd, true);
    m_user_count++;

    init();
}

void http_conn::init(){
     m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
 
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    bytes_have_send = 0;
    bytes_to_send = 0;
    memset( m_read_buf, '\0', READ_BUFFER_SIZE );
    memset( m_write_buf, '\0', WRITE_BUFFER_SIZE );
    memset( m_real_file, '\0', FILENAME_LEN );
}

////从状态机，用于解析一行内容
http_conn::LINE_STATUS http_conn::parse_line(){
    char temp;
    //checked_index指向buffer（应用程序的读缓冲区）中正在分析的字节，read_index指向buffer中客户数据的尾部的下一字节。
    //buffer中第0-checked_index字节都已分析完毕，第checked_index-(read_index - 1)字节由下面循环挨个分析
    for(; m_checked_idx < m_read_idx; ++m_checked_idx){
        //获取当前要分析的字节
        temp = m_read_buf[m_checked_idx];
        //如果当前的字节是\r即回车符，说明可能读到一个完整的行
        if(temp == '\r'){
            //如果\r是buffer中最后一个已经被读入的客户数据，那么这次分析没有读到一个完整的行，返回LINE_OPEN表示还需要继续读取才能解析
            if((m_checked_idx + 1) == m_read_idx){
                return LINE_OPEN;
            }else if(m_read_buf[m_checked_idx + 1] == '\n'){
                //如果下一字符是\n，说明读到一个完整行
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            //说明客户发送的HTTP请求存在语法问题
            return LINE_BAD;
        }else if (temp == '\n'){
            /* 读到一个\n，换行符也说明可能读到一个完整的行 */
            if((m_checked_idx > 1) && (m_read_buf[m_checked_idx - 1] == '\r')){
                m_read_buf[m_checked_idx-1] = '\0';
                m_read_buf[m_checked_idx++]   = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    //没读到\r\n说明还得继续读
    return LINE_OPEN;
}

//循环读取客户数据，知道无数据可读或者对方关闭连接
bool http_conn::read(){
    if(m_read_idx >= READ_BUFFER_SIZE){
        return false;
    }

    int bytes_read = 0;
    while(true){
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        if(bytes_read == -1){
            if(errno == EAGAIN || errno == EWOULDBLOCK){
                break;
            }
            return false;
        }else if(bytes_read == 0){
            return false;
        }
        m_read_idx += bytes_read;
    }
    return true;
}

//解析HTTP请求行，获得请求方法、目标URL，以及HTTP版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char* text){
    //请求行组成  请求 -- url --- 协议
    //找到空格位置，匹配第一个空格，空格后面对应的就是url
    m_url = strpbrk(text ," \t");
    //如果请求行中没有空白字符或"\t字符，则HTTP请求必有问题
    if(!m_url){
        return BAD_REQUEST;
    }
    //将找到的空格位置写\0， 并将url指针自增1，指向真正url
    *m_url++ = '\0';

    //method指回到请求头的第一个位置
    char* method = text;
    //和GET进行匹配
    if(strcasecmp(method, "GET") == 0){
        printf("The request method is GET\n");
        m_method = GET;
    }else{
        return BAD_REQUEST;
    }

    // strspn 检索字符串 str1 中第一个不在字符串 str2 中出现的字符下标
    m_url += strspn(m_url, " \t");
    //匹配从url开始的第一个空格，空格后面的就是version
    m_version = strpbrk(m_url, " \t");
    if(!m_version){
        return BAD_REQUEST;
    }
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    //仅支持HTTP/1.1
    if(strcasecmp(m_version, "HTTP/1.1") != 0){
        printf("协议为HTTP/1.1");
        return BAD_REQUEST;
    }
    //检查url是否合法
    if(strncasecmp(m_url, "http://", 7) == 0){
        m_url += 7;
        //strchr 在参数 str 所指向的字符串中搜索第一次出现字符 c（一个无符号字符）的位置（找http://后面的第一个/位置）
        m_url = strchr(m_url, '/');
    }
    if(!m_url || m_url[0] != '/'){
        return BAD_REQUEST;
    }
    printf("The request URL is : %s\n", m_url);
    //HTTP请求行处理完毕，状态转移到头部字段的分析
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

//解析HTTP请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char* text){
    //遇到一个空行，说明我们得到了一个正确的HTTP请求
    if(text[0] == '\0'){
        //如果HTTP请求有消息体，则还需要读取m_content_length字节的消息体，
        //状态机转移到CHECK_STATE_CONTENT状态
        if(m_content_length != 0){
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }else if(strncasecmp(text, "Host:", 5) == 0){  //比较temp前五个字母
        //处理host头部字段
        //跳过host:
        text += 5;
        // 检索字符串 str1 中第一个不在字符串 str2 中出现的字符下标
        text += strspn(text, "\t");
        printf("the request host is:%s\n", text);
    }else if(strncasecmp(text, "Connection:", 11) == 0){
        //处理Connection头部字段
        text += 11;
        text += strspn(text, " \t");
        if(strcasecmp(text, "keep-alive") == 0){
            m_linger = true;
        }
    }else if(strncasecmp(text, "Content-Length:", 15) == 0){
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }else{
        printf("oop! unknow header %s\n", text);
    }
    return NO_REQUEST;
}

//没有真正解析HTTP请求的消息体，只是读入完整的消息体
http_conn::HTTP_CODE http_conn::parse_content(char* text){
    if(m_read_idx >= (m_content_length + m_checked_idx)){
        text[ m_content_length ] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

//主状态机
http_conn::HTTP_CODE http_conn::process_read(){
    //buffer装着从客户端读来的内容，read_index是截止到哪， check_index代表从哪开始分析
    LINE_STATUS line_status = LINE_OK;  //记录当前行的读取状态
    HTTP_CODE ret = NO_REQUEST;    //记录HTTP请求的处理结果
    char* text = 0;
    //主状态机，用于从buffer中取出所有完整的行
    while((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK) || (line_status = parse_line()) == LINE_OK){
        //如果返回ok了，那么就代表拿到了完整的一行请求，并且这行请求以/0结尾
        text = get_line();                 //return m_read_buf + m_start_line;也就是当分析轮次的初始位置
        m_start_line = m_checked_idx;        //记录下一行的起始状态（因为m_checked_idx已经指向下一行数据的第一个位置了）
        //checkstate记录当前主状态机当前的状态
        switch (m_check_state)
        {
            case CHECK_STATE_REQUESTLINE:{
                //第一个状态， 分析请求行
                ret = parse_request_line(text);
                if(ret == BAD_REQUEST){
                    return BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE_HEADER:{   
                //第二个状态，分析头部字段
                ret = parse_headers(text);
                if(ret == BAD_REQUEST){
                    return BAD_REQUEST;
                }else if(ret == GET_REQUEST){
                    return do_request();
                }
                break;
            } 
            case CHECK_STATE_CONTENT:{
                ret = parse_content(text);
                if(ret == GET_REQUEST){
                    return do_request();
                }
                line_status = LINE_OPEN; //还需要继续读
                break;
            }
            default:{
                return INTERNAL_ERROR;
            }
        }
    }
    //若没有读取到 一个完整行，则表示害需要继续读取客户数据才能进一步分析
    return NO_REQUEST;
}

// 16进制数转化为10进制
int http_conn::hexit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;

    return 0;
}

/*
 *  这里的内容是处理%20之类的东西！是"解码"过程。
 *  %20 URL编码中的‘ ’(space)
 *  %21 '!' %22 '"' %23 '#' %24 '$'
 *  %25 '%' %26 '&' %27 ''' %28 '('......
 *  相关知识html中的‘ ’(space)是&nbsp
 */
void http_conn::encode_str(char* to, int tosize, const char* from)
{
    int tolen;

    for (tolen = 0; *from != '\0' && tolen + 4 < tosize; ++from) {    
        if (isalnum(*from) || strchr("/_.-~", *from) != (char*)0) {      
            *to = *from;
            ++to;
            ++tolen;
        } else {
            sprintf(to, "%%%02x", (int) *from & 0xff);
            to += 3;
            tolen += 3;
        }
    }
    *to = '\0';
}

void http_conn::decode_str(char *to, char *from)
{
    for ( ; *from != '\0'; ++to, ++from  ) {     
        if (from[0] == '%' && isxdigit(from[1]) && isxdigit(from[2])) {       
            *to = hexit(from[1])*16 + hexit(from[2]);
            from += 2;                      
        } else {
            *to = *from;
        }
    }
    *to = '\0';
}

//当得到一个完整的、正确的HTTP请求时，我们就分析目标文件的属性。如果目标文件存在、对所有用户可读，且不是目录，则使用mmap将
//其映射到内存地址m_file_address处，并告诉调用者获取文件成功
http_conn::HTTP_CODE http_conn::do_request(){
    // strcpy(m_real_file, doc_root);
    // int len = strlen(doc_root);
    //char *strncpy(char *dest, const char *src, size_t n) 把 src 所指向的字符串复制到 dest，最多复制 n 个字符。
    //当 src 的长度小于 n 时，dest 的剩余部分将用空字节填充。
    printf("m_url:%s\n", m_url);

    // 转码 将不能识别的中文乱码 -> 中文
    // 解码 %23 %34 %5f
    decode_str(m_url, m_url);

    // strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
    strncpy(m_real_file, m_url + 1, FILENAME_LEN - 1);
    printf("m_real_file:%s\n", m_real_file);
    char dirDialog[5] = "./";
    // 如果没有指定访问的资源, 默认显示资源目录中的内容
    if(strcmp(m_url, "/") == 0) {    
        // file的值, 资源目录的当前位置
        strncpy(m_real_file, dirDialog, FILENAME_LEN - 1);
        printf("dirpath = %s\n", m_real_file);
    }

    if(stat(m_real_file, &m_file_stat) < 0){
        return NO_RESOURCE;
    }
    if(!(m_file_stat.st_mode & S_IROTH)){
        return FORBIDDEN_REQUEST;
    }
    if(S_ISDIR(m_file_stat.st_mode)){
        //这里应该发送一个页面过去，页面中展示目录下的所有文件和目录
        printf("%s m_real_file\n", m_real_file);
        //对应文件的话应该把目录内容的地址指向m_file_address, 这里我觉得可以用数组来替代char buf[];

        return IS_DIR;
    }
    printf("%s m_real_file\n", m_real_file);
    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char*)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}

//对内存映射区执行munmap操作
void http_conn::unmap(){
    if(m_file_address){
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

//写HTTP相应
bool http_conn::write(){
    int temp = 0;
    // int bytes_have_send = 0;
    // int bytes_to_send = m_write_idx;
    if(bytes_to_send == 0){
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        init();
        return true;
    }
    while(1){
        // temp = writev(m_sockfd, m_iv, m_iv_count);
        // if(temp <= -1){
        //     //如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件，虽然在此期间
        //     //服务器无法立即接收到同一个客户的下一个请求，但这可以保证连接的完整性
        //     if(errno == EAGAIN){
        //         modfd(m_epollfd, m_sockfd, EPOLLOUT);
        //         return true;
        //     }
        //     unmap();  //这里有疑问？
        //     return false;
        // }
        // bytes_to_send -= temp;
        // bytes_have_send += temp;
        // if(bytes_to_send <= bytes_have_send){
        //     //发送HTTP相应成功，根据HTTP请求中的Connection字段决定是否立即关闭连接
        //     unmap();
        //     if(m_linger){
        //         init();
        //         modfd(m_epollfd, m_sockfd, EPOLLIN);
        //         return true;
        //     }else{
        //         modfd(m_epollfd, m_sockfd, EPOLLIN);
        //         return false;
        //     }
        // }
        temp = writev(m_sockfd, m_iv, m_iv_count);

        if(temp < 0){
            if(errno == EAGAIN){
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            }
            unmap();
            return false;
        }
        printf("写了多少数据%d\n", temp);
        //读了temp字节数的文件
        bytes_have_send += temp;
        //已经temp字节数的文件
        bytes_to_send -= temp;
        //如果可以发送的字节大于爆头，证明报头发送完毕
        if(bytes_have_send >= m_iv[0].iov_len){
            //报头长度清零
            m_iv[0].iov_len = 0;
            /*这行代码：因为m_write_idx表示为待发送文件的定位点，m_iv[0]指向m_write_buf，
            所以bytes_have_send（已发送的数据量） - m_write_idx（已发送完的报头中的数据量）
            就等于剩余发送文件映射区的起始位置*/
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }else{
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }
        if (bytes_to_send <= 0)
        {
            printf("写完了\n");
            //发送完毕，恢复默认值以便下次继续传输文件
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN);

            if (m_linger){
                init();
                return true;
            }else{
                return false;
            }
        }

    }
    

}

//向写缓冲中写入待发送的数据
bool http_conn::add_reponse(const char* format, ...){
    if(m_write_idx >= WRITE_BUFFER_SIZE){
        return false;
    }
    //VA_LIST 是在C语言中解决变参问题的一组宏，变参问题是指参数的个数不定，可以是传入一个参数也可以是多个;
    //可变参数中的每个参数的类型可以不同,也可以相同;可变参数的每个参数并没有实际的名称与之相对应，用起来是很灵活
    //定义一具VA_LIST型的变量，这个变量是指向参数的指针
    va_list arg_list;
    //用VA_START宏初始化变量刚定义的VA_LIST变量
    va_start(arg_list, format);
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    if(len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx)){
        return false;
    }
    m_write_idx += len;
    //用VA_END宏结束可变参数的获取
    va_end(arg_list);
    return true;
}

// 通过文件名获取文件的类型
const char * http_conn::get_file_type(const char *name)
{
    const char* dot;

    // 自右向左查找‘.’字符, 如不存在返回NULL
    dot = strrchr(name, '.');   
    if (dot == NULL)
        return "text/plain; charset=utf-8";
    if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0)
        return "text/html; charset=utf-8";
    if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0)
        return "image/jpeg";
    if (strcmp(dot, ".gif") == 0)
        return "image/gif";
    if (strcmp(dot, ".png") == 0)
        return "image/png";
    if (strcmp(dot, ".css") == 0)
        return "text/css";
    if (strcmp(dot, ".au") == 0)
        return "audio/basic";
    if (strcmp( dot, ".wav" ) == 0)
        return "audio/wav";
    if (strcmp(dot, ".avi") == 0)
        return "video/x-msvideo";
    if (strcmp(dot, ".mov") == 0 || strcmp(dot, ".qt") == 0)
        return "video/quicktime";
    if (strcmp(dot, ".mpeg") == 0 || strcmp(dot, ".mpe") == 0)
        return "video/mpeg";
    if (strcmp(dot, ".vrml") == 0 || strcmp(dot, ".wrl") == 0)
        return "model/vrml";
    if (strcmp(dot, ".midi") == 0 || strcmp(dot, ".mid") == 0)
        return "audio/midi";
    if (strcmp(dot, ".mp3") == 0)
        return "audio/mpeg";
    if (strcmp(dot, ".ogg") == 0)
        return "application/ogg";
    if (strcmp(dot, ".pac") == 0)
        return "application/x-ns-proxy-autoconfig";

    return "text/plain; charset=utf-8";
}

//将请求行加入到写缓冲区
bool http_conn::add_status_line(int status, const char* title){
    return add_reponse( "%s %d %s\r\n", "HTTP/1.1", status, title );
}

bool http_conn::add_headers(int content_len, const char* type){
    add_content_type(type);
    add_content_length( content_len );
    add_linger();
    add_blank_line();
}

bool http_conn::add_content_length(int content_len){
    return add_reponse("Content-Length: %d\r\n", content_len);
}

bool http_conn::add_linger(){
    return add_reponse("Connection: %s\r\n", (m_linger == true) ? "keep-alive" : "close");
}

bool http_conn::add_blank_line(){
    return add_reponse("%s", "\r\n");
}

bool http_conn::add_content_type(const char* type){
    printf("Content-Type:%s\n", type);
    return add_reponse("Content-Type:%s\r\n", type);
}

bool http_conn::add_content(const char* content){
    return add_reponse( "%s", content );
}



//根据服务器处理HTTP请求的结果，决定返回给客户端的内容
bool http_conn::process_write(HTTP_CODE ret){
    switch (ret)
    {
        case INTERNAL_ERROR:{
            add_status_line( 500, error_500_title );
            add_headers( strlen( error_500_form ) , get_file_type(".html"));
            if ( ! add_content( error_500_form ) )
            {
                return false;
            }
            break;
        }
        case BAD_REQUEST:{
            add_status_line( 400, error_400_title );
            add_headers( strlen( error_400_form ) , get_file_type(".html"));
            if ( ! add_content( error_400_form ) )
            {
                return false;
            }
            break;
        }
        case NO_RESOURCE:{
            add_status_line( 404, error_404_title );
            add_headers( strlen( error_404_form ) , get_file_type(".html"));
            if ( ! add_content( error_404_form ) )
            {
                return false;
            }
            break;
        }
        case FORBIDDEN_REQUEST:{
            add_status_line( 403, error_403_title );
            add_headers( strlen( error_403_form ) , get_file_type(".html"));
            if ( ! add_content( error_403_form ) )
            {
                return false;
            }
            break;
        }
        case FILE_REQUEST:{
            add_status_line(200, ok_200_title);
            if(m_file_stat.st_size != 0){
                printf("进到这个if里面了\n");
                add_headers(m_file_stat.st_size, get_file_type(m_real_file));
                m_iv[0].iov_base = m_write_buf;  //读缓冲区全是应答头
                m_iv[0].iov_len = m_write_idx;
                m_iv[1].iov_base = m_file_address;  //mmap映射的文件，也就是客户请求要看的文件
                m_iv[1].iov_len = m_file_stat.st_size;
                m_iv_count = 2;
                //下一行 为优化新增 保证还需要传入的数据量准确无误
                bytes_to_send = m_write_idx + m_file_stat.st_size;//还需传入的数据字节
                return true;
            }else{
                printf("进到这个else里面了\n");
                const char* ok_string = "<html><body></body></html>";
                add_headers(strlen(ok_string), get_file_type(".html"));
                if(!add_content(ok_string)){
                    return false;
                }
            }
        }
        case IS_DIR:{
            add_status_line(200, ok_200_title);
            //确定要发送的大小
            //把协议头装进去
            int i, ret;
            
            // 拼一个html页面<table></table>
            char buf[4096] = {0};

            sprintf(buf, "<html><head><title>目录名: %s</title></head>", m_real_file);
            sprintf(buf+strlen(buf), "<body><h1>当前目录: %s</h1><table>", m_real_file);

            char enstr[1024] = {0};
            char path[1024] = {0};
            
            // 目录项二级指针
            struct dirent** ptr;
            int num = scandir(m_real_file, &ptr, NULL, alphasort);
            
            // 遍历
            for(i = 0; i < num; ++i) {
            
                char* name = ptr[i]->d_name;

                // 拼接文件的完整路径
                sprintf(path, "%s/%s", m_real_file, name);
                printf("path = %s ===================\n", path);
                struct stat st;
                stat(path, &st);

                //编码生成 %E5 %A7 之类的东西
                encode_str(enstr, sizeof(enstr), name);
                
                // 如果是文件
                if(S_ISREG(st.st_mode)) {       
                    sprintf(buf+strlen(buf), 
                            "<tr><td><a href=\"%s\">%s</a></td><td>%ld</td></tr>",
                            enstr, name, (long)st.st_size);
                } else if(S_ISDIR(st.st_mode)) {		// 如果是目录       
                    sprintf(buf+strlen(buf), 
                            "<tr><td><a href=\"%s/\">%s/</a></td><td>%ld</td></tr>",
                            enstr, name, (long)st.st_size);
                }
                 // 如果是文件
                // if(S_ISREG(st.st_mode)) {       
                //     sprintf(buf+strlen(buf), "%s\n", name);
                // } else if(S_ISDIR(st.st_mode)) {		// 如果是目录       
                //     sprintf(buf+strlen(buf), "%s\n", name);
                // }
                // memset(buf, 0, sizeof(buf));
                // 字符串拼接
            }

            sprintf(buf+strlen(buf), "</table></body></html>");
            printf("size of buf %d\n", strlen(buf));
            printf("dir message send OK!!!!\n");

            add_headers(strlen(buf), get_file_type(".html"));
            m_iv[0].iov_base = m_write_buf;  //读缓冲区全是应答头
            m_iv[0].iov_len = m_write_idx;
            //把内容装进去
            m_iv[1].iov_base = buf;  //mmap映射的文件，也就是客户请求要看的文件
            m_iv[1].iov_len = strlen(buf);
            m_iv_count = 2;
            //下一行 为优化新增 保证还需要传入的数据量准确无误
            bytes_to_send = m_write_idx + strlen(buf);//还需传入的数据字节
            return true;
        }
    default:
        return false;
    }
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    //下一行 为优化新增 如果是其他行为，待发送的字节数则就位写缓冲区的数据
    bytes_to_send = m_write_idx;
    return true;
}

//线程池中的工作线程调用，这是处理HTTP请求的入口函数
//这里因为是某一个线程调用的，一个线程从工作队列里面拿一个socket的处理任务，所以这个m_sockfd会对应到那个socket的文件描述符
void http_conn::process(){
    HTTP_CODE read_ret = process_read(); //解析来的请求
    //如果解析全的话就得继续去监听读，不能放他往下走往写缓冲力写
    if(read_ret == NO_REQUEST){
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return;
    }
    bool write_ret = process_write(read_ret);
    if(!write_ret){
        close_conn();
    }
    //把东西都写到写缓冲里面了就监听写
    modfd(m_epollfd, m_sockfd, EPOLLOUT);
}



