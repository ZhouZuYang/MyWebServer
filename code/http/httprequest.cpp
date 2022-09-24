
#include "httprequest.h"
using namespace std;

const unordered_set<string> HttpRequest::DEFAULT_HTML{
            "/index", "/register", "/login",
             "/welcome", "/video", "/picture", };

const unordered_map<string, int> HttpRequest::DEFAULT_HTML_TAG {
            {"/register.html", 0}, {"/login.html", 1},  };

void HttpRequest::Init() {
    method_ = path_ = version_ = body_ = "";
    state_ = REQUEST_LINE;
    header_.clear();
    post_.clear();
}

bool HttpRequest::IsKeepAlive() const {
    if(header_.count("Connection") == 1) {
        return header_.find("Connection")->second == "keep-alive" && version_ == "1.1";
    }
    return false;
}

bool HttpRequest::parse(Buffer& buff) {//http的请求数据在传入的buff中
    const char CRLF[] = "\r\n";
    if(buff.ReadableBytes() <= 0) {//判断请求数据是否大于0
        return false;
    }
    while(buff.ReadableBytes() && state_ != FINISH) {//有可读字节并且状态不为finsh（初始状态为REQUEST_LINE）
        //根据\r\n的结束标志获取一行数据，lineend为这一行结束的位置
        const char* lineEnd = search(buff.Peek(), buff.BeginWriteConst(), CRLF, CRLF + 2);
        std::string line(buff.Peek(), lineEnd);//line为获取的一行的信息
        switch(state_)
        {
        case REQUEST_LINE:
            if(!ParseRequestLine_(line)) {//解析请求行，如果成功的话，就获取到了http请求行中的路径，再执行ParsePath_
                return false;
            }
            ParsePath_();//state的状态进行转换
            break;    
        case HEADERS:
            ParseHeader_(line);
            if(buff.ReadableBytes() <= 2) {
                state_ = FINISH;
            }
            break;
        case BODY:
            ParseBody_(line);
            break;
        default:
            break;
        }
        if(lineEnd == buff.BeginWrite()) { break; }//说明读取的位置到了写指针的位置，说明读完了
        buff.RetrieveUntil(lineEnd + 2);//移动读指针的位置
    }
    LOG_DEBUG("[%s], [%s], [%s]", method_.c_str(), path_.c_str(), version_.c_str());
    return true;
}

void HttpRequest::ParsePath_() {
    if(path_ == "/") {
        path_ = "/index.html"; 
    }
    else {
        for(auto &item: DEFAULT_HTML) {
            if(item == path_) {
                path_ += ".html";
                break;
            }
        }
    }
}

bool HttpRequest::ParseRequestLine_(const string& line) {
    //使用正则表达式匹配请求行，获取到的方式，路径、http版本等存放在method_、path_、version_中，并把状态改为请求头
    regex patten("^([^ ]*) ([^ ]*) HTTP/([^ ]*)$");
    smatch subMatch;
    if(regex_match(line, subMatch, patten)) {   
        method_ = subMatch[1];
        path_ = subMatch[2];
        version_ = subMatch[3];
        state_ = HEADERS;
        return true;
    }
    LOG_ERROR("RequestLine Error");
    return false;
}

void HttpRequest::ParseHeader_(const string& line) {
    regex patten("^([^:]*): ?(.*)$");//请求头都是键值对的形式
    smatch subMatch;
    if(regex_match(line, subMatch, patten)) {//使用请求头进行匹配，当到请求头的最后一行空格的时候不匹配，state编程请求请求体
        header_[subMatch[1]] = subMatch[2];//header_是一个unordered_map,将请求头的信息进行存储
    }
    else {
        state_ = BODY;
    }
}

void HttpRequest::ParseBody_(const string& line) {//将请求体的内容赋值给body，状态编程finish
    body_ = line;
    ParsePost_();
    state_ = FINISH;
    LOG_DEBUG("Body:%s, len:%d", line.c_str(), line.size());
}

int HttpRequest::ConverHex(char ch) {
    if(ch >= 'A' && ch <= 'F') return ch -'A' + 10;
    if(ch >= 'a' && ch <= 'f') return ch -'a' + 10;
    return ch;
}

