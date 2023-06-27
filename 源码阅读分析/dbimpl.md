# dbimpl
## leveldb的具体实现
### Memtable, Immutable:
Memtable和Immutable是一个递进的关系，Memtable可读写，
Immutable只能读，Memtable写满后，将转化为Immutable，
Immutable后续会被压缩到SST中，SST分级合并，Key空间尽量有序。

Memtable的内存分配是通过`Arena`实现的，[Arena.md](Arena.md)，定义在`memtable.h`中，
包含在`Memtable`类中，使用在`db_impl.cc`中，  
`Memtable* mem_ = new Memtable(internal_comparator_)`  
所以每个`Memtable`类只会用到一个`Arena`，只要在`Memtable`的应用中保证线程安全，
就不用担心`Arena`的线程安全问题。