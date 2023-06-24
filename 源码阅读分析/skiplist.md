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