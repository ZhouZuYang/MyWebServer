#include <mysql/mysql.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <stdlib.h>
#include <list>
#include <pthread.h>
#include <iostream>
#include "sql_connection_pool.h"

using namespace std;


connection_pool::connection_pool()
{
	this->CurConn = 0;
	this->FreeConn = 0;
}

connection_pool *connection_pool::GetInstance()
{
	static connection_pool connPool;
	return &connPool;
}

//! 初始化连接池
void connection_pool::init(string url, string User, string PassWord, string DBName, int Port, unsigned int MaxConn)
{	
	//! 初始化数据库信息
	this->url = url;
	this->Port = Port;
	this->User = User;
	this->PassWord = PassWord;
	this->DatabaseName = DBName;

	lock.lock();
	//! 创建MaxConn条数据库连接
	for (int i = 0; i < MaxConn; i++)
	{
		MYSQL *con = NULL;
		con = mysql_init(con);

		if (con == NULL)
		{
			cout << "Error:" << mysql_error(con);
			exit(1);
		}
		con = mysql_real_connect(con, url.c_str(), User.c_str(), PassWord.c_str(), DBName.c_str(), Port, NULL, 0);

		if (con == NULL)
		{
			cout << "Error: " << mysql_error(con);
			exit(1);
		}
		//! 更新连接池和空闲链接数量
		connList.push_back(con);
		++FreeConn;
	}

	//! 将信号量初始化为最大连接次数(创建一个信号量的类)
	reserve = sem(FreeConn);

	this->MaxConn = FreeConn;
	
	lock.unlock();
}


//当有请求时，从数据库连接池中返回一个可用连接，更新使用和空闲连接数
MYSQL *connection_pool::GetConnection()
{
	MYSQL *con = NULL;

	if (0 == connList.size())
		return NULL;
	//! 取出连接,信号量原子减一(实际为加锁),为0则等待
	//! 如果在使用一个数据库连接的时候,需要使用另一个数据库链接,
	//! 则继续将这个信号量减1,为0的时候再阻塞, 这也是信号量与互斥锁的区别
	reserve.wait();
	
	lock.lock();

	//! 取出连接池的头一个
	con = connList.front();
	connList.pop_front();

	--FreeConn;
	++CurConn;

	lock.unlock();
	return con;
}

//释放当前使用的连接
bool connection_pool::ReleaseConnection(MYSQL *con)
{
	if (NULL == con)
		return false;

	lock.lock();

	connList.push_back(con);
	++FreeConn;
	--CurConn;

	lock.unlock();

	//!  释放链接原子加1(解锁)
	reserve.post();
	return true;
}

//销毁数据库连接池
void connection_pool::DestroyPool()
{

	lock.lock();
	if (connList.size() > 0)
	{
		list<MYSQL *>::iterator it;

		//! 通过迭代器遍历,关闭数据库连接
		for (it = connList.begin(); it != connList.end(); ++it)
		{
			MYSQL *con = *it;
			mysql_close(con);
		}
		CurConn = 0;
		FreeConn = 0;

		//! 清空list
		connList.clear();

		lock.unlock();
	}

	lock.unlock();
}

//当前空闲的连接数
int connection_pool::GetFreeConn()
{
	return this->FreeConn;
}

connection_pool::~connection_pool()
{
	DestroyPool();
}

//! 数据库连接本身是指针类型,所以需要双指针才能对其进行修改
/*
* RALL 方式管理申请和释放资源的类:
* 对象创建时,执行 申请资源 的动作
* 对象析构时,执行 释放资源 的操作
* 禁止对象拷贝和赋值
*/
//! RALL机制便是通过利用对象(通过类构造的实例)的自动销毁，使得资源也具有了生命周期，有了自动销毁（自动回收）的功能。
ConnectionRaII::ConnectionRaII(MYSQL **SQL, connection_pool *connPool){
	*SQL = connPool->GetConnection();
	
	conRAII = *SQL;
	poolRAII = connPool;
}

ConnectionRaII::~ConnectionRaII(){
	poolRAII->ReleaseConnection(conRAII);
}