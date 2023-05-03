# Arena存储机制
## 源码解读
arena作为封装好的类，分析过程从下至上——先从成员变量出发，在具体分析其功能函数，最后明确arena的实现机制和移植思路
### 函数成员定义
``` C++
  char* alloc_ptr_;
  size_t alloc_bytes_remaining_;

  // Array of new[] allocated memory blocks
  std::vector<char*> blocks_;

  // Total memory usage of the arena.
  //
  // TODO(costan): This member is accessed via atomics, but the others are
  //               accessed without any locking. Is this OK?
  std::atomic<size_t> memory_usage_;
```
1. `char* alloc_ptr_`表示当前内存块(block)偏移量指针，也就是未使用内存的首地址
2. `size_t alloc_bytes_remaining_;`记录当前内存块未使用的空间大小
3. `std::vector<char*> blocks_;` vector数组结构，用来存储已分配的内存块对应的指针
4. `std::atomic<size_t> memory_usage_;` 
   1. `<atomic>` 是 C++11 引入的一个头文件，其中包含了一些模板类，用于实现多线程下的原子操作。原子操作是指，一组操作在执行过程中不会被中断的特殊操作，可以保证操作的完整性。原子操作是多线程编程的基础，能够有效避免竞态条件（race condition）等多线程编程中常见的问题。原子类型可以保证单个操作的原子性，从而避免了多线程情况下的数据竞争。C++11 中的 atomic 类型可以用于实现线程安全的计数器、锁等基本数据结构，大大简化了多线程编程的难度。
   2. 该变量本身记录迄今为止分配的内存块的总大小，采用atomic结构，避免了多线程操作时出现竞争情况从而形成死锁等等。
