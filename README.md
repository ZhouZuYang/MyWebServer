# MyWebServer



## 项目概况：

本项目是**Linux** 下基于**C++** 的轻量级服务器，具体有以下功能:

1. 并发模型采用**线程池** + **epoll** + 事件处理(**模拟proactor**)
2. 使用**状态机**解析HTTP请求报文， 支持解析**GET** 和 **POST** 请求
3. 通过数据库可以实现Web 端**注册和登录**的功能，并且请求服务器的图片和视频文件
4. 通过**定时器**删除非活动的连接
5. 经过**Webbench**压力测试可以实现**接近上万**的并发连接数据交换



## 系统环境

Unbuntu 18.04

g++ 7.5.0



## 框架

![image](https://github.com/liushuai839/MyWebServer/blob/main/1.png)





## 压力测试

当并发量为10000的时候，没有失败的连接

![image](https://github.com/liushuai839/MyWebServer/blob/main/image-20210521204148302.png)









## 致谢

Linux 高性能服务器编程，游双著.

参考：https://github.com/qinguoyi/TinyWebServer
