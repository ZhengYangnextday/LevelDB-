# Status设计
## 成员变量
```C++ {.line-numbers}
// OK status has a null state_.  Otherwise, state_ is a new[] array
// of the following form:
//    state_[0..3] == length of message
//    state_[4]    == code
//    state_[5..]  == message
const char* state_
```
一个提示当前状态的信息串，由以下内容组成:

- 消息全长(4B)
- 状态码(1B)
- 消息

如果状态码为0, 则认为无错误, OK, state_ = nullptr;  反之，给出具体错误内容。  
`status.cc`中的主要构造函数：
```C++
Status::Status(Code code, const Slice& msg, const Slice& msg2) {
  assert(code != kOk);
  const uint32_t len1 = static_cast<uint32_t>(msg.size());
  const uint32_t len2 = static_cast<uint32_t>(msg2.size());
    //                                  可以发现这里size多了2
  const uint32_t size = len1 + (len2 ? (2 + len2) : 0);
  char* result = new char[size + 5];
  std::memcpy(result, &size, sizeof(size));
  result[4] = static_cast<char>(code);
  std::memcpy(result + 5, msg.data(), len1);
  if (len2) {
    // 这里是多出的两位字符
    result[5 + len1] = ':';
    result[6 + len1] = ' ';
    std::memcpy(result + 7 + len1, msg2.data(), len2);
  }
  state_ = result;
}
```
比较直接，没啥好说的。下面还附带了个ToString函数，只是翻译了一下文本串，没啥特殊的。

但是在各个不同的地方有不同的应用，例如`env.h`中就对`status`进行了更多成员的定义，包括各类操作。

