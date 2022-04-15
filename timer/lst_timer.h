#ifndef LST_TIMER
#define LST_TIMER
#include <time.h>
class util_timer;

struct client_data
{
    // 客户端 socket 地址
    sockaddr_in address;
    // socket 文件描述符
    int sockfd;
    // 定时器
    util_timer* timer;
};

// 定时器类
class util_timer
{
public:
    util_timer(): prev(nullptr), next(nullptr) {}

public:
    // 超时时间   
    time_t expire;
    void (*cb_func)(client_data *); // 泛型指针，第一个括号表示函数名， 第二个括号表示参数列表
    // 连接资源
    client_data *user_data;
    // 前向定时器
    util_timer* prev;
    // 后向定时器
    util_timer* next;
};

// 定时器容器类
class sort_timer_lst
{
private:
    util_timer* head;
    util_timer* tail;

public:
    sort_timer_lst() : head(nullptr), tail(nullptr) {}
    // 释放整个链表的空间
    ~sort_timer_lst() {
        util_timer *tmp = head;
        while(tmp)
        {
            head = tmp->next;
            delete tmp;
            tmp = head;
        }
    }

    void add_timer(util_timer* timer)
    {
        if(!timer)
        {
            return;
        }
        if(!head)
        {
            head = tail = timer;
            return;
        }

        if(timer->expire < head->expire)
        {
            timer->next = head;
            head->prev = timer;
            head = timer;
            return;
        }
        add_timer(timer, head);
    }

    // 调整定时器的位置， 一般是某个定时器的超时时间延长后， 需要重新调整其位置
    void adjust_timer(util_timer* timer)
    {
        if(!timer){
            return;
        }
        util_timer *tmp = timer->next;
        // 被调整的定时器在尾部
        // 或者 定时器超时值小于下一个定时器的超时值， 不调整
        if(!tmp || (timer->expire < tmp->expire))
        {
            return;
        }
        // 被调整的节点是头结点， 重新插入
        if(timer == head)
        {
            head = head->next;
            head->prev = nullptr;
            timer->next = nullptr;
            add_timer(timer, head);
        }
        else{  // 否则将定时器取出， 重新插入
            timer->prev->next = timer->next;
            timer->next->prev = timer->prev;
            add_timer(timer, timer->next);
        }
    }

    // 删除定时器
    void del_timer(util_timer* timer)
    {
        if(!timer)
        {
            return;
        }
        if((timer == head) && (timer == tail))
        {
            delete timer;
            head = nullptr;
            tail = nullptr;
            return; 
        }
        if(timer == head)
        {
            head = head->next;
            head->prev = nullptr;
            delete timer;
            return;
        }
        if(timer == tail)
        {
            tail = tail->prev;
            tail->next = nullptr;
            delete timer;
        }
        // 需要删除的链表节点在中间
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        delete timer;
    }

    // 定时任务处理函数
    void tick(){
        if(!head)
            return;
        // 获取当前时间
        
        time_t cur = time(NULL);
        util_timer* tmp = head;

        while(tmp)
        {   
            // 当前时间小于链表头部定时器的超时时间， 说明没有定时器超时， 直接跳出
            if(cur < tmp->expire)
            {
                break;
            }
            // 当前定时器超时，调用回调函数，执行定时事件
            tmp->cb_func(tmp->user_data);
            
            // 将处理后的定时器从链表容器中删除，并且重置头结点
            head = tmp->next;
            if(head){
                head->prev = nullptr;
            } 
            delete tmp;
            tmp = head;
        }
    }

private:
    void add_timer(util_timer* timer, util_timer* lst_head)
    {
        util_timer *prev = lst_head;
        util_timer* tmp = prev->next;

        // 遍历头结点后的所有节点
        while(tmp)
        {   
            // 找到对应的位置， 将其插入tmp节点的前面
            if(timer->expire < tmp->expire)
            {
                prev->next = timer;
                timer->next = tmp;
                tmp->prev = timer;
                timer->next = prev;
                break;
            }
            prev = tmp;
            tmp = tmp->next;
        }
        // 遍历完链表发现，目标定时器应该需要插入到链表尾节点处
        if(!tmp)
        {
            prev->next = timer;
            timer->prev = prev;
            timer->next = nullptr;
            tail = timer;
        }
    }
};









#endif