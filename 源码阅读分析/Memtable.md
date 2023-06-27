# MemTable
## 整体解读
MemTable共包含两个存储部分：Memtable，Immutable Memtable。这两者结构一样，差别在于：
memtable 允许写入跟读取。
immutable memtable只读。
当Memtable写入的数据占用内存到达指定数量，则自动转换为Immutable Memtable，等待Dump到磁盘中，系统会自动生成新的Memtable供写操作写入新数据。
其中，须注意的是，MemTable在实现过程中，为减少I/O操作，,删除某个Key的Value时，是通过插入一条打上删除标记的记录实现的，并不存在真正的删除的操作，在后续Compaction操作中再去掉对应的KV记录。此外，MemTable中有序存储的实现依靠跳表部分，详细说明参见[skiplist.md](skiplist.md)。
## 源码解读
### 涉及文件

1. MemTable.h
2. MemTable.cc
3. dbformat.h
4. dbformat.cc
5. skiplist.h

其中，有关跳表skiplist的实现较为复杂，将单独在[skiplist.md](skiplist.md)讲解
### Key解读
作为KV存储引擎，需要明晰代码中出现的5种Key的概念。  
- InternalKey 
- ParsedInternalKey
- User Key
- LookupKey
- Memtable Key  

具体分析如下  
***
- InternalKey
```C++
class InternalKey {
 private:
  std::string rep_;

 public:
  InternalKey() {}  // Leave rep_ as empty to indicate it is invalid
  InternalKey(const Slice& user_key, SequenceNumber s, ValueType t) {
    AppendInternalKey(&rep_, ParsedInternalKey(user_key, s, t));
  }

  bool DecodeFrom(const Slice& s) {
    rep_.assign(s.data(), s.size());
    return !rep_.empty();
  }

  Slice Encode() const {
    assert(!rep_.empty());
    return rep_;
  }

  Slice user_key() const { return ExtractUserKey(rep_); }

  void SetFrom(const ParsedInternalKey& p) {
    rep_.clear();
    AppendInternalKey(&rep_, p);
  }

  void Clear() { rep_.clear(); }

  std::string DebugString() const;
};
```
***
- ParsedInternalKey
```C++
inline bool ParseInternalKey(const Slice& internal_key,
                             ParsedInternalKey* result) {
  const size_t n = internal_key.size();
  if (n < 8) return false;
  uint64_t num = DecodeFixed64(internal_key.data() + n - 8);
  uint8_t c = num & 0xff;
  result->sequence = num >> 8;
  result->type = static_cast<ValueType>(c);
  result->user_key = Slice(internal_key.data(), n - 8);
  return (c <= static_cast<uint8_t>(kTypeValue));
}
```
***
- User Key
***
- LookupKey
```C++
class LookupKey {
 public:
  // Initialize *this for looking up user_key at a snapshot with
  // the specified sequence number.
  LookupKey(const Slice& user_key, SequenceNumber sequence);

  LookupKey(const LookupKey&) = delete;
  LookupKey& operator=(const LookupKey&) = delete;

  ~LookupKey();

  // Return a key suitable for lookup in a MemTable.
  Slice memtable_key() const { return Slice(start_, end_ - start_); }

  // Return an internal key (suitable for passing to an internal iterator)
  Slice internal_key() const { return Slice(kstart_, end_ - kstart_); }

  // Return the user key
  Slice user_key() const { return Slice(kstart_, end_ - kstart_ - 8); }

 private:
  // We construct a char array of the form:
  //    klength  varint32               <-- start_
  //    userkey  char[klength]          <-- kstart_
  //    tag      uint64
  //                                    <-- end_
  // The array is a suitable MemTable key.
  // The suffix starting with "userkey" can be used as an InternalKey.
  const char* start_;
  const char* kstart_;
  const char* end_;
  char space_[200];  // Avoid allocation for short keys
};
```
***
- MemTable Key
***
### 成员变量
```C++
typedef SkipList<const char*, KeyComparator> Table;
KeyComparator comparator_;
int refs_;
Arena arena_;
Table table_;
```
`comparator_`、`table_`作为跳表，负责键值对的插入。`arena_`作为`Arena`类，负责内存的分配，分配机制参见[Arena.md](Arena.md)。`ref_`作为引用计数器来控制MemTable对象是否需要释放。MemTable本质上即对于SkipList的封装。
### 具体函数
```C++
void MemTable::Add(SequenceNumber s, ValueType type, const Slice& key,
                   const Slice& value) {
  // Format of an entry is concatenation of:
  //  key_size     : varint32 of internal_key.size()
  //  key bytes    : char[internal_key.size()]
  //  tag          : uint64((sequence << 8) | type)
  //  value_size   : varint32 of value.size()
  //  value bytes  : char[value.size()]
  size_t key_size = key.size();
  size_t val_size = value.size();
  size_t internal_key_size = key_size + 8;
  const size_t encoded_len = VarintLength(internal_key_size) +
                             internal_key_size + VarintLength(val_size) +
                             val_size;
  char* buf = arena_.Allocate(encoded_len);
  char* p = EncodeVarint32(buf, internal_key_size);
  std::memcpy(p, key.data(), key_size);
  p += key_size;
  EncodeFixed64(p, (s << 8) | type);
  p += 8;
  p = EncodeVarint32(p, val_size);
  std::memcpy(p, value.data(), val_size);
  assert(p + val_size == buf + encoded_len);
  table_.Insert(buf);
}
```
此段代码为`MemTable`中数据增加代码。虽然在skiplist中存储的是键值，实际上是包含键值的KV记录，即代码中buf所对应的数据内容。其结构如下:  
- KV记录
  - internal_key_size: Varint
  - internal_key
    - key
    - tag ((sequence<<8)|type) -> 8 bytes
  - value_size: Varint
  - value  

首先，根据键值`key`和数据值`value`，计算得到`internal_key`的数据长度`internal_key_size`和`value`的数据长度`value_size`，从而得到需要分配的数据长度`encoded_len`。  
之后，利用`arena_`分配`encoded_len` bytes的空间给buf。首先将`internal_key_size`经过编码后利用`EncodeVarint32`函数写入`buf`，并为`p`赋值为写入数据末尾的下一个地址指针，具体实现可以参见[coding.md](coding.md)。  
接下来，按照KV记录的结构逐步写入`internal_key`、`value_size`和`value`的数据。在比较指向写入数据末尾的地址是否相等`p + val_size == buf + encoded_len)`，观测是否需要报错。无问题则将`buf`插入到跳表`table_`中。
***
```C++
```
