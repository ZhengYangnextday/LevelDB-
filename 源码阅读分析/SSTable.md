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

### 1.2. Block格式

![BlockFormat](https://pic1.zhimg.com/80/v2-73468f410dcde74c3841070211c9dac8_1440w.webp)

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
