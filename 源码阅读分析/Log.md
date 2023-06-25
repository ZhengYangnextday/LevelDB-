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
### 成员变量及内置类
```C++
class Reporter {
  public:
  virtual ~Reporter();

  // Some corruption was detected.  "bytes" is the approximate number
  // of bytes dropped due to the corruption.
  virtual void Corruption(size_t bytes, const Status& status) = 0;
};
```
在`Reader`中内嵌`Reporter`类，用于报告错误，关于`Corruption`的定义根据具体情况有所不同。
```C++
SequentialFile* const file_;
Reporter* const reporter_;
bool const checksum_;
char* const backing_store_;
Slice buffer_;
bool eof_;  // Last Read() indicated EOF by returning < kBlockSize

// Offset of the last record returned by ReadRecord.
uint64_t last_record_offset_;
// Offset of the first location past the end of buffer_.
uint64_t end_of_buffer_offset_;

// Offset at which to start looking for the first record to return
uint64_t const initial_offset_;

// True if we are resynchronizing after a seek (initial_offset_ > 0). In
// particular, a run of kMiddleType and kLastType records can be silently
// skipped in this mode
bool resyncing_;
```
- `SequentialFile* const file_`: 指向文件指针
- `reporter_`: 指向reporter的指针
- `checksum_`: bool值，用于验证checksums是否可用
- `backing_store_`: Log的读取是以Block为单位去从磁盘取数据，从Block中取出的数据存储在blocking_store_中，相当于读数据的buffer。
- `buffer_`: Slice类型，指向`backing_store_`，方便进行数据的操作
- `eof_`: 判断是否到达文件末尾
- `last_record_offset_`: 上一条记录的初始偏移量
- `end_of_buffer_offset_`: 缓存数据结束的偏移量，一般而言是某个block对应的偏移量
- `initial_offset_`: 初始偏移量，从该偏移往后查找出第一条记录，该偏移量可能意义不大
- `resyncing_`: 如果`initial_offset_`不为0，则寻找的record的类型一定是`kFullType`或者`kFirstType`，因此当`initial_offset_`大于0时，初次读取跳过`kMiddleType`和`kLastType`的判断
***
### 函数定义
```C++
Reader::Reporter::~Reporter() = default;

Reader::Reader(SequentialFile* file, Reporter* reporter, bool checksum,
               uint64_t initial_offset)
    : file_(file),
      reporter_(reporter),
      checksum_(checksum),
      backing_store_(new char[kBlockSize]),
      buffer_(),
      eof_(false),
      last_record_offset_(0),
      end_of_buffer_offset_(0),
      initial_offset_(initial_offset),
      resyncing_(initial_offset > 0) {}

Reader::~Reader() { delete[] backing_store_; }
```
对数据进行初始化操作，析构函数时释放`backing_store_`。`checksum_`为校验和，作为可选项使用。
***
***
```C++
bool Reader::SkipToInitialBlock() {
  const size_t offset_in_block = initial_offset_ % kBlockSize;
  uint64_t block_start_location = initial_offset_ - offset_in_block;

  // Don't search a block if we'd be in the trailer
  if (offset_in_block > kBlockSize - 6) {
    block_start_location += kBlockSize;
  }

  end_of_buffer_offset_ = block_start_location;

  // Skip to start of first block that can contain the initial record
  if (block_start_location > 0) {
    Status skip_status = file_->Skip(block_start_location);
    if (!skip_status.ok()) {
      ReportDrop(block_start_location, skip_status);
      return false;
    }
  }

  return true;
}
```
首先，计算`initial_offset_`在当前block中的偏移量`offset_in_block = initial_offset_ % kBlockSize;`以及当前block的起始位置(偏移量)`block_start_location = initial_offset_ - offset_in_block;`，从`initial_offset_`往后开始搜寻第一个record。  
其次，由于当末尾不足7位时，block将自动填充0，因此若当前block中的偏移量`offset_in_block`已大于`kBlockSize`，则跳过该block，从下一个block开始搜寻。  
之后，利用`SequentialFile`类内置函数实现跳转，并记录跳转的状态`skip_stutus`。若跳转失败，则需要通过`ReportDrop`函数，利用成员变量`reporter`传递报错信息，此时返回值为`false`。此外，跳转成功，返回值为`true`
***
***
```C++
bool Reader::ReadRecord(Slice* record, std::string* scratch) {
  if (last_record_offset_ < initial_offset_) {
    if (!SkipToInitialBlock()) {
      return false;
    }
  }

  scratch->clear();
  record->clear();
  bool in_fragmented_record = false;
  // Record offset of the logical record that we're reading
  // 0 is a dummy value to make compilers happy
  uint64_t prospective_record_offset = 0;

  Slice fragment;
  while (true) {
    const unsigned int record_type = ReadPhysicalRecord(&fragment);

    // ReadPhysicalRecord may have only had an empty trailer remaining in its
    // internal buffer. Calculate the offset of the next physical record now
    // that it has returned, properly accounting for its header size.
    uint64_t physical_record_offset =
        end_of_buffer_offset_ - buffer_.size() - kHeaderSize - fragment.size();

    if (resyncing_) {
      if (record_type == kMiddleType) {
        continue;
      } else if (record_type == kLastType) {
        resyncing_ = false;
        continue;
      } else {
        resyncing_ = false;
      }
    }

    switch (record_type) {
      case kFullType:
        if (in_fragmented_record) {
          // Handle bug in earlier versions of log::Writer where
          // it could emit an empty kFirstType record at the tail end
          // of a block followed by a kFullType or kFirstType record
          // at the beginning of the next block.
          if (!scratch->empty()) {
            ReportCorruption(scratch->size(), "partial record without end(1)");
          }
        }
        prospective_record_offset = physical_record_offset;
        scratch->clear();
        *record = fragment;
        last_record_offset_ = prospective_record_offset;
        return true;

      case kFirstType:
        if (in_fragmented_record) {
          // Handle bug in earlier versions of log::Writer where
          // it could emit an empty kFirstType record at the tail end
          // of a block followed by a kFullType or kFirstType record
          // at the beginning of the next block.
          if (!scratch->empty()) {
            ReportCorruption(scratch->size(), "partial record without end(2)");
          }
        }
        prospective_record_offset = physical_record_offset;
        scratch->assign(fragment.data(), fragment.size());
        in_fragmented_record = true;
        break;

      case kMiddleType:
        if (!in_fragmented_record) {
          ReportCorruption(fragment.size(),
                           "missing start of fragmented record(1)");
        } else {
          scratch->append(fragment.data(), fragment.size());
        }
        break;

      case kLastType:
        if (!in_fragmented_record) {
          ReportCorruption(fragment.size(),
                           "missing start of fragmented record(2)");
        } else {
          scratch->append(fragment.data(), fragment.size());
          *record = Slice(*scratch);
          last_record_offset_ = prospective_record_offset;
          return true;
        }
        break;

      case kEof:
        if (in_fragmented_record) {
          // This can be caused by the writer dying immediately after
          // writing a physical record but before completing the next; don't
          // treat it as a corruption, just ignore the entire logical record.
          scratch->clear();
        }
        return false;

      case kBadRecord:
        if (in_fragmented_record) {
          ReportCorruption(scratch->size(), "error in middle of record");
          in_fragmented_record = false;
          scratch->clear();
        }
        break;

      default: {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "unknown record type %u", record_type);
        ReportCorruption(
            (fragment.size() + (in_fragmented_record ? scratch->size() : 0)),
            buf);
        in_fragmented_record = false;
        scratch->clear();
        break;
      }
    }
  }
  return false;
}
```
`ReadRecord`的代码实现较长，将依次进行分析  
- 跳转以及初始化  
```C++
bool Reader::ReadRecord(Slice* record, std::string* scratch) {
  if (last_record_offset_ < initial_offset_) {
    if (!SkipToInitialBlock()) {
      return false;
    }
  }

  scratch->clear();
  record->clear();
  bool in_fragmented_record = false;
  // Record offset of the logical record that we're reading
  // 0 is a dummy value to make compilers happy
  uint64_t prospective_record_offset = 0;

  Slice fragment;
```
首先判断是否需要进行跳转的操作，若上一条record的偏移量(或者说当前的偏移量)`last_record_offset_`已经大于等于初始偏移量`initial_offset`，表明下一条record已经是在初始偏移量`initial_offset`之后，此时无需进行调表操作，否则则需要执行跳转`SkipToInitialBlock`操作。
之后，将`record`和`scratch`清空，为之后的赋值操作做准备。这里读取记录的偏移量`prospective_record_offset`采用`uint64_t`格式，32位byte对应文件大小为4GB，无法满足数据存储要求，因此采用64位作为文件的偏移量，满足数据存储需要。  
`in_fragment_record`作为标识符，判断是否处于一段连续的record之内。`prospective_record_offset`用来记录当前record的偏移量。这里设置为0是因为编译器对于0有专门处理优化。`fragment`存储当前record的信息。
***
- 数据读取与类型判断
```C++
while (true) {
  const unsigned int record_type = ReadPhysicalRecord(&fragment);

  // ReadPhysicalRecord may have only had an empty trailer remaining in its
  // internal buffer. Calculate the offset of the next physical record now
  // that it has returned, properly accounting for its header size.
  uint64_t physical_record_offset =
      end_of_buffer_offset_ - buffer_.size() - kHeaderSize - fragment.size();

  if (resyncing_) {
    if (record_type == kMiddleType) {
      continue;
    } else if (record_type == kLastType) {
      resyncing_ = false;
      continue;
    } else {
      resyncing_ = false;
    }
  }
```
首先，利用`ReadPhysicalRecord`函数读取一条record信息到`fragment`中，有关`ReadPhysicalRecord`的具体操作参后。注意`fragment`中可能并无有效数据，还需根据返回的record类型具体分析。
之后根据`resyncling_`跳过部分数据处理，处理理由参见成员变量的说明。  
在此操作中，还会计算当前record的偏移量`physical_record_offset =  end_of_buffer_offset_ - buffer_.size() - kHeaderSize - fragment.size();`
***
- 根据条件选择合适处理方式
由于情况较多，将逐个进行分析
```C++
switch (record_type) {
  case kFullType:
    if (in_fragmented_record) {
      // Handle bug in earlier versions of log::Writer where
      // it could emit an empty kFirstType record at the tail end
      // of a block followed by a kFullType or kFirstType record
      // at the beginning of the next block.
      if (!scratch->empty()) {
        ReportCorruption(scratch->size(), "partial record without end(1)");
      }
    }
    prospective_record_offset = physical_record_offset;
    scratch->clear();
    *record = fragment;
    last_record_offset_ = prospective_record_offset;
    return true;
```
在分析之前，首先需明确`scratch`的作用。`Slice result`无法对数据进行处理操作，需要通过额外的参数调整`result`指针指向的数据，此参数即为`scratch`。
第一种情况为读取的数据类型为`kFullType`。若`in_fragmented_record`为`true`，则表示处于某一个片段中。产生的原因有两种: 在早期版本中，`log_writer`会在block末尾上传一段空的类型为`kFirstType`或者`kFullType`的记录，因此出现位于记录片段，但是并不是片段类数据的情况。但在该情况下，`scratch`中应无对应缓存，因此若`scratch`不为空，则是出现片段出错的情况，由`reporter`报错信息。  
之后，将记录的偏移量记录`prospective_record_offset = physical_record_offset;`。并清空`scratch`中的数据，为后续判断做准备。将`record`数据置为`fragment`，并更新记录上一条记录的偏移量。  
在该种情况下，由于数据正常读出，因此无论是否出现片段出错，都返回该片段。
***
```C++
  case kFirstType:
    if (in_fragmented_record) {
      // Handle bug in earlier versions of log::Writer where
      // it could emit an empty kFirstType record at the tail end
      // of a block followed by a kFullType or kFirstType record
      // at the beginning of the next block.
      if (!scratch->empty()) {
        ReportCorruption(scratch->size(), "partial record without end(2)");
      }
    }
    prospective_record_offset = physical_record_offset;
    scratch->assign(fragment.data(), fragment.size());
    in_fragmented_record = true;
    break;
```
第二种情况为读取的数据类型为`kFirstType`。由于处于初始状态，理论上`in_fragmented_record`此时还未置为`true`，因此增加错误判定。具体说明同`kFullType`，此处不再赘述。  
由于`prospective_record_offset`位于循环外，不会因为循环更新，因此先由`prospective_record_offset`记录下此时的记录物理偏移量`physical_record_offset`。
将`scratch`内容更新为`fragment`中的数据，为数据合并做准备。同时，将片段标识符`in_fragment_record`更新为`true`，表明数据处于片段中。
***
```C++
  case kMiddleType:
    if (!in_fragmented_record) {
      ReportCorruption(fragment.size(),
                        "missing start of fragmented record(1)");
    } else {
      scratch->append(fragment.data(), fragment.size());
    }
    break;
```
第三种情况为读取的数据类型为`kMiddleType`，此时需保证`in_fragmented_record`为`true`，否则说明读取时未读取到`kFirstType`类型的record，需要报错。  
由于位于record的中间位置，因此仅用向`scratch`后附加数据即可。
***
```C++
  case kLastType:
    if (!in_fragmented_record) {
      ReportCorruption(fragment.size(),
                        "missing start of fragmented record(2)");
    } else {
      scratch->append(fragment.data(), fragment.size());
      *record = Slice(*scratch);
      last_record_offset_ = prospective_record_offset;
      return true;
    }
    break;
```
第四种情况为读取的数据类型为`kLastType`，同`kMiddleType`一样，保证`in_fragmented_record`为`true`。
在此步进行附加操作时，不仅需要将`record`的值更新为`scratch`中的值，同时还需要更新上一个成功读取的record的偏移量`last_record_offset`。完成所有操作后，返回成功标识符。
***
```C++
  case kEof:
    if (in_fragmented_record) {
      // This can be caused by the writer dying immediately after
      // writing a physical record but before completing the next; don't
      // treat it as a corruption, just ignore the entire logical record.
      scratch->clear();
    }
    return false;
```
第五种情况为读取的数据类型为`kEof`。若位于文件尾部，若是出现`in_fragment_record == true`(具体原因同上)，则将`scratch`中数据情况。无论任何情况都返回`false`，表示未成功读取到数据。
***
```C++
  case kBadRecord:
    if (in_fragmented_record) {
      ReportCorruption(scratch->size(), "error in middle of record");
      in_fragmented_record = false;
      scratch->clear();
    }
    break;
```
第六种情况为读取的数据类型为`kBadRecord`。`in_fragment_record`不过多阐述。由于该种情况，既可能是因为读取到的record片段在`initial_offset_`前，也有可能是因为校验码`checksum_`等问题，因此该处仅break，并不返回，重新读取数据来观测后续结果。
***
```C++
  default: {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "unknown record type %u", record_type);
    ReportCorruption(
        (fragment.size() + (in_fragmented_record ? scratch->size() : 0)),
        buf);
    in_fragmented_record = false;
    scratch->clear();
    break;
  }
}
```
最后一种情况为读取数据类型未知，发出报错信息，并更改片段标识符`in_fragmented_record`和清空`scratch`中的数据。报错信息中包含出现错误的数据大小，供后续操作使用。该种情况下依旧不返回，采取重新读取数据的方式。
若未在循环内返回值(理论上不可能)，则返回false，表示读取失败。0
***
***
```C++
unsigned int Reader::ReadPhysicalRecord(Slice* result) {
  while (true) {
    if (buffer_.size() < kHeaderSize) {
      if (!eof_) {
        // Last read was a full read, so this is a trailer to skip
        buffer_.clear();
        Status status = file_->Read(kBlockSize, &buffer_, backing_store_);
        end_of_buffer_offset_ += buffer_.size();
        if (!status.ok()) {
          buffer_.clear();
          ReportDrop(kBlockSize, status);
          eof_ = true;
          return kEof;
        } else if (buffer_.size() < kBlockSize) {
          eof_ = true;
        }
        continue;
      } else {
        // Note that if buffer_ is non-empty, we have a truncated header at the
        // end of the file, which can be caused by the writer crashing in the
        // middle of writing the header. Instead of considering this an error,
        // just report EOF.
        buffer_.clear();
        return kEof;
      }
    }

    // Parse the header
    const char* header = buffer_.data();
    const uint32_t a = static_cast<uint32_t>(header[4]) & 0xff;
    const uint32_t b = static_cast<uint32_t>(header[5]) & 0xff;
    const unsigned int type = header[6];
    const uint32_t length = a | (b << 8);
    if (kHeaderSize + length > buffer_.size()) {
      size_t drop_size = buffer_.size();
      buffer_.clear();
      if (!eof_) {
        ReportCorruption(drop_size, "bad record length");
        return kBadRecord;
      }
      // If the end of the file has been reached without reading |length| bytes
      // of payload, assume the writer died in the middle of writing the record.
      // Don't report a corruption.
      return kEof;
    }

    if (type == kZeroType && length == 0) {
      // Skip zero length record without reporting any drops since
      // such records are produced by the mmap based writing code in
      // env_posix.cc that preallocates file regions.
      buffer_.clear();
      return kBadRecord;
    }

    // Check crc
    if (checksum_) {
      uint32_t expected_crc = crc32c::Unmask(DecodeFixed32(header));
      uint32_t actual_crc = crc32c::Value(header + 6, 1 + length);
      if (actual_crc != expected_crc) {
        // Drop the rest of the buffer since "length" itself may have
        // been corrupted and if we trust it, we could find some
        // fragment of a real log record that just happens to look
        // like a valid log record.
        size_t drop_size = buffer_.size();
        buffer_.clear();
        ReportCorruption(drop_size, "checksum mismatch");
        return kBadRecord;
      }
    }

    buffer_.remove_prefix(kHeaderSize + length);

    // Skip physical record that started before initial_offset_
    if (end_of_buffer_offset_ - buffer_.size() - kHeaderSize - length <
        initial_offset_) {
      result->clear();
      return kBadRecord;
    }

    *result = Slice(header + kHeaderSize, length);
    return type;
  }
}
```
此段代码较长，切片进行分析
***
- 状态判断，是否需要读取或者是否到文件到文件结尾
```C++
while (true) {
    if (buffer_.size() < kHeaderSize) {
      if (!eof_) {
        // Last read was a full read, so this is a trailer to skip
        buffer_.clear();
        Status status = file_->Read(kBlockSize, &buffer_, backing_store_);
        end_of_buffer_offset_ += buffer_.size();
        if (!status.ok()) {
          buffer_.clear();
          ReportDrop(kBlockSize, status);
          eof_ = true;
          return kEof;
        } else if (buffer_.size() < kBlockSize) {
          eof_ = true;
        }
        continue;
      } else {
        // Note that if buffer_ is non-empty, we have a truncated header at the
        // end of the file, which can be caused by the writer crashing in the
        // middle of writing the header. Instead of considering this an error,
        // just report EOF.
        buffer_.clear();
        return kEof;
      }
    }
```
第一个判定是`buffer_`的大小，如果`buffer_`的大小小于`kHeaderSize`，则将清空缓存`buffer_`(这里`buffer_`的条件可以改成`buffer_.size() == 0`，但是可能出现文件末尾填充的情况，因此条件为小于`kHeaderSize`)  
第二个判定为是否到达文件末尾，若已经到达文件末尾，则直接清空当前缓存`buffer_`(避免不必要的缓存`buffer_`问题)并返回`kEof`表示到达文件末尾。  
若非文件末尾，则将`buffer_`清空后，从上一个缓存结束的偏移量`end_of_buffer_offset_`开始，读取一个block大小的数据`kBlockSize`到`buffer_`和`backing_store_`当中。其中`file_->Read()`实现方法为先通过循环读取n个字节到`backing_store_`中，将`buffer_`所对应的数据指向`backing_store_`。`buffer_`的`size`保证了即使读取数据字节不足n，也能正常读取，此外，`backing_store_`在运行期间也需要始终保持Live，不能释放掉。  
之后，更新参数，将`end_of_buffer_offset_`更新，加上`buffer_`的长度，方便之后的校验工作。之后还需判断是否读取成功(文件读取时有无数据遗失)，是否到达文件末尾。第一种情况属于异常情况，需利用`reporter`报告异常情况。这两种情况下，都需要将`eof_`设置为true，但第一种情况直接返回`kEOF`，第二种情况则在下一次循环中返回。
在此步操作中，将数据读取至`buffer_`和`backing_store_`中，方便后续数据处理操作。
***
- 数据读取校验(利用record在block中放置顺序)
```C++
// Parse the header
const char* header = buffer_.data();
const uint32_t a = static_cast<uint32_t>(header[4]) & 0xff;
const uint32_t b = static_cast<uint32_t>(header[5]) & 0xff;
const unsigned int type = header[6];
const uint32_t length = a | (b << 8);
if (kHeaderSize + length > buffer_.size()) {
  size_t drop_size = buffer_.size();
  buffer_.clear();
  if (!eof_) {
    ReportCorruption(drop_size, "bad record length");
    return kBadRecord;
  }
  // If the end of the file has been reached without reading |length| bytes
  // of payload, assume the writer died in the middle of writing the record.
  // Don't report a corruption.
  return kEof;
}
```
由于每一个block都是以record开始放置的，因此通过block中第一个record的header信息对数据进行校验，此外在同一个block中，record是依次放置的，综上所述`buffer_`的前7位[0:6]为一条record的头部信息。首先计算数据长度`length = a | (b << 8);`，如果一条record长度`kHeaderSize + length`，则说明数据读取中有问题。  
若不是文件末尾，则证明数据读取时存在数据遗失的情况，需要利用`report`进行报错处理，如果位于文件末尾，这可能是由于没有读取到`|length|`长度的数据，数据存储时Writer中断所导致，因此仅返回到达文件末尾，并不报错，交给后续处理。
***
- 0记录以及校验码检测
```C++
if (type == kZeroType && length == 0) {
  // Skip zero length record without reporting any drops since
  // such records are produced by the mmap based writing code in
  // env_posix.cc that preallocates file regions.
  buffer_.clear();
  return kBadRecord;
}

// Check crc
if (checksum_) {
  uint32_t expected_crc = crc32c::Unmask(DecodeFixed32(header));
  uint32_t actual_crc = crc32c::Value(header + 6, 1 + length);
  if (actual_crc != expected_crc) {
    // Drop the rest of the buffer since "length" itself may have
    // been corrupted and if we trust it, we could find some
    // fragment of a real log record that just happens to look
    // like a valid log record.
    size_t drop_size = buffer_.size();
    buffer_.clear();
    ReportCorruption(drop_size, "checksum mismatch");
    return kBadRecord;
  }
}
```
若record类型为`kZeroType`，返回`kBadRecord`并跳过即可。该类型为`env_posix.cc`在预分配时出现的数据类型，不由`log_writer.cc`编写相关类型代码。  
此外利用`crc32c`类中函数对数据进行32位校验码检测，若无法匹配，则利用`reporter`报错，并返回`kBadRecord`。
***
- 将record中数据片段赋值
```C++
buffer_.remove_prefix(kHeaderSize + length);

// Skip physical record that started before initial_offset_
if (end_of_buffer_offset_ - buffer_.size() - kHeaderSize - length < initial_offset_) {
  result->clear();
  return kBadRecord;
}

*result = Slice(header + kHeaderSize, length);
return type;
```
**注意: 这里的header指向的是`buffer_`执行`remove_prefix`前最初的数据指针，即`buffer_`执行`remove_prefix`前开始的第一条记录，但并非一定是一个block的第一条记录**
`buffer_`首先更新，将数据指针指向`buffer_`中下一条record的开始。 
之后判断该record的头部是否在`initial_offset_`之后，若不在，则清空result并返回`kBadRecord`，交由`ReadRecord`函数进一步操作。若在，则赋值`result`(更新`result`指向的数据)，并返回record的类型。