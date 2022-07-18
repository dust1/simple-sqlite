## 一些自己的问题

### 在btree中用u16来表示一个指向Page的指针,这些指针是以何种表现形式存在的?又是如何保存的?


### 在btree中的MemPage.apCell与MemPage.u.aDisk是如何对应的?为什么apCell数组的大小要设置为MX_CELL+2?在数据插入过程中发生了什么?

1. apCell[]表示单个Page中保存的所有数据库实体(Cell/Entry)集合，它的大小MX_CELL是根据(页面大小-页面标头)/数据实体理论最小值得到的。


### 游标(Cursor)的作用是什么?
游标是BTree Table中的一种指针，分为只读和读写两种。在一个BTree Table中，只允许存在一个读写游标，允许存在两个或多个只读游标。
> 可能是通过游标来控制对BTree Table的读写权限

用户在插入数据的时候也是向游标指向的Table中进行插入


### sqlitepager_begin为什么传入pData而不是Pager?
在Btree和pager之间交互的就是pData，当从磁盘读取到pData后，pager和btree都是围绕着这块内存空间做操作。使用pData可以使得btree和pager耦合的不是那么紧密?
本质上sqlitepager_begin是开启整个数据库的事务,因此传入的pData只是为了获取到Pager指针。这更像是将参数抽象的一种方法，传入具体的pager指针可以，传入pData根据内存空间布局来获取pager也可以，那么为何不用更加通用的pData，因为btree也好pager也好本质上都是对pData的操作。


### MemPage中的n为什么要以结构体的形式?不能拆分或者以另一个结构体对象的形式嘛?
1. 不已另一个对象的形式：为了在序列化反序列话的时候将结构体的数据也加入到其中

### B+Tree是如何与Page关联上的
1. pgno=1: 这个Page的data属于PageOne结构体。当调用lockBtree函数的时候会尝试创建该Page并将Btree的page1指向这个Page结构化后的data
2. pgno=2: 这个Page会在newDatabase方法中创建。但是这个Page也不会作为Btree的root节点。
3. pgno=3: 从这个Page开始才正式构建BTree,newDatabase后会调用sqliteBtreeCreateTable,该方法会创建当前pagecount+1的下一个Page，也就是pgno=3作为当前BTree的root节点.
> 这里创建的Table为BTree Table，和数据库的表(Table)不是同一个

#### cell与Page的关联
当插入一条记录的时候，记录的key与value会组合为一个Cell,但是这个Cell的大小只有236字节,当记录的大小超过之后会创建一个溢出页面用来保存剩余数据,溢出页面的大小为1020字节.
而记录中key和data的长度信息记录在Cell的CellHdr中.key和data的长度最长为16字节(65535)
保存Cell的时候只需要保存Cell自身的数据，这些数据包括溢出页面的pgno,但不包括溢出页面的数据。溢出页面作为一个单独的Page有pager独立管理。

#### newDatabase的时候的会创建pgno=2的Page，这个Page的作用是什么?

#### 其他Page要如何组合成为BTree呢?

### MemPage与Page是如何关联的
MemPage是保存在磁盘中的Page在内存中的结构。
一个MemPage包括描述参数以及data本身，由于data本身占据了1024字节的长度，因此一个MemPage的大小比SQLITE_PAGE_SIZE要大，在打开数据库的时候对page的EXTRA_SIZE就是MemPage描述信息的大小：
```
#define EXTRA_SIZE (sizeof(MemPage) - SQLITE_PAGE_SIZE)
```
这里表示page的额外信息等于MemPage大小减去包含其中的data部分后剩余的大小。
而EXTRA_SIZE是在内存中分配的。并不会记录到磁盘中，Page在磁盘中的大小始终是SQLITE_PAGE_SIZE

### 如何通过key找到对应的Page
可能跟游标有关。

