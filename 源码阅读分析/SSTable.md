# SSTable

SSTable是Sorted Table，里面的Key会被做成有序的样子，并且SST的Level越高，有序性越强。
这里的“有序性越强”指的的同一level的SST，其key的聚集程度越高，并且除了level-0的SST(这些是
刚落盘的memtable，无法保证先后落盘的memtable的key不重叠)，其他level的SSTkey一定不会重叠。

低级的level向高级level合并的条件是文件大小达到一定阈值，并且越向高级走，阈值越大，因此越高level的
SST，key聚集程度越高。从level-0到level-1的阈值是2MB，从level-1向level-2是10MB，往后每级递增
10倍，引用自官方文档：

> we create a new level-1 file for every 2MB of data.
>
> When the combined size of files in level-L exceeds (10^L) MB (i.e., 10MB for level-1, 100MB for level-2, ...), one file in level-L, and all of the overlapping files in level-(L+1) are merged to form a set of new files for level-(L+1)

合并操作是判断level-L的文件大小达到阈值后，从所有level-(L-1)的文件中抽取和level-L的key空间交叠的记录，
将它们全部合成成一个level-(L+1)的SST，保持有序。

## 1. 格式

SST table内部格式为

- N个DataBlock
- N个MetaBlock
- metablock_index
- datablock_index
- footer

其中footer为定长，其他由于N的可变性，所以是变长。