void HttpRequest::ParsePost_() {
    //application/x-www-form-urlencoded： 数据被编码为名称/值对。这是标准的编码格式。
    if(method_ == "POST" && header_["Content-Type"] == "application/x-www-form-urlencoded") {
        //解析表单信息
        ParseFromUrlencoded_();//结果已经以键值对的形式存储在了body_中
        if(DEFAULT_HTML_TAG.count(path_)) {//如果找到了，说明就是注册或者是登录的页面
            int tag = DEFAULT_HTML_TAG.find(path_)->second;//0或者1
            LOG_DEBUG("Tag:%d", tag);
            if(tag == 0 || tag == 1) {
                bool isLogin = (tag == 1);
                if(UserVerify(post_["username"], post_["password"], isLogin)) {
                    path_ = "/welcome.html";
                } 
                else {
                    path_ = "/error.html";
                }
            }
        }
    }   
}
//# Form Data
//name=name&age=11
//post请求体的数据全放在post_这个unordered_map中了
void HttpRequest::ParseFromUrlencoded_() {
    if(body_.size() == 0) { return; }

    string key, value;
    int num = 0;
    int n = body_.size();//整个请求体一行的那个string大小
    int i = 0, j = 0;

    for(; i < n; i++) {
        //从请求体里挨个遍历，遍历到=时，把前面的取出来当作key，遍历到&把前面的取出来当作value
        char ch = body_[i];
        switch (ch) {
        case '=':
            key = body_.substr(j, i - j);
            j = i + 1;
            break;
        case '+':
            body_[i] = ' ';
            break;
        case '%':
            //简单加密操作，中文注册时会出现%
            num = ConverHex(body_[i + 1]) * 16 + ConverHex(body_[i + 2]);
            body_[i + 2] = num % 10 + '0';
            body_[i + 1] = num / 10 + '0';
            i += 2;
            break;
        case '&':
            value = body_.substr(j, i - j);
            j = i + 1;
            post_[key] = value;
            LOG_DEBUG("%s = %s", key.c_str(), value.c_str());
            break;
        default:
            break;
        }
    }
    assert(j <= i);
    if(post_.count(key) == 0 && j < i) {
        value = body_.substr(j, i - j);
        post_[key] = value;
    }
}

bool HttpRequest::UserVerify(const string &name, const string &pwd, bool isLogin) {//isLogin是否是登录
    if(name == "" || pwd == "") { return false; }
    LOG_INFO("Verify name:%s pwd:%s", name.c_str(), pwd.c_str());
    MYSQL* sql;
    SqlConnRAII(&sql,  SqlConnPool::Instance());
    assert(sql);
    
    bool flag = false;
    unsigned int j = 0;
    char order[256] = { 0 };
    MYSQL_FIELD *fields = nullptr;//字段列数组
    MYSQL_RES *res = nullptr;///这个结构代表返回行的一个查询结果集
    
    if(!isLogin) { flag = true; }//不是登录，flag = true，为下面的注册
    /* 查询用户及密码 */
    snprintf(order, 256, "SELECT username, password FROM user WHERE username='%s' LIMIT 1", name.c_str());
    LOG_DEBUG("%s", order);

    if(mysql_query(sql, order)) { 
        mysql_free_result(res);//成功释放结果
        return false; 
    }
    res = mysql_store_result(sql);//保存结果
    j = mysql_num_fields(res);//获取字段
    fields = mysql_fetch_fields(res);//提取字段

    while(MYSQL_ROW row = mysql_fetch_row(res)) {//提取行
        LOG_DEBUG("MYSQL ROW: %s %s", row[0], row[1]);
        string password(row[1]);
        /* 注册行为 且 用户名未被使用*/
        if(isLogin) {
            if(pwd == password) { flag = true; }
            else {
                flag = false;
                LOG_DEBUG("pwd error!");
            }
        } 
        else { 
            flag = false; 
            LOG_DEBUG("user used!");
        }
    }
    mysql_free_result(res);

    /* 注册行为 且 用户名未被使用*/
    if(!isLogin && flag == true) {
        LOG_DEBUG("regirster!");
        bzero(order, 256);
        snprintf(order, 256,"INSERT INTO user(username, password) VALUES('%s','%s')", name.c_str(), pwd.c_str());
        LOG_DEBUG( "%s", order);
        if(mysql_query(sql, order)) { 
            LOG_DEBUG( "Insert error!");
            flag = false; 
        }
        flag = true;
    }
    SqlConnPool::Instance()->FreeConn(sql);
    LOG_DEBUG( "UserVerify success!!");
    return flag;
}

std::string HttpRequest::path() const{
    return path_;
}

std::string& HttpRequest::path(){
    return path_;
}
std::string HttpRequest::method() const {
    return method_;
}

std::string HttpRequest::version() const {
    return version_;
}

std::string HttpRequest::GetPost(const std::string& key) const {
    assert(key != "");
    if(post_.count(key) == 1) {
        return post_.find(key)->second;
    }
    return "";
}

std::string HttpRequest::GetPost(const char* key) const {
    assert(key != nullptr);
    if(post_.count(key) == 1) {
        return post_.find(key)->second;
    }
    return "";
}