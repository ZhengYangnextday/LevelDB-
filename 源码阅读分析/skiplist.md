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

跳表本身的实现不是整个数据库实现的重点，
它只是传统B树的一个弱同步需求下的替代品。
因此我们只关注其中插入和随机确定层数的部分，
同时也是弱同步实现的部分。
即Insert函数和NewNode函数:

### **NewNode函数**

```c++
// NewNode()
template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node* SkipList<Key, Comparator>::NewNode(
    const Key& key, int height) {
  char* const node_memory = arena_->AllocateAligned(
      sizeof(Node) + sizeof(std::atomic<Node*>) * (height - 1));
  return new (node_memory) Node(key);
}
```

用arena分配一块对齐的内存`node_memory`，大小为一个node类加上其他node指针的宽度。
此处使用了定位new运算符的操作，在new后用括号指定一个指针位置，可以让分配的空间指定到该指针所指地址上。
这是利用arena的一种方式，通过arena保证`node_memory`的干净，然后用`new (node_memory) Node(key)`将
key分配到这块干净的内存上。

### **Insert函数**

```C++
// Insert()
template <typename Key, class Comparator>
void SkipList<Key, Comparator>::Insert(const Key& key) {
  Node* prev[kMaxHeight];
  Node* x = FindGreaterOrEqual(key, prev);

  assert(x == nullptr || !Equal(key, x->key));

  int height = RandomHeight();
  if (height > GetMaxHeight()) {
    for (int i = GetMaxHeight(); i < height; i++) {
      prev[i] = head_;
    }
    // It is ok to mutate max_height_ without any synchronization
    // with concurrent readers.  A concurrent reader that observes
    // the new value of max_height_ will see either the old value of
    // new level pointers from head_ (nullptr), or a new value set in
    // the loop below.  In the former case the reader will
    // immediately drop to the next level since nullptr sorts after all
    // keys.  In the latter case the reader will use the new node.
    //
    // max_height可以在不同步的情况下修改，无论是使用新的max_height值还是
    // 旧的max_height值都不会影响跳表的建立（个人认为可以理解为随机到了一个较低的
    // height值罢了。
    max_height_.store(height, std::memory_order_relaxed);
  }

  x = NewNode(key, height);
  for (int i = 0; i < height; i++) {
    // NoBarrier_SetNext() suffices since we will add a barrier when
    // we publish a pointer to "x" in prev[i].
    x->NoBarrier_SetNext(i, prev[i]->NoBarrier_Next(i));
    prev[i]->SetNext(i, x);
  }
}
```

使用`random_height`随机一个高度值`height`，新建`height`数量的节点，将其前后节点连接
至该节点，`NoBarrier_SetNext`是`SetNext`的强制版本，在读取内存的限制上更宽松，使用
`memory_order_relaxed`
因为`prev[i]`不是修改对象，所以不用担心读取到旧版本。而`prev[i]->SetNext(i, x)`中，
x是待插入节点，可能会被多线程读取到旧版本，所以需要用安全一些的`memory_order_release`。

可以把插入过程想象成链表，只是需要连接本key下的所有高度的节点。

### **Comparator**

另一个需要解决的问题是Comparator的重构。

在Skiplist中，comparator只用来判断key的大小，其出现的地方有如下：

```C++
// Equal
  bool Equal(const Key& a, const Key& b) const { return (compare_(a, b) == 0); }
// LessThan
  return (n != nullptr) && (compare_(n->key, key) < 0);
// GreaterThan
  if (next == nullptr || compare_(next->key, key) >= 0)
```

构造函数：

```C++
SkipList<Key, Comparator>::SkipList(Comparator cmp, Arena* arena)
    : compare_(cmp),
      arena_(arena),
      head_(NewNode(0 /* any key will do */, kMaxHeight)),
      max_height_(1),
      rnd_(0xdeadbeef) {
  for (int i = 0; i < kMaxHeight; i++) {
    head_->SetNext(i, nullptr);
  }
}
```

故我们只需要设计一个Compare类（结构体）包含一个重载()运算符的函数，
LE 返回负数， EQ返回0， GE返回正数即可。

```C++
struct Comparator
{
    int operator()(const Key &a, const Key &b) const
    {
        if (a < b) {
            return -1;
        } else if (a > b) {
            return +1;
        } else {
            return 0;
        }
    }
};
```

测试一些经典的接口。

```C++
//Contains()
  REQUIRE(!list.Contains(10));
//Iterator::Valid()
  SkipList<Key, Comparator>::Iterator iter(&list);
  REQUIRE(!iter.Valid());
  iter.SeekToFirst();
  REQUIRE(!iter.Valid());
  iter.Seek(100);
  REQUIRE(!iter.Valid());
  iter.SeekToLast();
  REQUIRE(!iter.Valid());
```

更多测试在`${PROJECT_BINARY_DIR}/tests/skiplist_test.cc`中
