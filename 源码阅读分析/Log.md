# Log

## Motivation

日志，给memtable提供可恢复能力。写入memtable前先持久化到log上，所以log需要有恢复备份的设计。

## 结构

**定长分块**，保证log部分损坏后仍能按定长读取后续内容。块内部存储记录，**记录可变长**，变长部分是所需记录的数据，所以需要保存每条记录的**长度**和**checksum**。
同时，过长的用户数据(>=32KB)无法一口气存入块内，故需要搭配分片功能。分片要求记录中包含片相对位置的信息，
由于一写多读可以保证顺序写入，所以不用考虑片打乱顺序的情况，只需要记录头、中、尾三种**分片类型**，以及未分片类型，**一共4种**。

单个header长度为7字节，若block剩余长度小于7字节，则无法再填入任何数据(hearder都装不下)，所以需要拿全0填充尾部(padding)

![log图片](https://pic4.zhimg.com/80/v2-53bceb6e579b860b93fe344ff3b437df_1440w.webp)

```c++
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

以下是分片的例子：

**32KB = 2^15 = 32768，故记录长度不能大于32768**，数据长度将会更短（考虑hearders和block已装入的内容）

![分片](https://pic1.zhimg.com/80/v2-380dfc64703fc09c1deba5ca1ba7323c_1440w.webp)
