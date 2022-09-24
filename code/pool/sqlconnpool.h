
#ifndef SQLCONNPOOL_H
#define SQLCONNPOOL_H

#include <mysql/mysql.h>
#include <string>
#include <queue>
#include <mutex>
#include <semaphore.h>
#include <thread>
#include "../log/log.h"

class SqlConnPool {
public:
    static SqlConnPool *Instance();

    MYSQL *GetConn();//获取一个连接
    void FreeConn(MYSQL * conn);//释放一个连接，放到线程池中
    int GetFreeConnCount();

    void Init(const char* host, int port,//主机名 端口
              const char* user,const char* pwd, //用户名 密码
              const char* dbName, int connSize);//数据库名 连接的数量
    void ClosePool();

private:
    SqlConnPool();
    ~SqlConnPool();

    int MAX_CONN_;//连接数据库的最大连接数
    int useCount_;//当前用户数
    int freeCount_;//空闲用户数

    std::queue<MYSQL *> connQue_;//队列（MYSQL *）
    std::mutex mtx_;//互斥锁
    sem_t semId_;//信号量
};


#endif // SQLCONNPOOL_H