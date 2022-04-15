#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <assert.h>
#include <sys/epoll.h>
#include <string.h>
#include <stdbool.h>

#include "./lock/locker.h"
#include "./threadpool/threadpool.h"
#include "./http/http_conn.h"
#include "./CGImysql/sql_connection_pool.h"
#include "./threadpool/threadpool.h"
#include "./timer/lst_timer.h"


#define MAX_EVENT_NUMBER 10000
#define MAX_FD  65536
#define TIMESHOT 5


// 设置定时器相关参数
static sort_timer_lst timer_lst;


//这三个函数在http_conn.cpp中定义，改变链接属性
extern int addfd(int epollfd, int fd, bool one_shot);
extern int remove(int epollfd, int fd);
extern int setnonblocking(int fd);

static int pipefd[2];
static int epollfd = 0;



void sig_handler(int sig)
{
    //为保证函数的可重入性，保留原来的errno
    int save_errno = errno;
    int msg = sig;
    // 将信号值从管道写端写入,传输字符类型,而非整型
    send(pipefd[1], (char *)&msg, 1, 0);
    errno = save_errno;
}




// 设置信号函数
void addsig(int sig, void(handler)(int), bool restart = true)
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    if(restart)
        sa.sa_flags |= SA_RESTART;
    // 将所有信号添加到信号集中
    sigfillset(&sa.sa_mask);
    // 设置信号捕捉函数
    assert(sigaction(sig, &sa, NULL) != -1);
}



// 定时处理任务，重新定时以不断触发SIGNAL信号
void timer_handler()
{
    timer_lst.tick();
    alarm(TIMESHOT);
}


// 定时器回调函数， 删除非活动连接在socket 上的注册事件
void cb_func(client_data* user_data)
{   
    // 删除非活动连接在socket 上的注册事件
    epoll_ctl(epollfd, EPOLL_CTL_DEL,user_data->sockfd, 0);
    // 关闭文件描述符
    close(user_data->sockfd);
    // 减少连接数
    http_conn::m_user_count--;
}