***
### Arena类初始化
``` C++
  Arena();

  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  ~Arena();
```
第一行` Arena()`以及`~Arena()`为构造函数和析构函数，剩余两行使用了C++11中的delete关键字来禁用复制构造函数。这意味着当程序中尝试使用复制构造函数时，编译器将会报错。
***
``` C++
Arena::Arena()
    : alloc_ptr_(nullptr), alloc_bytes_remaining_(0), memory_usage_(0) {}

Arena::~Arena() {
  for (size_t i = 0; i < blocks_.size(); i++) {
    delete[] blocks_[i];
  }
}
```
构造类时，即将所有值初始化，析构函数时，根据blocks_的大小，逐步把blocks_存储的内存块释放
***
### 成员函数功能详解
``` C++
size_t MemoryUsage() const {
    return memory_usage_.load(std::memory_order_relaxed);
  }
```
`memory_usage_.load(std::memory_order_relaxed)` 是一个 C++ 中的原子类型操作，用于从 memory_usage_ 变量中加载当前的值。这个操作使用了 `std::memory_order_relaxed `内存顺序，表示对该原子类型的操作不需要同步其他线程的内存访问。这意味着该操作可以在多线程环境下更快地执行，但是可能会导致其他线程读取到过期的值。因此，需要根据具体的情况来选择使用适当的内存顺序，以确保正确的同步和线程安全。
***
``` C++
char* Arena::AllocateNewBlock(size_t block_bytes) {
  char* result = new char[block_bytes];
  blocks_.push_back(result);
  memory_usage_.fetch_add(block_bytes + sizeof(char*),
                          std::memory_order_relaxed);
  return result;
}
```
`AllocateNewBlock`函数利用new的方式申请block_bytes字节大小的内存，并更新blocks_和memory_usage_信息。
***
``` C++
inline char* Arena::Allocate(size_t bytes) {
  // The semantics of what to return are a bit messy if we allow
  // 0-byte allocations, so we disallow them here (we don't need
  // them for our internal use).
  assert(bytes > 0);
  if (bytes <= alloc_bytes_remaining_) {
    char* result = alloc_ptr_;
    alloc_ptr_ += bytes;
    alloc_bytes_remaining_ -= bytes;
    return result;
  }
  return AllocateFallback(bytes);
}
``` 
`Allocate`函数输入为数据大小，返回为已分配好的存储空间的地址。共有两层判断。第一层判断为检查输入合法性，若输入数据大小小于等于0说明数据有误，利用`assert`函数报错。第二层判断为是否需要重新分配块的判断，若当前块剩余空间大小足够，则返回当前块未使用内存的首地址，同时更新alloc_ptr_和alloc_bytes_remaining_。若当前块剩余空间小于所需数据大小，则执行`AllocateFallback`函数
***
``` C++
static const int kBlockSize = 4096;
char* Arena::AllocateFallback(size_t bytes) {
  if (bytes > kBlockSize / 4) {
    // Object is more than a quarter of our block size.  Allocate it separately
    // to avoid wasting too much space in leftover bytes.
    char* result = AllocateNewBlock(bytes);
    return result;
  }

  // We waste the remaining space in the current block.
  alloc_ptr_ = AllocateNewBlock(kBlockSize);
  alloc_bytes_remaining_ = kBlockSize;

  char* result = alloc_ptr_;
  alloc_ptr_ += bytes;
  alloc_bytes_remaining_ -= bytes;
  return result;
}
```
kBlockSize是默认分配的内存块大小。若所需分配的数据大小已经超过kBlockSize的四分之一，则直接分配对应数据大小的内存。但是本次操作并不更新alloc_ptr_以及alloc_bytes_remaining_，为单独分配内存空间的操作，避免当前内存块还有较多剩余空间却因重新分配造成浪费。  
除此之外，则直接分配kBlockSize即4096字节大小的内存，并根据对应bytes大小更新alloc_ptr_以及alloc_bytes_remaining_。  
在两种情况下，都是传出分配内存块的首地址，但是在第一种情况下alloc_ptr_不变，而在第二种情况下alloc_ptr_为新分配的内存块在分配好后未使用内存块的首地址
***
``` C++
char* Arena::AllocateAligned(size_t bytes) {
  const int align = (sizeof(void*) > 8) ? sizeof(void*) : 8;
  static_assert((align & (align - 1)) == 0,
                "Pointer size should be a power of 2");
  size_t current_mod = reinterpret_cast<uintptr_t>(alloc_ptr_) & (align - 1);
  size_t slop = (current_mod == 0 ? 0 : align - current_mod);
  size_t needed = bytes + slop;
  char* result;
  if (needed <= alloc_bytes_remaining_) {
    result = alloc_ptr_ + slop;
    alloc_ptr_ += needed;
    alloc_bytes_remaining_ -= needed;
  } else {
    // AllocateFallback always returned aligned memory
    result = AllocateFallback(bytes);
  }
  assert((reinterpret_cast<uintptr_t>(result) & (align - 1)) == 0);
  return result;
}
```
`AllocateAligned`为基于malloc的字节对齐内存分配，该操作利用了对齐内存的技巧，可以加速内存访问。  
首先`const int align = (sizeof(void*) > 8) ? sizeof(void*) : 8;`为判断该系统为32位系统还是64位系统。  
之后的代码`static_assert((align & (align - 1)) == 0,"Pointer size should be a power of 2");`用以判断是否为2的整数幂，具体原理如下，任何2的整数幂可以表示为10..0，减去1后为01..1，作与的位运算，则必定为0，不为0则利用`static_assert`生成报错提示。  
之后`size_t current_mod = reinterpret_cast<uintptr_t>(alloc_ptr_) & (align - 1);`代码表示将alloc_ptr_转换为无符号整型，并根据align利用位运算取模。得到current_mod后很容易求得slop = align - current_mod多个字节，内存才是对齐的，于是result = alloc_ptr_ + slop对齐后的结果，所需的内存空间从原本的bytes更新为bytes + slop
## 总结
Arena的作法本质上即避免小对象的频繁分配，减少了对new的调用。通过一次申请大块内存(4096字节)，多次分配。

