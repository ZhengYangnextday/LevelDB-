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

### MemTable整体结构


### 测试相关

主要测试内容为Memtable的Add和Get接口，以及测试`Ref()`和`Unref()`函数是否能有效控制memtable的构造和析构。

1. **构造Comparator**

```c++
    MemTable *mem_ = new MemTable(comparator_);
    mem_->Ref();
```

用提供的构造函数`MemTable(Comparator*)`来构造，其中`comparator_`采用Slice中自带的comparator：`BytewiseComparator`.

```C++
InternalKeyComparator NewTestComparator()
{
    return InternalKeyComparator(BytewiseComparator());
}
```

这是一个根据字符串长度和字典序进行比较的Comparator.

2. 设计测试用例

```c++
    const std::string testkey_ = "name";
    const std::string testvalue_ = "CO2NoTear";
    mem_->Add(seq, kTypeValue, testkey_, testvalue_);
    LookupKey lookupkey_(testkey_, seq);
    Status s;
    std::string lookupvalue_;
    REQUIRE(mem_->Get(lookupkey_, &lookupvalue_, &s));
    REQUIRE(s.ok());
    REQUIRE(lookupvalue_ == testvalue_);
```

3. 测试memtable的Finish功能：

MemTable需要适时转化为ImmuTable，故我们需要测试memtable自我析构再重构的功能。

```C++
Status FinishMemtable(MemTable *mem_, KVMap *data)
{
    mem_->Unref();
    mem_ = new MemTable(NewTestComparator());
    mem_->Ref();
    int seq = 1;
    for (const auto &kvp : *data) {
        mem_->Add(seq, kTypeValue, kvp.first, kvp.second);
        seq++;
        LookupKey lookupkey_(kvp.first, seq);
        std::string lookupvalue_;
        Status s;
        REQUIRE(mem_->Get(lookupkey_, &lookupvalue_, &s));
        REQUIRE(s.ok());
        REQUIRE(lookupvalue_ == kvp.second);
    }
    return Status::OK();
}
```

当调用`Unref()`时，memtable内置的ref_变量会-1，并判断是否在-1后达到0，若已经达到0，
则认为该memtable已经被抛弃，memtable内容转入immutable，调用private的析构函数，将
自身析构。这里测试析构后重构的memtable是否还能正常写入读取。

```c++
        KVMap *data = new KVMap;
        std::vector<KVItem> dataset;
        for (int seq = 1; seq <= 100; ++seq) {
            char *key_ = new char[20];
            std::snprintf(key_, 20, "%d's value", seq);
            std::string testkey_ = std::string(key_);
            char *value_ = new char[20];
            std::snprintf(key_, 20, "%d", seq);
            std::string testvalue_ = std::string(key_);
            delete[] key_;
            delete[] value_;
            dataset.push_back(KVItem(testkey_, testvalue_));
        }
        data->insert(dataset.begin(), dataset.end());

        FinishMemtable(mem_, data);
```

memtable的后续是SST table，将在Compaction中介绍。
