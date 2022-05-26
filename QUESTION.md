## 一些自己的问题

### 在btree中用u16来表示一个指向Page的指针,这些指针是以何种表现形式存在的?又是如何保存的?


### 在btree中的MemPage.apCell与MemPage.u.aDisk是如何对应的?为什么apCell数组的大小要设置为MX_CELL+2?在数据插入过程中发生了什么?

1. apCell[]表示单个Page中保存的所有数据库实体(Cell/Entry)集合，它的大小MX_CELL是根据(页面大小-页面标头)/数据实体理论最小值得到的。


### 游标(Cursor)的作用是什么?