
#include "buffer.h"

Buffer::Buffer(int initBuffSize) : buffer_(initBuffSize), readPos_(0), writePos_(0) {}

size_t Buffer::ReadableBytes() const {//可以读的字节数
    return writePos_ - readPos_;
}
size_t Buffer::WritableBytes() const {//可以写的字节数
    return buffer_.size() - writePos_;
}

size_t Buffer::PrependableBytes() const {//前面可写的空间，指read完的数据位置可以用来重新写
    return readPos_;
}

const char* Buffer::Peek() const {
    return BeginPtr_() + readPos_;
}

void Buffer::Retrieve(size_t len) {
    assert(len <= ReadableBytes());
    readPos_ += len;
}

void Buffer::RetrieveUntil(const char* end) {
    assert(Peek() <= end );
    Retrieve(end - Peek());
}

void Buffer::RetrieveAll() {
    bzero(&buffer_[0], buffer_.size());
    readPos_ = 0;
    writePos_ = 0;
}

std::string Buffer::RetrieveAllToStr() {
    std::string str(Peek(), ReadableBytes());
    RetrieveAll();
    return str;
}

const char* Buffer::BeginWriteConst() const {
    return BeginPtr_() + writePos_;
}

char* Buffer::BeginWrite() {
    return BeginPtr_() + writePos_;
}

void Buffer::HasWritten(size_t len) {
    writePos_ += len;
} 

void Buffer::Append(const std::string& str) {
    Append(str.data(), str.length());
}

void Buffer::Append(const void* data, size_t len) {
    assert(data);
    Append(static_cast<const char*>(data), len);
}

void Buffer::Append(const char* str, size_t len) {//扩充buffer的大小
    assert(str);
    EnsureWriteable(len);
    std::copy(str, str + len, BeginWrite());
    HasWritten(len);
}

void Buffer::Append(const Buffer& buff) {
    Append(buff.Peek(), buff.ReadableBytes());
}

void Buffer::EnsureWriteable(size_t len) {
    if(WritableBytes() < len) {//可写空间比长度要小
        MakeSpace_(len);    //然后扩容
    }
    assert(WritableBytes() >= len);
}

ssize_t Buffer::ReadFd(int fd, int* saveErrno) {
    char buff[65535];  //临时数据，保证能够把所有的数据全都读出来
    struct iovec iov[2];
    const size_t writable = WritableBytes();//可写的组字节数

    /* 分散读， 保证数据全部读完 */    //先往对象里面定义的readbuf里面读，读满了再向buff[65535]里面读
    iov[0].iov_base = BeginPtr_() + writePos_;//内存从哪读
    iov[0].iov_len = writable;//剩余可写空间的大小
    iov[1].iov_base = buff;
    iov[1].iov_len = sizeof(buff);

    const ssize_t len = readv(fd, iov, 2);
    if(len < 0) {
        *saveErrno = errno;
    }
    else if(static_cast<size_t>(len) <= writable) {
        writePos_ += len;  //移动可写位置指针
    }
    else {
        writePos_ = buffer_.size();//可写位置移动到最后，表示已经满了
        Append(buff, len - writable);
    }
    return len;
}

ssize_t Buffer::WriteFd(int fd, int* saveErrno) {
    size_t readSize = ReadableBytes();
    ssize_t len = write(fd, Peek(), readSize);
    if(len < 0) {
        *saveErrno = errno;
        return len;
    } 
    readPos_ += len;
    return len;
}

char* Buffer::BeginPtr_() {
    return &*buffer_.begin();
}

const char* Buffer::BeginPtr_() const {
    return &*buffer_.begin();
}

void Buffer::MakeSpace_(size_t len) {
    if(WritableBytes() + PrependableBytes() < len) {//后面的可写空间+前面已经read完的空间之和小于len的长度，调整大小
        buffer_.resize(writePos_ + len + 1);//重新调整buffer_的大小
    } 
    else {//如果大于len的长度，说明剩余空间可以将len装下去
        size_t readable = ReadableBytes();//将read之后，write之前的数据从头开始拷贝。将readpos和writepos的位置重新定位
        std::copy(BeginPtr_() + readPos_, BeginPtr_() + writePos_, BeginPtr_());
        readPos_ = 0;
        writePos_ = readPos_ + readable;
        assert(readable == ReadableBytes());
    }
}