#include <map>
#include <unordered_map>
#include <mysql/mysql.h>
#include <fstream>
#include "http_conn.h"



//定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";


const char *doc_root = "/home/liushuai/Myworkspace/MyWebServer/root";
map<string, string> users;
locker m_lock;



//! 将数据库中的用户名和密码加载到服务器中的map中来
void http_conn::initmysql_result(connection_pool *connPool)
{
    //先从连接池中取一个连接
    MYSQL *mysql = NULL;
    ConnectionRaII mysqlcon(&mysql, connPool);

    //在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        //LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

// 对文件描述符设置非阻塞
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

// 将内核事件表注册读事件， ET模式， 并设置非阻塞模式
void addfd(int epollfd, int fd, bool one_shot)
{
    epoll_event event;
    event.data.fd = fd;

    if(one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd ,&event);
    setnonblocking(fd);
}

//初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in &addr)
{
    m_sockfd = sockfd;
    m_address = addr;
    //int reuse=1;
    //setsockopt(m_sockfd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
    addfd(m_epollfd, sockfd, true);
    cout << "************ connfd add epolltree **************" << endl;
    m_user_count++;
    init();
}

//初始化新接受的连接
//check_state默认为分析请求行状态
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
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
    cgi = 0;
    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

//循环读取客户数据，直到无数据可读或对方关闭连接
//非阻塞ET工作模式下，需要一次性将数据读完
bool http_conn::read_once()
{   // 数据的下一个位置超出缓冲区的长度
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    int bytes_read = 0;

                                // 缓冲区的起始位置         // 缓冲区的长度
    bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);

    // 修改m_read_idx的读取字节数
    m_read_idx += bytes_read;

    if (bytes_read <= 0)
    {
        return false;
    }
    return true;
}


// 将事件重置为EPOLLONESHOT
void modfd(int epollfd, int fd, int ev)
{
    epoll_event event;
    event.data.fd = fd;

    event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}


//从内核时间表删除描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;
//关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}



