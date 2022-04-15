#ifndef _CONNECTION_POOL
#define _CONNECTION_POOL

#include <stdio.h>
#include <list>
#include <mysql/mysql.h>
#include <error.h>
#include <string.h>
#include <iostream>
#include <string>
#include "../lock/locker.h"

using namespace std;

class connection_pool
{
public:
    MYSQL* GetConnection();      // 获取数据库连接
    bool ReleaseConnection(MYSQL *con);    // 释放连接
    int GetFreeConn();           // 获取连接
    void DestroyPool();          // 销毁所有连接

    // 单例模式
    static connection_pool *GetInstance();

    void init(string url, string User, string PassWord, string DataBaseName, int Port, unsigned int MaxConn); 
	
	connection_pool(); //! 不应该是私有化构造函数,以防外界创建单例类的对象
	~connection_pool();

private:
    unsigned int MaxConn;    // 最大连接数
    unsigned int CurConn;    // 目前已使用的连接数
    unsigned int FreeConn;   // 当前空闲的连接数

private:
    locker lock;
    list<MYSQL *> connList;
    sem reserve;

private:
    string url;          // 主机地址
    string Port;         // 端口号
    string User;         // 登录数据库用户名
    string PassWord;     // 登录数据库密码
    string DatabaseName; // 使用数据库名
};

class ConnectionRaII
{
public:
    ConnectionRaII(MYSQL **con, connection_pool *connPool);
    ~ConnectionRaII();

private:
    MYSQL *conRAII;
    connection_pool *poolRAII;
};






#endif