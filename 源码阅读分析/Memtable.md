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