// 解析http请求行，获得请求方法，目标url及http版本号
// 解析完主状态机的状态变成CHECK_STATE_HEADER 以便主状态机进行状态转移
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    m_url = strpbrk(text, "\t");
    if(!m_url)
    {
        return BAD_REQUEST;
    }

    *m_url++ = '\0';
    char *method = text;

    if(strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if(strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }    
    else    
        return BAD_REQUEST;
    
    m_url += strspn(m_url, " \t");
    m_version = strpbrk(m_url, " \t");
    if(!m_version)
        return BAD_REQUEST;
    
    m_version += strspn(m_version, " \t");

    if(strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;
    
    // 对请求资源的前七个字符进行判断
    // 这里主要是有些报文的请求资源中会带有http://，这里需要对这种情况进行单独处理
    if(strncasecmp(m_url, "http://",7) == 0){
        m_url += 7;
        // strchr 该函数返回在字符串 str 中第一次出现字符 c 的位置
        // 目的就是去掉前面的http:/
        m_url = strchr(m_url, '/');
    }
    // https的情况,方法同上
    if(strncasecmp(m_url, "https://",8) == 0){
        m_url += 8;
        m_url = strchr(m_url, '/');
    }
    // 一般的不会带有上述两种符号，直接是单独的/或/后面带访问资源
    if(!m_url || m_url[0] != '/')
        return BAD_REQUEST;
    // 显示 登录 or 注册的界面
    if(strlen(m_url) == 1)
        strcat(m_url, "judge.html");

    // 请求行处理完毕，将主状态机转移请求处理请求头
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}


// 解析http 请求的头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char* text)
{
    if(text[0] == '\0')
    {
        if(m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t"); // 去掉空格


        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;
        }
    }
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else{
        // log
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::parse_content(char* text)
{   

    // 判断buffer中是否读取了消息体
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        // POST请求中最后为输入的用户名和密码
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}






//从状态机，用于分析出一行内容
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
//! m_read_idx 指向缓冲区m_read_buf数据末尾的下一个字节
//! m_checked_idx 指向从状态机当前正在分析的字节
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {   
        //! temp 为将要分析的字节
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r')
        {   
            //! 没有"\r\n"这样的结束标志,但是指针指向了末尾,说明buffer还需要继续接收
            if ((m_checked_idx + 1) == m_read_idx)
                return LINE_OPEN; //! 读取的行不完整
            //! 即读到了"\r\n",读到了一行的末尾,则将\r\n修改成\0\0
            else if (m_read_buf[m_checked_idx + 1] == '\n') 
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK; //! 完整读取
            }
            //语法错误
            return LINE_BAD;
        }
        //! 一般是上次读取到\r就到了buffer末尾，没有接收完整，再次接收时会出现这种情况
        else if (temp == '\n')
        {   //! 当前字符是"\n", 判断前一个字符是否为"\r", 则将"\r\n"修改成""\0\0"
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    //并没有找到\r\n，需要继续接收
    return LINE_OPEN;
}

http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    while((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) && ((line_status = parse_line()) == LINE_OK))
    {
        text = get_line();
        m_start_line = m_checked_idx;
        cout << "************* text ****************" << endl; 
        cout << text << endl;
        switch(m_check_state){

            // 解析请求行
            case CHECK_STATE_REQUESTLINE:
            {
                ret = parse_request_line(text);
                if(ret == BAD_REQUEST)
                    return BAD_REQUEST;
                break;
            }
            // 解析请求头
            case CHECK_STATE_HEADER:
            {
                ret = parse_headers(text);
                if(ret == BAD_REQUEST)
                    return BAD_REQUEST;
                else if(ret == GET_REQUEST)
                {   // 完整解析 GET 请求后， 跳转到报文响应函数
                    return do_request();
                }
                break;
            }
            // 解析消息体
            case CHECK_STATE_CONTENT:
            {
                ret = parse_content(text);
                // 完整解析 GET 请求后， 跳转到报文响应函数
                if(ret == GET_REQUEST)
                    return do_request();
                // 解析完消息体即完成报文解析，避免再次进入循环，更新line_status
                line_status = LINE_OPEN;
                break;
            }
            default:
                return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::do_request()
{   
    // m_real_file = "/home/liushuai/Myworkspace/TinyWebServer-raw_version/root"
    strcpy(m_real_file, doc_root);
    int len = sizeof(doc_root);

    const char* p = strrchr(m_url, '/');
    // cgi = 1 是POST 请求
    // 登录或者注册
    if(cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {   
        // 根据标志判断是登录检测还是注册检测
        char flag = m_url[1];
        
        char* m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url+2); // 把m_url中/2 后面的路径添加到m_url_real 中
        // 把 m_url_real 添加到m_real_file， 至此添加到m_real_file是需要文件或者目录的完整路径
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1); 
        free(m_url_real);

        // 提取姓名和密码
        //user=123&passwd=123, 这个格式是 cgi 解释器规定的 
        char name[100], password[100];
        int i;

        for(i = 5; m_string[i] != '&'; ++i)
            name[i-5] = m_string[i];
        name[i-5] = '\0';

        int j = 0;
        for(i = i + 10; m_string[i] != '\0'; ++i,++j)
            password[j] = m_string[i];
        password[j] = '\0';

        // '3' 是注册
        if(*(p + 1) == '3')
        {
            //如果是注册，先检测数据库中是否有重名的
            //没有重名的，进行增加数据

            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            // sql_insert = "INSERT INTO user(usename, passwd) VALUES(" 'name', 'password')
            strcpy(sql_insert, "INSERT INTO user(usename, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            // 在数据库中找不到 登录时的名字
            if(users.find(name) == users.end())
            {   // 向数据库中插入数据
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();

                // 查询失败则说明校验成功,跳转登录页面
                if(!res)
                    strcpy(m_url, "/log.html");
                else    
                    strcpy(m_url, "/registerError.html");
            }
            else
                strcpy(m_url, "/registerError.html");
        }
        // 如果是登录, 直接判断
        else if(*(p + 1) == '2')
        {   
            // 判断能否找到对应的名字 以及 名字对应的密码是否正确
            if(users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/welcome.html");
            else    
                strcpy(m_url, "/logError.html");
        }
    }
    
    // 如果请求资源为\0,表示跳转注册页面
    if (*(p + 1) == '0')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        //将网站目录和/register.html进行拼接，更新到m_real_file中
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    // 如果请求资源为\1,表示跳转登录页面
    else if (*(p + 1) == '1')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        //将网站目录和/log.html进行拼接，更新到m_real_file中
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    // 跳转到图片页面
    else if (*(p + 1) == '5')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }

    // 跳转到视频页面
    else if (*(p + 1) == '6')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }

    // 跳转到关注页面
    else if (*(p + 1) == '7')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else    
        // 如果以上均不符合,即不是登录和注册,直接将url与目录网站拼接
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
    
    if(stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;   // 表示资源不存在
    // 判断文件是否可读
    if(!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;

    // 判断文件是否是目录， 如果是，返回BAD_REQUEST，表示请求报文有误
    if(S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;

    // 以只读方式打开文件获取文件描述符， 通过mmap 将该文件映射到内存上
    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    // 表示文件存在， 而且可以访问
    return FILE_REQUEST;
}


void http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

// 将响应报文发送给浏览器端
bool http_conn::write()
{
    int temp = 0;

    // 响应报文为空
    if (bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        init();
        return true;
    }

    while (1)
    {   //! 将响应报文(状态行，消息头，空行和响应正文)   发送给浏览器端
        temp = writev(m_sockfd, m_iv, m_iv_count);

        // 发送有异常
        if (temp < 0)
        {   // 判断缓冲区是否满了
            if (errno == EAGAIN)
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            }
            unmap();
            return false;
        }

        // 更新已经发送的字节
        bytes_have_send += temp;
        bytes_to_send -= temp;
        if (bytes_have_send >= m_iv[0].iov_len)
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        // 数据已经全都发送完毕
        if (bytes_to_send <= 0)
        {   
            // 取消映射
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN);

            // 如果是长连接则重新初始化
            if (m_linger)
            {
                init();
                return true;
            }
            else
            {
                return false;
            }
        }
    }
}


//! 利用可变形参向m_write_buf缓冲区写入内容， 并更新m_write_idx指针
bool http_conn::add_response(const char *format, ...)
{   // 如果写入内容超出m_write_buf大小则报错
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;
    // 定义可变参数列表
    va_list arg_list;
    // 将可变形参列表的第一参数地址赋值给arg_list
    va_start(arg_list, format);

    // 将数据format 从可变形参列表写入缓冲区， 返回写入数据的长度
    // vsnprintf: 将可变参数格式化输出到一个字符数组
    //                 写入的起始地址                 数组长度                             要写入的内容    形参列表
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format,      arg_list);
    // 如果要写入的数据长度超过缓冲区剩余空间， 则报错
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);
        return false;
    }
    m_write_idx += len;

    // 清空形参列表
    va_end(arg_list);
    // LOG_INFO("request:%s", m_write_buf);
    // Log::get_instance()->flush();
    return true;
}



// 添加状态行: http/1.1 状态码 状态消息
bool http_conn::add_status_line(int status, const char* title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

// 添加消息报头
bool http_conn::add_headers(int content_len)
{
    add_content_length(content_len);
    add_linger();
    add_blank_line();
}

// 添加响应报文长度
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}
// 添加文本类型， 这里是html
bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}
// 添加链接状态， 通知浏览器端是连接还是关闭
bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}
// 添加空行
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
} 