![SSTFormat](https://pic1.zhimg.com/80/v2-969d40895a0659d356c3c2e6bd59f904_1440w.webp)
  
整体文件格式如下

![SSTable_Full](https://img-blog.csdnimg.cn/img_convert/c1251bf95451102b36a2e4c454c833ca.png)
### 1.1. Footer 格式

![FooterFormat](https://img-blog.csdnimg.cn/6fc97d6466724818bac71bd7682e8348.png)

BlockHandle两个，分别对应指向metablock_index和datablock_index。
BlockHandle内部只有两个值，一个是offset，起到指针的作用，另一个是size，确定范围。
这两个值使用了VarInt64，详参[coding.md](coding.md)。由于64位可变长整形会占用
$64\times \frac{8}{7} \approx 9.2 \le 10$字节，所以单个BlockHandle最大占用空间为20，
两个BlockHandle是40，再加上footer尾部有8字节的magic number用来标识，一共是48字节。

**不过这里有个疑惑**：Footer是定长的，意味着BlockHandle中的Varint如果被压缩表示了，反而会被填充(padding)
成10字节长度的最大长度版本，其实并没有起到varint的压缩占用空间的作用，那为什么不直接采用FixedInt64的定长版本呢？
反正都要填充，不如直接定长表示，还更省空间一些。难道是利用Varint在小数据上读取比FixedInt快一个常数时间吗？
感觉有点没必要。

**Code**:

```C++
void Footer::EncodeTo(std::string* dst) const {
  const size_t original_size = dst->size();
  metaindex_handle_.EncodeTo(dst);
  index_handle_.EncodeTo(dst);
  dst->resize(2 * BlockHandle::kMaxEncodedLength);  // Padding
  PutFixed32(dst, static_cast<uint32_t>(kTableMagicNumber & 0xffffffffu));
  PutFixed32(dst, static_cast<uint32_t>(kTableMagicNumber >> 32));
  assert(dst->size() == original_size + kEncodedLength);
  (void)original_size;  // Disable unused variable warning.
}
```

个人认为有很多奇怪的点。尤其是为什么不对dst清空，甚至假定里面其实是有数据的呢？
并且还assert了dst的size不小于48，这不就和footer的定长设计违背了吗？

如果假设dst并不是indexBlock的终点，而是indexBlock末尾的某一段上的点，那么
在`resize`操作后dst的大小也只会是40，再加上长度为8的magicNumber后只能是48。
如果assert要通过，只能是`original_size`为0，为何要大费周章地这样去定义呢？

`std::string::resize()`的c++ reference描述如下：

> **Resize string**
>
> Resizes the string to a length of **n** characters.
>
> If n is smaller than the current string length, the current value is shortened to its first n
> character, removing the characters beyond the nth.

```c++
// resizing string
#include <iostream>
#include <string>

int main ()
{
  std::string str ("I like to code in C");
  std::cout << str << '\n';

  unsigned sz = str.size();

  str.resize (sz+2,'+');
  std::cout << str << '\n';

  str.resize (14);
  std::cout << str << '\n';
  return 0;
}
```

Output:

```markdown
I like to code in C
I like to code in C++
I like to code
```

### 1.2. DataBlock格式

![EntryFormat](https://pic1.zhimg.com/80/v2-73468f410dcde74c3841070211c9dac8_1440w.webp)

Block中每条k-v记录被称作一条entry，上图为一条entry的格式。
每条key采用前缀压缩存储方式，当前entry仅需记录以下信息即可还原完整的key-value对:

- 与前一条entry的key**相同前缀的长度**
- 与前一条entry的key**不同前缀的长度**
- 与前一条entry的key**不同前缀的内容**
- value长度
- value内容

由此可见entry间具有依赖性，一旦前一条entry损坏，则无法还原后一条entry，从而导致后续
都无法读取。
为了避免这种情况，DataBlock中每16个entry就会设置一个重启点，重启点即第0/16/32/64/...条k-v记录，
这些点会强制记录**完整信息**，即强制令与前一条key的匹配长度为0，从而在`key_delta`中记录完整的key，
从而避免错误的继续传递。

```c++
// Add():增加一条DataBlock entry
void BlockBuilder::Add(const Slice& key, const Slice& value) {
  // ... 省略一些初始化内容
  size_t shared = 0;
  if (counter_ < options_->block_restart_interval) { // 非重启点
    // ... 计算shared长度，此处shared会增加。
  } else { //重启点，强制shared = 0
    restarts_.push_back(buffer_.size());
    counter_ = 0;
  }
  const size_t non_shared = key.size() - shared;

  // Add "<shared><non_shared><value_size>" to buffer_
  PutVarint32(&buffer_, shared);
  PutVarint32(&buffer_, non_shared);
  PutVarint32(&buffer_, value.size());

  // Slice:data()返回指向Slice数据头部的指针，因此data()+shared偏移就指向了shared的后一个元素，
  // 从这里开始，填入剩下的non_shared个元素，即不同的内容。
  buffer_.append(key.data() + shared, non_shared);
  buffer_.append(value.data(), value.size());

  // ... 
  counter_++;
}
```

这个设计还是蛮巧妙的。

注意到其中有个`restarts_`数组，其中运行到重启点的时候保存了`buffer_.size()`，
这些都是重启点的偏移值，因此在datablock的最后将`restarts_`中的内容全部写入，放在
datablock的尾部，然后再把restarts数组的长度也写入，最后形成这样的结构：

![datablockFormat](https://pic4.zhimg.com/80/v2-037362ba238f6120d5d1ef4bdd479b33_1440w.webp)

`restarts_`记录的这些重启点由于key完整，再搭配SSTable本身key有序的性质，可以作为entry的索引，
提供二分查找的功能。这大大加速了查找的过程，也是很漂亮的做法。

### 1.3. MetaBlock 格式

MetaBlock中保存的是每2KB DataBlock写入后对其内容进行的[BloomFilter](BloomFilter.md)内容。
MetaIndex会记录这些MetaBlock的偏移。

## 2. 构建

### 2.1. TableBuilder

在1.2中我们已经介绍了DataBlock的每一条entry是如何被添加到buffer中，重启点又是如何工作的。
其余几种Block的生成方式都与之类似。

Block中的数据抵达BlockSize阈值后，会被TableBuilder执行`Flush()`来落盘。
因此SST不会等待所有内容填充完成后再一口气写入，而是从dataBlock开始就逐步写入了。
用户（自动化进程）一次交互必须call一个结束的指令，调用`Finish`，才能使SST完全落盘。

```C++
  const size_t estimated_block_size = r->data_block.CurrentSizeEstimate();
  if (estimated_block_size >= r->options.block_size) {
    Flush();
  }
```

DataBlock会这样被一个一个写入，然而metaBlock和IndexBlock不会，它们因为比较小，
并且考虑到写入位置必须在DataBlock后，因此会暂存在缓冲区内（也就是保存在变量里面，或者
写入log之类的），等待DataBlock全部写完后，TableBuilder会call自己的`Finish()`函数，
才开始写入metaBlock和indexBlock。

这里写入metaBlock（官方内部称作filterBlock，也蛮合理）是用的`WriteRawBlock`函数，
禁用了Compression。

```c++
  // Write filter block
  if (ok() && r->filter_block != nullptr) {
    WriteRawBlock(r->filter_block->Finish(), kNoCompression,
                  &filter_block_handle);
  }
```

完事儿之后再写入filterBlockIndex，然后再写IndexBlock，然后再写Footer，详细代码就不贴了，
和这里的到差不差，只是用了`WriteBlock`函数，`WriteBlock`中还有添加CRC等内容，具体就不阐述了。

`Finish`函数和`Abandon`放弃函数在`dbimpl.cc`中一定会被Call其中一个，目的如上述所说，
是为了保证不出现残缺的SST。

```C++
  Status s = input->status();
  const uint64_t current_entries = compact->builder->NumEntries();
  if (s.ok()) {
    s = compact->builder->Finish();
  } else {
    compact->builder->Abandon();
  }
```

## 3. 合成大SST

这里有两种Compaction操作：

1. 从memtable（准确来讲是immutable）到SST的Minor Compaction
2. 从Low level SST 到 High level SST的Major Compaction

前者很好实现，只需要把immutable的东西检查一下，筛掉被打上删除标记的记录，直接落盘即可。
基本代码路径是：

DBImpl::CompactMemTable => DBImpl::WriteLevel0Table => BuildTable

```C++
// CompactMemTable():
void DBImpl::CompactMemTable() {
  mutex_.AssertHeld();
  assert(imm_ != nullptr);

  // Save the contents of the memtable as a new Table
  VersionEdit edit;
  Version* base = versions_->current();
  base->Ref();
  Status s = WriteLevel0Table(imm_, &edit, base);
  base->Unref();
  // ...

// WriteLevel0Table():
// ...
// 生成文件，获得锁，创建一个immutable的迭代器iter传入BuildTable函数。
    s = BuildTable(dbname_, env_, options_, table_cache_, iter, &meta);
// ...
// 写入ManiFest中
}
```

Major Compaction更为复杂，

1. 先要对Manifest文件中记录的所有的SST进行一个审查，
确定需要进行compaction的一个SST。  
这里确定确定最适合进行的compaction的SST的方法是通过版本控制`version_set.cc`和`version_set.h`中`compaction_score_`变量进行衡量，同时每个版本都会有一个变量跟踪当前版本中最适合进行compaction的level(`current_->compaction_level_`)，`compaction_score_`计算方法为`score =static_cast<double>(level_bytes) / MaxBytesForLevel(options_, level)`，当前level数据大小`level_bytes`越靠近上限，score值越高。  
  除此之外，版本控制`version_set`还维持一个`KNUmLevels`的数组`compact_pointer_`，用来记录不同level中上次被compaction的最大key的值，下次再对这个level进行compaction时，就直接从key大于`compact_pointer_[level]`的文件开始，降低时间复杂度。  
2. 然后再检查该SST的低一级的所有SST的key空间范围，两种情况：  
   1. 如果存在一个低级SST，与高级的SST Key空间没有任何交叠：  
      直接Level+1，不用合并，轻松愉快。  

    ```c++
    // ... 上面是manual compaction的情况，不考虑
    // 选择需要compaction的SST: level-L
    } else {
        c = versions_->PickCompaction();
    }

    Status status;
    if (c == nullptr) {
        // Nothing to do
    } 
    else if (!is_manual  && c->IsTrivialMove()) {
        // Move file to next level
        assert(c->num_input_files(0) == 1);
        FileMetaData* f = c->input(0, 0);
        c->edit()->RemoveFile(c->level(), f->number);
        c->edit()->AddFile(c->level() + 1, f->number, f->file_size, f->smallest,
                        f->largest);
        status = versions_->LogAndApply(c->edit(), &mutex_);
        if (!status.ok()) {
          RecordBackgroundError(status);
        }
        VersionSet::LevelSummaryStorage tmp;
        Log(options_.info_log, "Moved #%lld to level-%d %lld bytes %s: %s\n",
            static_cast<unsigned long long>(f->number), c->level() + 1,
            static_cast<unsigned long long>(f->file_size),
            status.ToString().c_str(), versions_->LevelSummary(&tmp));
    }
    ```  

   2. 反之，用多路归并算法把它俩合并。  
    ```C++
    else {
    CompactionState* compact = new CompactionState(c);
    status = DoCompactionWork(compact);
    if (!status.ok()) {
      RecordBackgroundError(status);
    }
    CleanupCompaction(compact);
    c->ReleaseInputs();
    RemoveObsoleteFiles();
    }
    ```
    该部分主体工作位于`DBImpl::DoCompactionWork`，接下来将着重分析该部分源码。  
    在正式分析前，对多路归并进行总体说明。多路归并算法思路比较简单，即从当前Level中选择一个SST文件，然后从Level+1中找到所有和这个文件的Key范围有重合的文件进行合并，最后将合并的文件都放在Level+1中。当然，这一点的实现是基于同一个Level中的文件Key值都不会有重合，但是Level0的文件作为例外，还需额外判断。  
    在以上说明中，需保存涉及到的文件参数。这一点主要通过`PickCompaction()`函数实现，并将文件信息保存至`compaction`类中成员变量`inputs_`数组中。`inputs_[0]`包含需进行合并Level的文件信息，`inputs_[1]`包含Level+1中需要和Level进行合并的文件信息。除Level0外`inputs_[0]`应只包含一个文件信息。

    ***
    首先，定义迭代器，访问`compact`的`inputs_`数组里所指向文件的Key_Value。
    ```C++
    Iterator* input = versions_->MakeInputIterator(compact->compaction);
    mutex_.Unlock();
    input->SeekToFirst();
    ```
    同时，初始化变量，供后续使用
    ```C++
    Status status;
    ParsedInternalKey ikey;
    std::string current_user_key;
    bool has_current_user_key = false;
    SequenceNumber last_sequence_for_key = kMaxSequenceNumber;
    ```
    ***
    之后，进入大循环，通过迭代器`input`遍历`inputs_`数组中的所有文件，即所有参与`compaction`的文件，并获取其对应的键值key。该循环用于判断KV记录是否需要合并入生成文件中。
    ```C++
    while (input->Valid() && !shutting_down_.load(std::memory_order_acquire)) {
      // Prioritize immutable compaction work
      if (has_imm_.load(std::memory_order_relaxed)) {
        const uint64_t imm_start = env_->NowMicros();
        mutex_.Lock();
        if (imm_ != nullptr) {
          //将imm_数据写入磁盘，即前面描述的Minor Compaction
          CompactMemTable();
          // Wake up MakeRoomForWrite() if necessary.
          background_work_finished_signal_.SignalAll();
        }
        mutex_.Unlock();
        imm_micros += (env_->NowMicros() - imm_start);
      }
    ```
    循环首先检查当前的`imm_`是否为空，若不为空，则按照前面所说明的Minor Compaction的方式，将`imm_`数据落盘，避免因`imm_`未及时落盘，导致用户线程无法向磁盘写入新数据。
    ***
    ```C++
    Slice key = input->key();
    if (compact->compaction->ShouldStopBefore(key) &&
        compact->builder != nullptr) {
      status = FinishCompactionOutputFile(compact, input);
      if (!status.ok()) {
        break;
      }
    }
    ```
    然后，先将当前迭代器所指向的Key提取出来。根据Compaction类中的`ShouldStopBefore`函数判断是否符合生成一个新的sstable的条件。如果符合，则利用`FinishCompactionOutputFile`函数，将sstable写盘，若不符合要求则继续操作，**需注意的是，无论是否将sstable落盘，直至此步结束时还未将Key-Value写入任何的sstable中** 
    注意这里`ShouldStopBefore`类实现是根据Compaction类中的`grandparents_`数组和`grandparent_index`实现的，用于控制生成的sstable文件的大小，同时避免Level和Level+2有过多的文件重合，导致单个文件承担过大的合并压力。  
    ***
    接下来，需判断该key值是否应该假如当前的SSTable中，利用`bool drop = false;`作为标志位进行判断，若`drop`为`true`则说明该Key值需丢弃。  
    ```C++
   if (!ParseInternalKey(key, &ikey)) {
      // Do not hide error keys
      current_user_key.clear();
      has_current_user_key = false;
      last_sequence_for_key = kMaxSequenceNumber;
    }
    ```
    **注意，本步相较于常识会比较奇怪**，即在处理不合法数据`!ParseInternalKey(key, &ikey)`时，并不选择丢弃，而是保存下来。根据代码中注释，这样做可能是为了