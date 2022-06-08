## 一些自己的问题

### 在btree中用u16来表示一个指向Page的指针,这些指针是以何种表现形式存在的?又是如何保存的?


### 在btree中的MemPage.apCell与MemPage.u.aDisk是如何对应的?为什么apCell数组的大小要设置为MX_CELL+2?在数据插入过程中发生了什么?

1. apCell[]表示单个Page中保存的所有数据库实体(Cell/Entry)集合，它的大小MX_CELL是根据(页面大小-页面标头)/数据实体理论最小值得到的。


### 游标(Cursor)的作用是什么?
游标是BTree Table中的一种指针，分为只读和读写两种。在一个BTree Table中，只允许存在一个读写游标，允许存在两个或多个只读游标。
> 可能是通过游标来控制对BTree Table的读写权限

用户在插入数据的时候也是向游标指向的Table中进行插入


### sqlitepager_begin为什么传入pData而不是Pager?
sqlitepager_begin只有两个地方会被调用到，分别是sqlitepager_begin和sqliteBtreeBeginTrans.

### B+Tree是如何与Page关联上的
1. pgno=1: 这个Page的data属于PageOne结构体。当调用lockBtree函数的时候会尝试创建该Page并将Btree的page1指向这个Page结构化后的data
2. pgno=2: 这个Page会在newDatabase方法中创建。但是这个Page也不会作为Btree的root节点。
3. pgno=3: 从这个Page开始才正式构建BTree,newDatabase后会调用sqliteBtreeCreateTable,该方法会创建当前pagecount+1的下一个Page，也就是pgno=3作为当前BTree的root节点.
> 这里创建的Table为BTree Table，和数据库的表(Table)不是同一个

#### cell与Page的关联
当插入一条记录的时候，记录的key与value会组合为一个Cell,但是这个Cell的大小只有236字节,当记录的大小超过之后会创建一个溢出页面用来保存剩余数据,溢出页面的大小为1020字节.
而记录中key和data的长度信息记录在Cell的CellHdr中.key和data的长度最长为16字节(65535)

#### newDatabase的时候的会创建pgno=2的Page，这个Page的作用是什么?

#### 其他Page要如何组合成为BTree呢?
