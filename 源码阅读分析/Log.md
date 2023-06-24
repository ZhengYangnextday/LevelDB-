# Log

## Motivation

日志，给memtable提供可恢复能力。写入memtable前先持久化到log上，所以log需要有恢复备份的设计。

## 结构

**定长分块**，保证log部分损坏后仍能按定长读取后续内容。块内部存储记录，**记录可变长**，变长部分是所需记录的数据，所以需要保存每条记录的**长度**和**checksum**。
同时，过长的用户数据(>=32KB)无法一口气存入块内，故需要搭配分片功能。分片要求记录中包含片相对位置的信息，
由于一写多读可以保证顺序写入，所以不用考虑片打乱顺序的情况，只需要记录头、中、尾三种**分片类型**，以及未分片类型，**一共4种**。

单个header长度为7字节，若block剩余长度小于7字节，则无法再填入任何数据(hearder都装不下)，所以需要拿全0填充尾部(padding)

![log图片](https://pic4.zhimg.com/80/v2-53bceb6e579b860b93fe344ff3b437df_1440w.webp)

以下是分片的例子：

**32KB = 2^15 = 32768，故记录长度不能大于32768**，数据长度将会更短（考虑hearders和block已装入的内容）

![分片](https://pic1.zhimg.com/80/v2-380dfc64703fc09c1deba5ca1ba7323c_1440w.webp)
首先定义record的几种类型，第一种类型为0类型，在实际record的读写中当block剩余空间小于header长度时补0。第二种类型为FULL类型，表示record数据完整的存储在当前block中，或者说block剩余空间充足，可以放下整个record。第三、四、五种类型都是在一个block剩余空间不足，需要多个block参与，共同存储同一个record时采用的类型。根据数据大小和record中数据所处完整数据的位置，分为First、Middle、Last。  

## 源码解读

### 涉及文件

1. log_format.h
2. log_reader.h
3. log_reader.cc
4. log_writer.h
5. log_writer.cc

### Log整体结构 -> log_format.h

使用leveldb添加记录时，在写入Memtable之前，会先将记录写入log文件。系统故障恢复时，可以从log中恢复尚未持久化到磁盘的数据。  
一个log文件被划分为多个32K大小的block，每个block块都由一系列record记录组成，其结构如下  

- block
    - record
      - checksum: uint32 //type和data的crc32c校验和，采用小端方式存储
      - length: uint16 //数据长度
      - type: uint8 //定义Record的类型，由于32K有限，可能无法在同一个block中存储完所有数据，因此需要定义利用首部定义，该record是否完整，以及位于整个record的哪一部分。
      - data: uint8[length] //原始数据，以字节存储  

据此，分析log_format.h源码
```C++
enum RecordType {
  // Zero is reserved for preallocated files
  kZeroType = 0,

  kFullType = 1,

  // For fragments
  kFirstType = 2,
  kMiddleType = 3,
  kLastType = 4
};
```

```C++
static const int kMaxRecordType = kLastType;

static const int kBlockSize = 32768;

// Header is checksum (4 bytes), length (2 bytes), type (1 byte).
static const int kHeaderSize = 4 + 2 + 1;
```
这里定义了常数，包括最大类型号`kMaxRecordType`，避免类型不匹配；Block的大小`kBlockSize`为32 * 1024 bytes = 32768；首部的长度`kHeaderSize`为7。其中首部包括除数据外的三部分：校验和(4B)、数据长度(2B)以及record类型(1B)
### Log写入
### 成员变量
```C++
  WritableFile* dest_;
  int block_offset_;  // Current offset in block

  // crc32c values for all supported record types.  These are
  // pre-computed to reduce the overhead of computing the crc of the
  // record type stored in the header.
  uint32_t type_crc_[kMaxRecordType + 1];
```
首先`WritableFile* dest_`为指向可写文件的指针，不过多阐述。block_offset记录当前块地址偏移量，用于判断是否需要分块存储。type_crc_为数据类型的crc数据校验和，利用`InitTypeCrc()`函数预计算。
### 函数定义
```C++
static void InitTypeCrc(uint32_t* type_crc) {
  for (int i = 0; i <= kMaxRecordType; i++) {
    char t = static_cast<char>(i);
    type_crc[i] = crc32c::Value(&t, 1);
  }
}
```
预计算数据类型的校验和并记录到type_crc数组中，供后续操作使用。
***
```C++
Writer::Writer(WritableFile* dest) : dest_(dest), block_offset_(0) {
  InitTypeCrc(type_crc_);
}

Writer::Writer(WritableFile* dest, uint64_t dest_length)
    : dest_(dest), block_offset_(dest_length % kBlockSize) {
  InitTypeCrc(type_crc_);
}

Writer(const Writer&) = delete;
Writer& operator=(const Writer&) = delete;
Writer::~Writer() = default;
```
构造函数，须在开始时提供dest的数据长度，若不提供，将被初始化为空文件。此外，dest还需在Writer使用期间保持Live。  
另外，Writer的拷贝构造函数和赋值运算符都被删除，意味着不能用它们来创建Writer的副本或将其赋值给另一个Writer。
***
```C++
Status Writer::AddRecord(const Slice& slice) {
  const char* ptr = slice.data();
  size_t left = slice.size();

  // Fragment the record if necessary and emit it.  Note that if slice
  // is empty, we still want to iterate once to emit a single
  // zero-length record
  Status s;
  bool begin = true;
  do {
    const int leftover = kBlockSize - block_offset_;
    assert(leftover >= 0);
    if (leftover < kHeaderSize) {
      // Switch to a new block
      if (leftover > 0) {
        // Fill the trailer (literal below relies on kHeaderSize being 7)
        static_assert(kHeaderSize == 7, "");
        dest_->Append(Slice("\x00\x00\x00\x00\x00\x00", leftover));
      }
      block_offset_ = 0;
    }
    // Invariant: we never leave < kHeaderSize bytes in a block.
    assert(kBlockSize - block_offset_ - kHeaderSize >= 0);

    const size_t avail = kBlockSize - block_offset_ - kHeaderSize;
    const size_t fragment_length = (left < avail) ? left : avail;

    RecordType type;
    const bool end = (left == fragment_length);
    if (begin && end) {
      type = kFullType;
    } else if (begin) {
      type = kFirstType;
    } else if (end) {
      type = kLastType;
    } else {
      type = kMiddleType;
    }

    s = EmitPhysicalRecord(type, ptr, fragment_length);
    ptr += fragment_length;
    left -= fragment_length;
    begin = false;
  } while (s.ok() && left > 0);
  return s;
}
```
***
本节代码较长，将依次分析  
- 变量声明
```C++
const char* ptr = slice.data();
size_t left = slice.size();
Status s;
bool begin = true;
```
声明变量，将传入的slice中数据指针赋值到ptr上，同时对left赋值slice的长度，为后续block的判断做铺垫。
***
- 错误检测以及零填充
```C++
const int leftover = kBlockSize - block_offset_;
assert(leftover >= 0);
if (leftover < kHeaderSize) {
  // Switch to a new block
  if (leftover > 0) {
    // Fill the trailer (literal below relies on kHeaderSize being 7)
    static_assert(kHeaderSize == 7, "");
    dest_->Append(Slice("\x00\x00\x00\x00\x00\x00", leftover));
  }
  block_offset_ = 0;
}
```
首先，若`block_offset`对应的偏移量超出Block的大小`KBlockSize`，则说明计算出错，因此引发报错。  
其次，假如剩余空间已经小于record头部大小`kHeaderSize`，则将block剩余空间填充0，同时，将偏移量`block_offset_`重置为0，代表全新的block。  
这当中依旧有对于record头部大小`kHeaderSize`正确性的判断，正确的`kHeaderSize`大小应为7。
***
- 根据剩余block大小以及前后关系明确类型
```C++
assert(kBlockSize - block_offset_ - kHeaderSize >= 0);

const size_t avail = kBlockSize - block_offset_ - kHeaderSize;
const size_t fragment_length = (left < avail) ? left : avail;

RecordType type;
const bool end = (left == fragment_length);
if (begin && end) {
  type = kFullType;
} else if (begin) {
  type = kFirstType;
} else if (end) {
  type = kLastType;
} else {
  type = kMiddleType;
}
```
首先计算该block剩余空间`avail = kBlockSize - block_offset_ - kHeaderSize`。由于在上一步补0操作中，若block中剩余空间大小小于`kHeaderSize`时，将补0，并重置`block_offset_`，从新的block开始存储，因此avail的值需大于0，否则报错。  
之后比较left和avail的大小，若left小于等于avail，则**当前片段大小** `fragment_length`等于数据剩余大小`left`，否则等于`avail`。
Type判断实现较为简单，利用两个标志位判断当前模块属于何种Type，具体效果如下：
- 当begin和end同时为真时，表示在第一次循环中，left大小就小于avail大小，可以一次分配完，所以Type为`kFullType`
- 当只有begin为真时，表示在第一次循环中，left大小大于avail大小，无法一次分配完，因此需要分块存储，在本块中存储的类型为`kFirstType`
- 当只有end为真时，表示在非第一次循环中，或者已分块的后续操作中，剩余数据大小`left`小于可支配空间大小`avail`，因此在本块中存储完剩余信息，类型为`kLastType`
- 当begin和end都为假时，表示在非第一次循环中，剩余数据无法一次存储，所以在该block中存储的为中间数据，类型为`kMiddleType`
***
- 分配空间并更新参数
```C++
s = EmitPhysicalRecord(type, ptr, fragment_length);
ptr += fragment_length;
left -= fragment_length;
begin = false;
```
将各个参数更新，`EmitPhysicalRecord`函数负责记录record的头部信息并将其合并到`dest_`当中，剩余信息更新，并将begin置为`false`，表示非第一次循环
***
将错误检测以及零填充、根据剩余block大小以及前后关系明确类型、分配空间并更新参数在循环中执行，直至`s.ok() && left > 0`结果为假。这里需注意若s出错即`s.ok()`为`false`也会导致循环终止，此时返回s，根据s判断是否记录成功。
***
```C++
Status Writer::EmitPhysicalRecord(RecordType t, const char* ptr,
                                  size_t length) {
  assert(length <= 0xffff);  // Must fit in two bytes
  assert(block_offset_ + kHeaderSize + length <= kBlockSize);

  // Format the header
  char buf[kHeaderSize];
  buf[4] = static_cast<char>(length & 0xff);
  buf[5] = static_cast<char>(length >> 8);
  buf[6] = static_cast<char>(t);

  // Compute the crc of the record type and the payload.
  uint32_t crc = crc32c::Extend(type_crc_[t], ptr, length);
  crc = crc32c::Mask(crc);  // Adjust for storage
  EncodeFixed32(buf, crc);

  // Write the header and the payload
  Status s = dest_->Append(Slice(buf, kHeaderSize));
  if (s.ok()) {
    s = dest_->Append(Slice(ptr, length));
    if (s.ok()) {
      s = dest_->Flush();
    }
  }
  block_offset_ += kHeaderSize + length;
  return s;
}
```
本节代码执行功能为上传record到block中存储，代码较长，依旧依次分析。  
***
```C++
assert(length <= 0xffff);  // Must fit in two bytes
assert(block_offset_ + kHeaderSize + length <= kBlockSize);
```
首先，需进行错误判断，由于`kHeaderSize`中`length`分配字节数为2字节，因此若`length`超过了两字节，将出现数据溢出的情况，因此报错。此外，分配的空间应小于block的大小`kBlockSize`，因此利用`block_offset_ + kHeaderSize + length <= kBlockSize`判断是否分配合理。  
***
```C++
char buf[kHeaderSize];
buf[4] = static_cast<char>(length & 0xff);
buf[5] = static_cast<char>(length >> 8);
buf[6] = static_cast<char>(t);

// Compute the crc of the record type and the payload.
uint32_t crc = crc32c::Extend(type_crc_[t], ptr, length);
crc = crc32c::Mask(crc);  // Adjust for storage
EncodeFixed32(buf, crc);
```
之后，将首部信息存储在buf中，先将长度信息以及类型存储，之后再根据数据计算得到数据和record类型的32位校验码`crc`，存储在buf的0-3位
***
```C++
Status s = dest_->Append(Slice(buf, kHeaderSize));
if (s.ok()) {
  s = dest_->Append(Slice(ptr, length));
  if (s.ok()) {
    s = dest_->Flush();
  }
}
block_offset_ += kHeaderSize + length;
return s;
```
最后，根据`Status s`依次向`dest_`中写入首部信息`buf`以及对应的数据`Slice(ptr, length)`，并判断s是否写入成功，若全部成功，则落盘刷新`dest_->Flush();`。之后将`block_offset`更新并返回`s`，为`AddRecord`中进一步操作做准备。
***
### Log读取

// Compute the crc of the record type and the payload.
uint32_t crc = crc32c::Extend(type_crc_[t], ptr, length);
crc = crc32c::Mask(crc);  // Adjust for storage
EncodeFixed32(buf, crc);
```
之后，将首部信息存储在buf中，先将长度信息以及类型存储，之后再根据数据计算得到数据和record类型的32位校验码`crc`，存储在buf的0-3位
***
```C++
Status s = dest_->Append(Slice(buf, kHeaderSize));
if (s.ok()) {
  s = dest_->Append(Slice(ptr, length));
  if (s.ok()) {
    s = dest_->Flush();
  }
}
block_offset_ += kHeaderSize + length;
return s;
```
最后，根据`Status s`依次向`dest_`中写入首部信息`buf`以及对应的数据`Slice(ptr, length)`，并判断s是否写入成功，若全部成功，则落盘刷新`dest_->Flush();`。之后将`block_offset`更新并返回`s`，为`AddRecord`中进一步操作做准备。
***
### Log读取
