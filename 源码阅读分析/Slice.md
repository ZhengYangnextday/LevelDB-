# Slice(片)结构
## 源码解读
Slice作为片，作用或者使用方法类似于C++中String类，为LevelDB中最基础类型之一。接下来将从下至上逐步分析S
### 类成员定义
``` C++
private:
    const char* data_;
    size_t size_;
```
此处data_采用的`const char*`类型，一方面说明data_对应的数据不可更改，另一方面Slice类存储的仅为指针，节省了存储开销。要注意的是，Slice并不存在析构函数，在销毁时并不会对data_指向的数据进行操作。同时，在代码注释中也提示到当Slice对应的外部存储空间被释放时，需确保该Slice不再被使用，否则会引发错误。简单来说，就是要始终注意Slice中data_地址的有效性。  
`size_t size_`用以记录数据长度，所有数据大小均为字节的整数倍。
***
### Slice类初始化
```　C++
Slice() : data_(""), size_(0) {}

// Create a slice that refers to d[0,n-1].
Slice(const char* d, size_t n) : data_(d), size_(n) {}

// Create a slice that refers to the contents of "s"
Slice(const std::string& s) : data_(s.data()), size_(s.size()) {}

// Create a slice that refers to s[0,strlen(s)-1]
Slice(const char* s) : data_(s), size_(strlen(s)) {}

// Intentionally copyable.
Slice(const Slice&) = default;
Slice& operator=(const Slice&) = default;
```
此处均为Slice类的构造函数，当无数据传入时，data_指向的`const char*`为空字符串，size_为0，当传入字符串时，data_赋值为字符串首地址(注意这里为`const char*`)，size_大小与字符串长度相同；当传入数据为string类型时，和字符串类似，data_赋值为字符串首地址，size_赋值为字符串长度；当传入数据同为Slice类型或利用等号赋值时，采用浅拷贝赋值相应Slice。
***
### 成员函数功能详解
``` C++
  // Return a pointer to the beginning of the referenced data
  const char* data() const { return data_; }

  // Return the length (in bytes) of the referenced data
  size_t size() const { return size_; }

  // Return true iff the length of the referenced data is zero
  bool empty() const { return size_ == 0; }
```
由于data_和size_为private变量，无法直接通过外部访问，因此需要成员函数进行访问。
***
``` C++
  // Return the ith byte in the referenced data.
  // REQUIRES: n < size()
  char operator[](size_t n) const {
    assert(n < size());
    return data_[n];
  }
```
当需要类似于字符串访问单个字节数据时，对于`[]`符号的重载，同时若访问数据越界，则利用`assert`函数生成报错信息
***
``` C++
  // Change this slice to refer to an empty array
  void clear() {
    data_ = "";
    size_ = 0;
  }
```
重置Slice数据，注意这里并未对数据原本对应的`const char*`指向的字符串做处理
***
``` C++ 
  // Drop the first "n" bytes from this slice.
  void remove_prefix(size_t n) {
    assert(n <= size());
    data_ += n;
    size_ -= n;
  }
```
和注释描述作用相同，首先判断n是否出现“数组”越界的情况，之后将字符串指针向后移动n位，数据大小减小n代表删除前n个字节，同`clear`函数，该函数并未对数据原本对应的`const char*`指向的字符串做处理
***
``` C++
  // Return a string that contains the copy of the referenced data.
  std::string ToString() const { return std::string(data_, size_); }
```
将Slice数据转换为String类型数据
***
``` C++
  // Three-way comparison.  Returns value:
  //   <  0 iff "*this" <  "b",
  //   == 0 iff "*this" == "b",
  //   >  0 iff "*this" >  "b"
  inline int Slice::compare(const Slice& b) const {
    const size_t min_len = (size_ < b.size_) ? size_ : b.size_;
    int r = memcmp(data_, b.data_, min_len);
    if (r == 0) {
      if (size_ < b.size_)
        r = -1;
      else if (size_ > b.size_)
        r = +1;
    }
    return r;
  }
```
由于属于Slice成员函数，因此访问b的size_不需要通过函数间接访问，这里的字符串比较规则和memcmp比较方法类似
***
``` C++
  // Return true iff "x" is a prefix of "*this"
  bool starts_with(const Slice& x) const {
    return ((size_ >= x.size_) && (memcmp(data_, x.data_, x.size_) == 0));
  }
```
判断x是否为该Slice的前缀
***
### Slice类与运算符结合
``` C++
inline bool operator==(const Slice& x, const Slice& y) {
  return ((x.size() == y.size()) &&
          (memcmp(x.data(), y.data(), x.size()) == 0));
}

inline bool operator!=(const Slice& x, const Slice& y) { return !(x == y); }
```
这里需格外注意，由于operator并非成员函数，因此访问Slice数据时需通过函数`size()`和`data()`间接访问