int main(int argc, char* argv[])
{

    if(argc <= 1)
    {
        printf("usage: %s ip_address port number\n", basename(argv[0]));
        return 1;
    }
    int port = atoi(argv[1]);



    // 创建数据库连接池
    connection_pool *connPool = connection_pool::GetInstance();
    connPool->init("localhost", "root", "root", "test_server", 3306, 8);

    // 创建线程池
    threadpool<http_conn> *pool = NULL;
    try{
        pool = new threadpool<http_conn>(connPool);
    }
    catch(...)
    {
        return 1;
    }


    // 创建 http 对象数组
    http_conn *users = new http_conn[MAX_FD];
    assert(users);

    // 初始化数据库连接
    users->initmysql_result(connPool);
    

    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd >= 0);

    int ret = 0;
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    // 设置端口复用
    int flag = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));

    ret = bind(listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);
    ret = listen(listenfd, 5);
    assert(ret >= 0);

    // 创建内核事件表
    epoll_event  events[MAX_EVENT_NUMBER];
    epollfd = epoll_create(5);
    assert(epollfd != -1);

    // 添加监听fd
    addfd(epollfd, listenfd, false);
    http_conn::m_epollfd = epollfd;
    //创建管道 //! 利用本地套接字进行通信
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);

    // 设置管道写端为非阻塞: 如果写端导致缓冲区满了,则会阻塞,会进一步增加信号处理函数的执行时间
    setnonblocking(pipefd[1]);
    // 设置管道读端为ET非阻塞,把管道添加到监听树上
    addfd(epollfd, pipefd[0], false);
    // 定时器的信号，注册了信号捕捉函数，当捕捉到信号时，会执行相应的回调函数，
    // 回调函数会将信号值发送给主进程， 主进程利用epoll监听到信号值的时候，就会执行主进程相应的函数
    addsig(SIGALRM, sig_handler, false);  
    addsig(SIGTERM, sig_handler, false);  //进程终止信号(原理同上)

    client_data* users_timer = new client_data[MAX_FD];

    bool timeout = false;
    alarm(TIMESHOT); // 设置定时单位， 每个TIMESHOT触发SIGNAL信号
    bool stop_server = false;


    while(!stop_server)
    {   
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if(number < 0 && errno != EINTR)
        {
            // 少一句log_error
            break;
        }
        
        for(int i = 0; i < number; ++i)
        {
            int sockfd = events[i].data.fd;
            if(sockfd == listenfd)
            {   
                
                struct sockaddr_in client_addr;
                socklen_t client_length = sizeof(client_addr);
               
                int connfd = accept(listenfd, (struct sockaddr*)&client_addr, &client_length);
               
                users[connfd].init(connfd, client_addr);
                if(connfd < 0)
                {   
                    // log_error
                    continue;
                }
                // 创建定时器， 设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
                users_timer[connfd].address = client_addr;
                users_timer[connfd].sockfd = connfd;
                // 创建定时器临时变量
                util_timer *timer = new util_timer;
                // 设置定时器对应的资源
                timer->user_data = &users_timer[connfd];
                // 设置回调函数
                timer->cb_func = cb_func;
                // 获取当前时间
                time_t cur = time(NULL);
                // 设置超时时间
                timer->expire = cur + 3 * TIMESHOT;
                users_timer[connfd].timer = timer;
                // 将定时器添加到链表容器当中
                timer_lst.add_timer(timer);
            }
            

            //处理信号
            //  管道读端对应文件描述符发生读事件
            else if ((sockfd == pipefd[0]) && (events[i].events & EPOLLIN))
            {
                int sig;
                char signals[1024];
                
                // 从管道读端读出信号值,成功返回字节数,失败返回-1 
                // 正常情况下，这里的ret值是1，只有14和15两个ASCII码对应的字符
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if (ret == -1)
                {
                    continue;
                }
                else if (ret == 0)
                {
                    continue;
                }
                else
                {
                    for (int i = 0; i < ret; ++i)
                    {   
                        // 处理信号值对应的逻辑,只考虑SIGALRM 和 SIGTERM
                        switch (signals[i])
                        {
                        case SIGALRM:
                        {   
                            // 处理定时信号,定时标志设置为true
                            timeout = true;
                            break;
                        }
                        case SIGTERM:
                        {
                            stop_server = true;
                        }
                        }
                    }
                }
            }


            // 读浏览器来的数据
            else if(events[i].events & EPOLLIN)
            {      
                
                util_timer *timer = users_timer[sockfd].timer;
                if(users[sockfd].read_once()){
                    
                    // users 是 http 类的一个数组， users + sockfd 表示数组中第 sockfd 个元素
                    pool->append(users + sockfd); 

                    // 若有数据传输， 则将定时器往后延迟3个单位时间
                    // 并对新的定时器在链表中的位置进行调整
                    if(timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESHOT;
                        timer_lst.adjust_timer(timer);
                    }
                }
                // 数据没有读完
                else{
                    // 执行定时事件
                    timer->cb_func(&users_timer[sockfd]);
                    if(timer)
                    {
                        timer_lst.del_timer(timer);
                    }
                }

            }
            // 有写事件发生
            else if(events[i].events & EPOLLOUT){
                util_timer *timer = users_timer[sockfd].timer;
                if(users[sockfd].write()){
                    
                    // 若有数据传输， 则将定时器往后延迟3个单位时间
                    // 并对新的定时器在链表中的位置进行调整
                    if(timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESHOT;
                        timer_lst.adjust_timer(timer);
                    }
                }
                else{
                    // 执行定时事件
                    timer->cb_func(&users_timer[sockfd]);
                    if(timer)
                    {
                        timer_lst.del_timer(timer);
                    }
                }
            }
        }
        if(timeout)
        {
            timer_handler();
            timeout = false;
        }
    }
}