// 添加消息体
bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}

bool http_conn::process_write(HTTP_CODE ret)
{
    switch(ret)
    {   
        // 内部错误 500
        case INTERNAL_ERROR:
        {
            add_status_line(500, error_500_title);
            add_headers(strlen(error_500_form));
            if(!add_content(error_500_form))
                return false;
            break;
        }
        // 报文语法有错,404
        case BAD_REQUEST:
        {
            add_status_line(404, error_404_title);
            add_headers(strlen(error_404_form));
            if (!add_content(error_404_form))
                return false;
            break;
        }
        // 资源没有权限访问,403
        case FORBIDDEN_REQUEST:
        {
            add_status_line(403, error_403_title);
            add_headers(strlen(error_403_form));
            if (!add_content(error_403_form))
                return false;
            break;
        }
        case FILE_REQUEST:
        {
            add_status_line(200, ok_200_title);
            // 如果请求的文件存在， 需要将其映射到共享内存
            if(m_file_stat.st_size != 0)
            {
                add_headers(m_file_stat.st_size);
                // 第一个iovec 指针指向响应报文缓冲区, 长度指向m_write_idx
                m_iv[0].iov_base = m_write_buf;
                m_iv[0].iov_len = m_write_idx;

                m_iv[1].iov_base = m_file_address;
                m_iv[1].iov_len = m_file_stat.st_size;
                m_iv_count = 2;
                bytes_to_send = m_write_idx + m_file_stat.st_size;
                return true;
            }
            else{
                const char* ok_string = "<html><body></body></html>";
                add_headers(strlen(ok_string));
                if(!add_content(ok_string))
                    return false;
            }
        }
        default:
        return false;
    }
    //除FILE_REQUEST状态外，其余状态只申请一个iovec，指向响应报文缓冲区
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

void http_conn::process()
{
    HTTP_CODE read_ret = process_read();
    // NO_REQUEST，表示请求不完整，需要继续接收请求数据
    if(read_ret == NO_REQUEST)
    {   
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return;
    }
    bool write_ret = process_write(read_ret);
    if(!write_ret){
        close_conn();
    }
    // 在子线程完成响应报文后，最后注册写事件， 以便主线程能够检测到写事件， 并向浏览器端发送数据
    modfd(m_epollfd, m_sockfd, EPOLLOUT);

}


