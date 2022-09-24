
#include "httpconn.h"
using namespace std;

const char* HttpConn::srcDir;
std::atomic<int> HttpConn::userCount;
bool HttpConn::isET;

HttpConn::HttpConn() { 
    fd_ = -1;
    addr_ = { 0 };
    isClose_ = true;
};

HttpConn::~HttpConn() { 
    Close(); 
};

void HttpConn::init(int fd, const sockaddr_in& addr) {
    assert(fd > 0);
    userCount++;
    addr_ = addr;
    fd_ = fd;
    writeBuff_.RetrieveAll();
    readBuff_.RetrieveAll();
    isClose_ = false;
    LOG_INFO("Client[%d](%s:%d) in, userCount:%d", fd_, GetIP(), GetPort(), (int)userCount);
}

void HttpConn::Close() {
    response_.UnmapFile();
    if(isClose_ == false){
        isClose_ = true; 
        userCount--;
        close(fd_);
        LOG_INFO("Client[%d](%s:%d) quit, UserCount:%d", fd_, GetIP(), GetPort(), (int)userCount);
    }
}

int HttpConn::GetFd() const {
    return fd_;
};

struct sockaddr_in HttpConn::GetAddr() const {
    return addr_;
}

const char* HttpConn::GetIP() const {
    return inet_ntoa(addr_.sin_addr);
}

int HttpConn::GetPort() const {
    return addr_.sin_port;
}

ssize_t HttpConn::read(int* saveErrno) {
    ssize_t len = -1;
    do {
        len = readBuff_.ReadFd(fd_, saveErrno);//将读到的对象封装到buffer中
        if (len <= 0) {
            break;
        }
    } while (isET);//ET循环读出，否则不提醒
    return len;
}

ssize_t HttpConn::write(int* saveErrno) {
    ssize_t len = -1;
    do {
        len = writev(fd_, iov_, iovCnt_);//分散写
        //len为写出的字节数长度
        if(len <= 0) {
            *saveErrno = errno;
            break;
        }
        if(iov_[0].iov_len + iov_[1].iov_len  == 0) { break; } /* 传输结束 */
        else if(static_cast<size_t>(len) > iov_[0].iov_len) {//第一各内存中的写完了，然后重新调整内存1、2的读写位置
            iov_[1].iov_base = (uint8_t*) iov_[1].iov_base + (len - iov_[0].iov_len);//重新定位内存块2的起始位置
            iov_[1].iov_len -= (len - iov_[0].iov_len);//修改长度
            if(iov_[0].iov_len) {//重置内存块0的读写位置，因为1已经写完了
                writeBuff_.RetrieveAll();
                iov_[0].iov_len = 0;
            }
        }
        else {//写到了第一个内存块
            iov_[0].iov_base = (uint8_t*)iov_[0].iov_base + len; //重新定位内存块1的起始位置
            iov_[0].iov_len -= len; //修改剩余要写的长度
            writeBuff_.Retrieve(len);//剩余内存块1中要写的位置
        }
    } while(isET || ToWrite=Bytes() > 10240);
    return len;
}

bool HttpConn::process() {
    request_.Init();
    if(readBuff_.ReadableBytes() <= 0) {//判断是否有对象可读
        return false;
    }
    else if(request_.parse(readBuff_)) {//解析buf内容，buf中存放的是http请求报文
        LOG_DEBUG("%s", request_.path().c_str());
        //解析成功后
        response_.Init(srcDir, request_.path(), request_.IsKeepAlive(), 200);
    } else {
        response_.Init(srcDir, request_.path(), false, 400);
    }

    response_.MakeResponse(writeBuff_);//将响应头、相应行、空格添加到了writeBuff_中，请问的文件映射到了内存中
    /* 响应头 */
    //分散写，先写writeBuff_中的响应头、响应行、空格等
    iov_[0].iov_base = const_cast<char*>(writeBuff_.Peek());
    iov_[0].iov_len = writeBuff_.ReadableBytes();
    iovCnt_ = 1;

    /* 文件 */
    //再把映射的内存中的文件分散写入
    if(response_.FileLen() > 0  && response_.File()) {
        iov_[1].iov_base = response_.File();
        iov_[1].iov_len = response_.FileLen();
        iovCnt_ = 2;
    }
    LOG_DEBUG("filesize:%d, %d  to %d", response_.FileLen() , iovCnt_, ToWriteBytes());
    return true;
}
