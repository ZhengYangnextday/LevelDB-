# skiplist
## 多线程保障
### 线程安全
线程安全是保证读写过程不出错的重要要求。  
- 写：skiplist要求写线程在外部做好同步，比如使用mutex来保证线程同步。
- 读：skiplist要求读取过程中整个skiplist不能被销毁。除此之外，没有其他限制。

不变性：  
1. 已经分配的节点无需删除，他们只会在skiplist被销毁的时候一并删除。这条性质是通过代码本身保证的，作者没有在skiplist里面提供任何删除单个节点的操作。
2. 只有Insert()操作可以修改skiplist，除了next/prev指针以外的内容都会被放入immutable中。

Skiplist要求的其实是一写多读的操作，为了做到同时只有一个进程在写，在`db_impl.h`中定义了一个写队列  
`std::deque<Writer*> writers_ GUARDED_BY(mutex_)`  
在需要写入内容的时候，生成一个`Writer`类，并将其`push_back`到`writers_`队列中。
## 具体实现以及源码解读
跳表`skiplist`作为一种随机化的数据结构，实现插入，删除，查找的时间复杂度均为O(logN)。在leveldb源码中，涉及到的文件为`skiplist.h`。需注意的是在源码实现跳表的实现中，***除非跳表本身被破坏，否则已分配的节点不会被删除，即任何跳表中的节点都不会被删除。删除插入节点实现，具体不做过多阐述。***  
其主要结构如下  
- 跳表Skiplist
  - Insert 插入
  - Contains 查找
  - RandomHeight 
  - Equal 返回键值是否相等
  - KeyIsAfterNode 返回当前键值是否大于节点中的值
  - FindGreaterOrEqual
  - FindLessThan
  - FindLast
  - Struct Node 节点信息
    - Next/NoBarrier_Next 获取后续节点
    - SetNext/NoBarrier_Next 设置后续节点
  - Class Iterator 迭代器
    - Valid 记录是否有效
    - key 获取当前值
    - Next 下一个节点
    - Prev 前向节点
    - Seek 随机定位节点
    - SeekToFirst 跳转到头
    - SeekToLast 跳转到尾