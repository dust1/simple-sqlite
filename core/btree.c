/*
** 2001 September 15
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** $Id: btree.c,v 1.62 2002/06/06 23:16:05 drh Exp $
**
** This file implements a external (disk-based) database using BTrees.
** For a detailed discussion of BTrees, refer to
**
**     Donald E. Knuth, THE ART OF COMPUTER PROGRAMMING, Volume 3:
**     "Sorting And Searching", pages 473-480. Addison-Wesley
**     Publishing Company, Reading, Massachusetts.
**
** The basic idea is that each page of the file contains N database
** entries and N+1 pointers to subpages.
** 其基本思想是，文件的每个页面都包含N个数据库条目和N+1个指向子页面的指针
**
**   ----------------------------------------------------------------
**   |  Ptr(0) | Key(0) | Ptr(1) | Key(1) | ... | Key(N) | Ptr(N+1) |
**   ----------------------------------------------------------------
**
** All of the keys on the page that Ptr(0) points to have values less
** than Key(0).  All of the keys on page Ptr(1) and its subpages have
** values greater than Key(0) and less than Key(1).  All of the keys
** on Ptr(N+1) and its subpages have values greater than Key(N).  And
** so forth.
** 页面上Ptr(0)指向的所有key的值都小于key(0).
** 页面上Ptr(1)及其子页面上的所有key的值都大于key(0)且小于key(1)
** Ptr(N+1)及其子页面上的所有key的值都大于key(N)...
**
** Finding a particular key requires reading O(log(M)) pages from the
** disk where M is the number of entries in the tree.
** 查找特定key所需的时间复杂度为O(log(M)),其中M为查询的实体在tree中的条目数
**
** In this implementation, a single file can hold one or more separate
** BTrees.  Each BTree is identified by the index of its root page.  The
** key and data for any entry are combined to form the "payload".  Up to
** MX_LOCAL_PAYLOAD bytes of payload can be carried directly on the
** database page.  If the payload is larger than MX_LOCAL_PAYLOAD bytes
** then surplus bytes are stored on overflow pages.  The payload for an
** entry and the preceding pointer are combined to form a "Cell".  Each
** page has a small header which contains the Ptr(N+1) pointer.
** 在这个实现中，一个文件可以保存一个或多个单独的BTree。
** 每个BTree都由其root页面的索引标识。
** 任何条目的key和数据都被组合为"payload"。
** 多达MX_LOCAL_PAYLOAD字节的payload可以直接在数据库Page上携带.
** 如果payload大于MX_LOCAL_PAYLOAD，剩余字节保存在溢出页上。
** 条目的有效payload和前面的指针组合一起成为一个"单元".
** 每个页面都有一个小标题，其中包含Ptr(N+1)指针
** > payload可能是一条记录，即data
**
** The first page of the file contains a magic string used to verify that
** the file really is a valid BTree database, a pointer to a list of unused
** pages in the file, and some meta information.  The root of the first
** BTree begins on page 2 of the file.  (Pages are numbered beginning with
** 1, not 0.)  Thus a minimum database contains 2 pages.
** 文件的第一个Page包含magic number用于校验改文件是否是该数据库的有效数据文件，
** 指向文件中未使用Page列表的指针，以及一些元信息。
** 第一个BTree的root从文件的第二个Page开始。
** > pgnp从1开始，一个数据库至少包含两个page
*/
#include "sqliteInt.h"
#include "pager.h"
#include "btree.h"
#include <assert.h>
#include "sqlite.h"
#include "hash.h"
#include "util.h"

/*
** Forward declarations of structures used only in this file.
*/
typedef struct PageOne PageOne;
typedef struct MemPage MemPage;
typedef struct PageHdr PageHdr;
typedef struct Cell Cell;
typedef struct CellHdr CellHdr;
typedef struct FreeBlk FreeBlk;
typedef struct OverflowPage OverflowPage;
typedef struct FreelistInfo FreelistInfo;

/*
** All structures on a database page are aligned to 4-byte boundries.
** This routine rounds up a number of bytes to the next multiple of 4.
**
** This might need to change for computer architectures that require
** and 8-byte alignment boundry for structures.
*/
#define ROUNDUP(X) ((X + 3) & ~3)

/*
** This is a magic string that appears at the beginning of every
** SQLite database in order to identify the file as a real database.
*/
static const char zMagicHeader[] =
    "** This file contains an SQLite 2.1 database **";
#define MAGIC_SIZE (sizeof(zMagicHeader))

/*
** This is a magic integer also used to test the integrity of the database
** file.  This integer is used in addition to the string above so that
** if the file is written on a little-endian architecture and read
** on a big-endian architectures (or vice versa) we can detect the
** problem.
**
** The number used was obtained at random and has no special
** significance other than the fact that it represents a different
** integer on little-endian and big-endian machines.
*/
#define MAGIC 0xdae37528

/*
** The first page of the database file contains a magic header string
** to identify the file as an SQLite database file.  It also contains
** a pointer to the first free page of the file.  Page 2 contains the
** root of the principle BTree.  The file might contain other BTrees
** rooted on pages above 2.
** 数据库的第一个Page，包含头文件校验、空闲Page链表的指针、和一些元数据.
** 第二页则包含BTree的root节点，改文件可能包含两个以上Page为root的其他BTree
**
** The first page also contains SQLITE_N_BTREE_META integers that
** can be used by higher-level routines.
** 第一个Page还包含SQLITE_N_BTREE_META整数，可供更高级别的例程使用
**
** Remember that pages are numbered beginning with 1.  (See pager.c
** for additional information.)  Page 0 does not exist and a page
** number of 0 is used to mean "no such page".
** 记住，pgno起始编号为1. 不存在pgno为0的Page
*/
struct PageOne
{
  char zMagic[MAGIC_SIZE];            /* String that identifies the file as a database | 该数据的文件校验头 */
  int iMagic;                         /* Integer to verify correct byte order | 用于验证正确字节顺序的整数 */
  Pgno freeList;                      /* First free page in a list of all free pages | 所有free页面组成链表的头页面编号 */
  int nFree;                          /* Number of pages on the free list | 所有free页面的数量 */
  int aMeta[SQLITE_N_BTREE_META - 1]; /* User defined integers | 其他元数据信息 */
};

/*
** Each database page has a header that is an instance of this
** structure.
** 每个数据库Page都有一个标头，他是此结构的实例
**
** PageHdr.firstFree is 0 if there is no free space on this page.
** 如果页面中没有空闲空间,则PageHdr.firstFree为0
** Otherwise, PageHdr.firstFree is the index in MemPage.u.aDisk[] of a
** FreeBlk structure that describes the first block of free space.
** All free space is defined by a linked list of FreeBlk structures.
**
**
** Data is stored in a linked list of Cell structures.  PageHdr.firstCell
** is the index into MemPage.u.aDisk[] of the first cell on the page.  The
** Cells are kept in sorted order.
**
** A Cell contains all information about a database entry and a pointer
** to a child page that contains other entries less than itself.  In
** other words, the i-th Cell contains both Ptr(i) and Key(i).  The
** right-most pointer of the page is contained in PageHdr.rightChild.
*/
struct PageHdr
{
  Pgno rightChild; /* Child page that comes after all cells on this page */
  u16 firstCell;   /* Index in MemPage.u.aDisk[] of the first cell|  */
  u16 firstFree;   /* Index in MemPage.u.aDisk[] of the first free block */
};

/*
** Entries on a page of the database are called "Cells".  Each Cell
** has a header and data.  This structure defines the header.  The
** key and data (collectively the "payload") follow this header on
** the database page.
**
** A definition of the complete Cell structure is given below.  The
** header for the cell must be defined first in order to do some
** of the sizing #defines that follow.
*/
struct CellHdr
{
  Pgno leftChild; /* Child page that comes before this cell */
  u16 nKey;       /* Number of bytes in the key */
  u16 iNext;      /* Index in MemPage.u.aDisk[] of next cell in sorted order */
  u8 nKeyHi;      /* Upper 8 bits of key size for keys larger than 64K bytes */
  u8 nDataHi;     /* Upper 8 bits of data size when the size is more than 64K */
  u16 nData;      /* Number of bytes of data */
};

/*
** The key and data size are split into a lower 16-bit segment and an
** upper 8-bit segment in order to pack them together into a smaller
** space.  The following macros reassembly a key or data size back
** into an integer.
*/
#define NKEY(h) (h.nKey + h.nKeyHi * 65536)
#define NDATA(h) (h.nData + h.nDataHi * 65536)

/*
** The minimum size of a complete Cell.  The Cell must contain a header
** and at least 4 bytes of payload.
** 一个数据库条目(Entry/Cell)的最小大小,Cell必须包含一个标头和至少4字节的有效数据
*/
#define MIN_CELL_SIZE (sizeof(CellHdr) + 4)

/*
** The maximum number of database entries that can be held in a single
** page of the database.
** 可以在数据库单个页面中保存的最大数据库条目(Entry)数量
** (单个Page大小-Page标头信息)/最小条目大小
*/
#define MX_CELL ((SQLITE_PAGE_SIZE - sizeof(PageHdr)) / MIN_CELL_SIZE)

/*
** The amount of usable space on a single page of the BTree.  This is the
** page size minus the overhead of the page header.
*/
#define USABLE_SPACE (SQLITE_PAGE_SIZE - sizeof(PageHdr))

/*
** The maximum amount of payload (in bytes) that can be stored locally for
** a database entry.  If the entry contains more data than this, the
** extra goes onto overflow pages.
** 一个数据库条目在本地存储的最大数据量，如果条目的数据超过这条,则溢出的数据进入溢出页面
**
** This number is chosen so that at least 4 cells will fit on every page.
*/
#define MX_LOCAL_PAYLOAD ((USABLE_SPACE / 4 - (sizeof(CellHdr) + sizeof(Pgno))) & ~3)

/*
** Data on a database page is stored as a linked list of Cell structures.
** Both the key and the data are stored in aPayload[].  The key always comes
** first.  The aPayload[] field grows as necessary to hold the key and data,
** up to a maximum of MX_LOCAL_PAYLOAD bytes.  If the size of the key and
** data combined exceeds MX_LOCAL_PAYLOAD bytes, then Cell.ovfl is the
** page number of the first overflow page.
**
** Though this structure is fixed in size, the Cell on the database
** page varies in size.  Every cell has a CellHdr and at least 4 bytes
** of payload space.  Additional payload bytes (up to the maximum of
** MX_LOCAL_PAYLOAD) and the Cell.ovfl value are allocated only as
** needed.
*/
struct Cell
{
  CellHdr h;                       /* The cell header */
  char aPayload[MX_LOCAL_PAYLOAD]; /* Key and data */
  Pgno ovfl;                       /* The first overflow page */
};

/*
** Free space on a page is remembered using a linked list of the FreeBlk
** structures.  Space on a database page is allocated in increments of
** at least 4 bytes and is always aligned to a 4-byte boundry.  The
** linked list of FreeBlks is always kept in order by address.
** 使用 FreeBlk 结构的链表来记住页面上的可用空间。
** 数据库页面上的空间以至少 4 个字节的增量分配，并且始终与 4 个字节的边界对齐。
** FreeBlks 的链表总是按地址顺序排列的。
*/
struct FreeBlk
{
  u16 iSize; /* Number of bytes in this block of free space|这个block的空闲大小 */
  u16 iNext; /* Index in MemPage.u.aDisk[] of the next free block| */
};

/*
** The number of bytes of payload that will fit on a single overflow page.
*/
#define OVERFLOW_SIZE (SQLITE_PAGE_SIZE - sizeof(Pgno))

/*
** When the key and data for a single entry in the BTree will not fit in
** the MX_LOCAL_PAYLOAD bytes of space available on the database page,
** then all extra bytes are written to a linked list of overflow pages.
** Each overflow page is an instance of the following structure.
**
** Unused pages in the database are also represented by instances of
** the OverflowPage structure.  The PageOne.freeList field is the
** page number of the first page in a linked list of unused database
** pages.
*/
struct OverflowPage
{
  Pgno iNext;
  char aPayload[OVERFLOW_SIZE];
};

/*
** The PageOne.freeList field points to a linked list of overflow pages
** hold information about free pages.  The aPayload section of each
** overflow page contains an instance of the following structure.  The
** aFree[] array holds the page number of nFree unused pages in the disk
** file.
*/
struct FreelistInfo
{
  int nFree;
  Pgno aFree[(OVERFLOW_SIZE - sizeof(int)) / sizeof(Pgno)];
};

/*
** For every page in the database file, an instance of the following structure
** is stored in memory.  The u.aDisk[] array contains the raw bits read from
** the disk.  The rest is auxiliary information held in memory only. The
** auxiliary info is only valid for regular database pages - it is not
** used for overflow pages and pages on the freelist.
** 对于数据库中的每一个Page，都采用以下的数据结构实例存储在内存中.
** u.aDisk[]数组包含从磁盘读取的原始数据(即Page的data部分，大小为SQLITE_PAGE_SIZE)
** 其余的是仅保存在内存中的辅助信息，辅助信息仅对常规Page有效，它不适用于
** 溢出页面和空闲列表上的页面
**
** Of particular interest in the auxiliary info is the apCell[] entry.  Each
** apCell[] entry is a pointer to a Cell structure in u.aDisk[].  The cells are
** put in this array so that they can be accessed in constant time, rather
** than in linear time which would be needed if we had to walk the linked
** list on every access.
** 对辅助信息特别感兴趣的是apCell[]条目。每个apCell[]条目都是指向u.aDisk[]中的Cell结构指针。
** 将cell放入这个数组中，以便可以在恒定时间内访问它们，而不是在每次访问时都必须花费遍历链表的线性时间
**
** Note that apCell[] contains enough space to hold up to two more Cells
** than can possibly fit on one page.  In the steady state, every apCell[]
** points to memory inside u.aDisk[].  But in the middle of an insert
** operation, some apCell[] entries may temporarily point to data space
** outside of u.aDisk[].  This is a transient situation that is quickly
** resolved.  But while it is happening, it is possible for a database
** page to hold as many as two more cells than it might otherwise hold.
** The extra two entries in apCell[] are an allowance for this situation.
** apCell[]包含的空间足以容纳比一页可能容纳的多两个单元格。
** > 在稳定状态下，每个apCell[]都指向u.aDisk[]内部的内存。但是在插入操作的过程中，
** > 一些apCell[]条目可能会暂时指向u.aDisk[]之外的数据空间。这是一个短暂的情况,
** > 很快就会得到解决。
** > 但当这个情况发生时，一个数据库页面有可能比其他情况下多容纳两个单元格。
** > apCell[]中额外的两个条目是针对这种情况的允许值
**
** The pParent field points back to the parent page.  This allows us to
** walk up the BTree from any leaf to the root.  Care must be taken to
** unref() the parent page pointer when this page is no longer referenced.
** The pageDestructor() routine handles that chore.
** pParent指向父页面,这使得我们可以从任意叶子Page找到root Page
*/
struct MemPage
{
  union   // union - C的union,里面的元素共用同一段内存空间,长度为最长元素的大小,只能使用一个元素.类似于enum
  {
    char aDisk[SQLITE_PAGE_SIZE]; /* Page data stored on disk| 存储在磁盘中的Page.data */
    PageHdr hdr;                  /* Overlay page header| 页面的头结点 */
  } u;
  int isInit;                /* True if auxiliary data is initialized | 辅助信息是否初始化 */
  MemPage *pParent;          /* The parent of this page.  NULL for root| 该页面的父页面 */
  int nFree;                 /* Number of free bytes in u.aDisk[] | u.aDisk[]中空闲字节数 */
  int nCell;                 /* Number of entries on this page | 这个Page中的entry(Cell}数 */
  int isOverfull;            /* Some apCell[] points outside u.aDisk[] | 是否溢出?u.aDisk[]之外的一些apCell[] */
  Cell *apCell[MX_CELL + 2]; /* All data entires in sorted order | 这个Page中的所有Entry实体(Cell数据) */
};

/*
** The in-memory image of a disk page has the auxiliary information appended
** to the end.  EXTRA_SIZE is the number of bytes of space needed to hold
** that extra information.
** Page保存额外信息大小
*/
#define EXTRA_SIZE (sizeof(MemPage) - SQLITE_PAGE_SIZE)

/*
** Everything we need to know about an open database
*/
struct Btree
{
  Pager *pPager;     /* The page cache | 页面管理器 */
  BtCursor *pCursor; /* A list of all open cursors | 所有打开的游标的列表 */
  PageOne *page1;    /* First page of the database | pgno为1的数据结构, 该结构为Page的data数据 */
  u8 inTrans;        /* True if a transaction is in progress | 如果一个事务正在运行，则为true */
  u8 inCkpt;         /* True if there is a checkpoint on the transaction | 如果事务上有个checkpoing则为true */
  u8 readOnly;       /* True if the underlying file is readonly | 如果数据库是只读的，则为true */
  Hash locks;        /* Key: root page number.  Data: lock count | page持有锁的数量映射。key：pgnp。data：锁的数量  */
};
typedef Btree Bt;

/*
** A cursor is a pointer to a particular entry in the BTree.
** The entry is identified by its MemPage and the index in
** MemPage.apCell[] of the entry.
** 游标(Cursor)是指向BTree中特定条目的指针。
** 该实体由MemPage和MemPage.apCell[] 的索引组成
*/
struct BtCursor
{
  Btree *pBt;              /* The Btree to which this cursor belongs | 此光标所属的BTree */
  BtCursor *pNext, *pPrev; /* Forms a linked list of all cursors | 所有游标的链表 */
  Pgno pgnoRoot;           /* The root page of this tree | 这棵树的根节点(roow Page)编号 */
  MemPage *pPage;          /* Page that contains the entry | 包含该实体(entry)的Page */
  int idx;                 /* Index of the entry in pPage->apCell[] | 该实体位于pPage->apCell[]列表中的下标 */
  u8 wrFlag;               /* True if writable | 如果允许写入则为true */
  u8 bSkipNext;            /* sqliteBtreeNext() is no-op if true | 如果为true,则sqliteBtreeNext()不启用 */
  u8 iMatch;               /* compare result from last sqliteBtreeMoveto() | 比较上一次执行sqliteBtreeMoveto()的结果 */
};

/*
** Compute the total number of bytes that a Cell needs on the main
** database page.  The number returned includes the Cell header,
** local payload storage, and the pointer to overflow pages (if
** applicable).  Additional space allocated on overflow pages
** is NOT included in the value returned from this routine.
** 计算一个cell在数据库页面上需要的总字节数。返回的数字包括Cell标头、
** 本地有效负载存储和指向溢出页面的指针大小。
** 该函数返回的大小不包括在溢出页面上分配的额外空间.
*/
static int cellSize(Cell *pCell)
{
  int n = NKEY(pCell->h) + NDATA(pCell->h);
  if (n > MX_LOCAL_PAYLOAD)
  {
    n = MX_LOCAL_PAYLOAD + sizeof(Pgno);
  }
  else
  {
    n = ROUNDUP(n);
  }
  n += sizeof(CellHdr);
  return n;
}

/*
** Defragment the page given.  All Cells are moved to the
** beginning of the page and all free space is collected
** into one big FreeBlk at the end of the page.
*/
static void defragmentPage(MemPage *pPage)
{
  int pc, i, n;
  FreeBlk *pFBlk;
  char newPage[SQLITE_PAGE_SIZE];

  assert(sqlitepager_iswriteable(pPage));
  pc = sizeof(PageHdr);
  pPage->u.hdr.firstCell = pc;
  memcpy(newPage, pPage->u.aDisk, pc);
  for (i = 0; i < pPage->nCell; i++)
  {
    Cell *pCell = pPage->apCell[i];

    /* This routine should never be called on an overfull page.  The
    ** following asserts verify that constraint. */
    assert(Addr(pCell) > Addr(pPage));
    assert(Addr(pCell) < Addr(pPage) + SQLITE_PAGE_SIZE);

    n = cellSize(pCell);
    pCell->h.iNext = pc + n;
    memcpy(&newPage[pc], pCell, n);
    pPage->apCell[i] = (Cell *)&pPage->u.aDisk[pc];
    pc += n;
  }
  assert(pPage->nFree == SQLITE_PAGE_SIZE - pc);
  memcpy(pPage->u.aDisk, newPage, pc);
  if (pPage->nCell > 0)
  {
    pPage->apCell[pPage->nCell - 1]->h.iNext = 0;
  }
  pFBlk = (FreeBlk *)&pPage->u.aDisk[pc];
  pFBlk->iSize = SQLITE_PAGE_SIZE - pc;
  pFBlk->iNext = 0;
  pPage->u.hdr.firstFree = pc;
  memset(&pFBlk[1], 0, SQLITE_PAGE_SIZE - pc - sizeof(FreeBlk));
}

/*
** Allocate nByte bytes of space on a page.  nByte must be a
** multiple of 4.
** 在页面上分配nByte字节空间。nByte必须是4的倍数
**
** Return the index into pPage->u.aDisk[] of the first byte of
** the new allocation. Or return 0 if there is not enough free
** space on the page to satisfy the allocation request.
** 返回的是这个Cell在Page的索引,这个索引放就是pPage->u.aDisk[]中的下标
**
** If the page contains nBytes of free space but does not contain
** nBytes of contiguous free space, then this routine automatically
** calls defragementPage() to consolidate all free space before
** allocating the new chunk.
*/
static int allocateSpace(MemPage *pPage, int nByte)
{
  FreeBlk *p;
  u16 *pIdx;
  int start;
  int cnt = 0;

  assert(sqlitepager_iswriteable(pPage));
  assert(nByte == ROUNDUP(nByte));
  if (pPage->nFree < nByte || pPage->isOverfull)
    return 0;
  pIdx = &pPage->u.hdr.firstFree;
  p = (FreeBlk *)&pPage->u.aDisk[*pIdx];
  while (p->iSize < nByte)
  {
    assert(cnt++ < SQLITE_PAGE_SIZE / 4);
    if (p->iNext == 0)
    {
      defragmentPage(pPage);
      pIdx = &pPage->u.hdr.firstFree;
    }
    else
    {
      pIdx = &p->iNext;
    }
    p = (FreeBlk *)&pPage->u.aDisk[*pIdx];
  }
  if (p->iSize == nByte)
  {
    start = *pIdx;
    *pIdx = p->iNext;
  }
  else
  {
    FreeBlk *pNew;
    start = *pIdx;
    pNew = (FreeBlk *)&pPage->u.aDisk[start + nByte];
    pNew->iNext = p->iNext;
    pNew->iSize = p->iSize - nByte;
    *pIdx = start + nByte;
  }
  pPage->nFree -= nByte;
  return start;
}

/*
** Return a section of the MemPage.u.aDisk[] to the freelist.
** The first byte of the new free block is pPage->u.aDisk[start]
** and the size of the block is "size" bytes.  Size must be
** a multiple of 4.
**
** Most of the effort here is involved in coalesing adjacent
** free blocks into a single big free block.
*/
static void freeSpace(MemPage *pPage, int start, int size)
{
  int end = start + size;
  u16 *pIdx, idx;
  FreeBlk *pFBlk;
  FreeBlk *pNew;
  FreeBlk *pNext;

  assert(sqlitepager_iswriteable(pPage));
  assert(size == ROUNDUP(size));
  assert(start == ROUNDUP(start));
  pIdx = &pPage->u.hdr.firstFree;
  idx = *pIdx;
  while (idx != 0 && idx < start)
  {
    pFBlk = (FreeBlk *)&pPage->u.aDisk[idx];
    if (idx + pFBlk->iSize == start)
    {
      pFBlk->iSize += size;
      if (idx + pFBlk->iSize == pFBlk->iNext)
      {
        pNext = (FreeBlk *)&pPage->u.aDisk[pFBlk->iNext];
        pFBlk->iSize += pNext->iSize;
        pFBlk->iNext = pNext->iNext;
      }
      pPage->nFree += size;
      return;
    }
    pIdx = &pFBlk->iNext;
    idx = *pIdx;
  }
  pNew = (FreeBlk *)&pPage->u.aDisk[start];
  if (idx != end)
  {
    pNew->iSize = size;
    pNew->iNext = idx;
  }
  else
  {
    pNext = (FreeBlk *)&pPage->u.aDisk[idx];
    pNew->iSize = size + pNext->iSize;
    pNew->iNext = pNext->iNext;
  }
  *pIdx = start;
  pPage->nFree += size;
}

/*
** Initialize the auxiliary information for a disk block.
**
** The pParent parameter must be a pointer to the MemPage which
** is the parent of the page being initialized.  The root of the
** BTree (usually page 2) has no parent and so for that page,
** pParent==NULL.
**
** Return SQLITE_OK on success.  If we see that the page does
** not contained a well-formed database page, then return
** SQLITE_CORRUPT.  Note that a return of SQLITE_OK does not
** guarantee that the page is well-formed.  It only shows that
** we failed to detect any corruption.
*/
static int initPage(MemPage *pPage, Pgno pgnoThis, MemPage *pParent)
{
  int idx;        /* An index into pPage->u.aDisk[] */
  Cell *pCell;    /* A pointer to a Cell in pPage->u.aDisk[] */
  FreeBlk *pFBlk; /* A pointer to a free block in pPage->u.aDisk[] */
  int sz;         /* The size of a Cell in bytes */
  int freeSpace;  /* Amount of free space on the page */

  if (pPage->pParent)
  {
    assert(pPage->pParent == pParent);
    return SQLITE_OK;
  }
  if (pParent)
  {
    pPage->pParent = pParent;
    sqlitepager_ref(pParent);
  }
  if (pPage->isInit)
    return SQLITE_OK;
  pPage->isInit = 1;
  pPage->nCell = 0;
  freeSpace = USABLE_SPACE;
  idx = pPage->u.hdr.firstCell;
  while (idx != 0)
  {
    if (idx > SQLITE_PAGE_SIZE - MIN_CELL_SIZE)
      goto page_format_error;
    if (idx < sizeof(PageHdr))
      goto page_format_error;
    if (idx != ROUNDUP(idx))
      goto page_format_error;
    pCell = (Cell *)&pPage->u.aDisk[idx];
    sz = cellSize(pCell);
    if (idx + sz > SQLITE_PAGE_SIZE)
      goto page_format_error;
    freeSpace -= sz;
    pPage->apCell[pPage->nCell++] = pCell;
    idx = pCell->h.iNext;
  }
  pPage->nFree = 0;
  idx = pPage->u.hdr.firstFree;
  while (idx != 0)
  {
    if (idx > SQLITE_PAGE_SIZE - sizeof(FreeBlk))
      goto page_format_error;
    if (idx < sizeof(PageHdr))
      goto page_format_error;
    pFBlk = (FreeBlk *)&pPage->u.aDisk[idx];
    pPage->nFree += pFBlk->iSize;
    if (pFBlk->iNext > 0 && pFBlk->iNext <= idx)
      goto page_format_error;
    idx = pFBlk->iNext;
  }
  if (pPage->nCell == 0 && pPage->nFree == 0)
  {
    /* As a special case, an uninitialized root page appears to be
    ** an empty database */
    return SQLITE_OK;
  }
  if (pPage->nFree != freeSpace)
    goto page_format_error;
  return SQLITE_OK;

page_format_error:
  return SQLITE_CORRUPT;
}

/*
** Set up a raw page so that it looks like a database page holding
** no entries.
** 设置一个原始页面，使其看起来像一个没有条目的数据库页面
** 对MemPage初始化
*/
static void zeroPage(MemPage *pPage)
{
  // 新建PageHdr和FreeBlk对象
  PageHdr *pHdr;
  FreeBlk *pFBlk;

  assert(sqlitepager_iswriteable(pPage));
  // 先清理pPage
  memset(pPage, 0, SQLITE_PAGE_SIZE);

  // 设置该Page的Hdr
  pHdr = &pPage->u.hdr;
  pHdr->firstCell = 0;
  pHdr->firstFree = sizeof(*pHdr);

  pFBlk = (FreeBlk *)&pHdr[1];
  pFBlk->iNext = 0;
  pFBlk->iSize = SQLITE_PAGE_SIZE - sizeof(*pHdr);

  pPage->nFree = pFBlk->iSize;
  pPage->nCell = 0;
  pPage->isOverfull = 0;
}

/*
** This routine is called when the reference count for a page
** reaches zero.  We need to unref the pParent pointer when that
** happens.
** 当Page引用为0时，调用该函数清除相关附加信息。这部分信息主要位于MemPage中
*/
static void pageDestructor(void *pData)
{
  MemPage *pPage = (MemPage *)pData;
  if (pPage->pParent)
  {
    MemPage *pParent = pPage->pParent;
    pPage->pParent = 0;
    sqlitepager_unref(pParent);
  }
}

/*
** Open a new database.
**
** Actually, this routine just sets up the internal data structures
** for accessing the database.  We do not open the database file
** until the first page is loaded.
**
** zFilename is the name of the database file.  If zFilename is NULL
** a new database with a random name is created.  This randomly named
** database file will be deleted when sqliteBtreeClose() is called.
*/
int sqliteBtreeOpen(
    const char *zFilename, /* Name of the file containing the BTree database */
    int mode,              /* Not currently used */
    int nCache,            /* How many pages in the page cache|page缓存数量 */
    Btree **ppBtree        /* Pointer to new Btree object written here */
)
{
  Btree *pBt;
  int rc;

  printf("[testBtree1] MemPage sizeof => %d\n", sizeof(MemPage));
  // 获取指针长度 => 64位环境下是64
  pBt = malloc(sizeof(*pBt));
  memset(pBt, 0, sizeof(*pBt));

  if (pBt == 0)
  {
    *ppBtree = 0;
    return SQLITE_NOMEM;
  }
  // page缓存的最小数量为10
  if (nCache < 10)
    nCache = 10;

  // 打开Pager
  rc = sqlitepager_open(&pBt->pPager, zFilename, nCache, EXTRA_SIZE);

  if (rc != SQLITE_OK)
  {
    if (pBt->pPager)
      sqlitepager_close(pBt->pPager);
    sqliteFree(pBt);
    *ppBtree = 0;
    return rc;
  }

  // 设置Page引用为0时的析构函数，将附加信息从MemPage中移除
  sqlitepager_set_destructor(pBt->pPager, pageDestructor);

  pBt->pCursor = 0;
  pBt->page1 = 0;
  pBt->readOnly = sqlitepager_isreadonly(pBt->pPager);

  // 初始化Hash对象
  sqliteHashInit(&pBt->locks, SQLITE_HASH_INT, 0);
  *ppBtree = pBt;
  return SQLITE_OK;
}

/*
** Close an open database and invalidate all cursors.
*/
int sqliteBtreeClose(Btree *pBt)
{
  while (pBt->pCursor)
  {
    sqliteBtreeCloseCursor(pBt->pCursor);
  }
  sqlitepager_close(pBt->pPager);
  sqliteHashClear(&pBt->locks);
  sqliteFree(pBt);
  return SQLITE_OK;
}

/*
** Change the limit on the number of pages allowed the cache.
**
** The maximum number of cache pages is set to the absolute
** value of mxPage.  If mxPage is negative, the pager will
** operate asynchronously - it will not stop to do fsync()s
** to insure data is written to the disk surface before
** continuing.  Transactions still work if synchronous is off,
** and the database cannot be corrupted if this program
** crashes.  But if the operating system crashes or there is
** an abrupt power failure when synchronous is off, the database
** could be left in an inconsistent and unrecoverable state.
** Synchronous is on by default so database corruption is not
** normally a worry.
*/
int sqliteBtreeSetCacheSize(Btree *pBt, int mxPage)
{
  sqlitepager_set_cachesize(pBt->pPager, mxPage);
  return SQLITE_OK;
}

/*
** Get a reference to page1 of the database file.  This will
** also acquire a readlock on that file.
** 获取对数据库文件page1的引用。这也将获得该文件的读锁
**
** SQLITE_OK is returned on success.  If the file is not a
** well-formed database file, then SQLITE_CORRUPT is returned.
** SQLITE_BUSY is returned if the database is locked.  SQLITE_NOMEM
** is returned if we run out of memory.  SQLITE_PROTOCOL is returned
** if there is a locking protocol violation.
*/
static int lockBtree(Btree *pBt)
{
  int rc;
  if (pBt->page1)
    return SQLITE_OK;
  // 获取pgno为1的Page,这个Page是特殊的
  // 它的data数据会结构化为PageOne结构体
  printf("[lockBTree] page1 sizeof = %d\n", sizeof(PageOne));
  /**
   * 在lockBtree中会获取pgno为1的Page,其data部分数据会被结构化为PageOne结构体
   */
  rc = sqlitepager_get(pBt->pPager, 1, (void **)&pBt->page1);
  if (rc != SQLITE_OK)
    return rc;

  /* Do some checking to help insure the file we opened really is
  ** a valid database file.
  ** 做一些检查帮助我们确认打开的文件确实是一个数据库文件
  */
  int page_count = sqlitepager_pagecount(pBt->pPager);
  printf("page count ====> %d\n", page_count);
  if (page_count > 0)
  {
    // 第一次进入该方法的时候虽然生成了pgno=1的Page，但是这个page还没有commit,此时page count为0
    PageOne *pP1 = pBt->page1;
    if (strcmp(pP1->zMagic, zMagicHeader) != 0 || pP1->iMagic != MAGIC)
    {
      // 标头校验
      rc = SQLITE_CORRUPT;
      goto page1_init_failed;
    }
  }
  return rc;

page1_init_failed:
  sqlitepager_unref(pBt->page1);
  pBt->page1 = 0;
  return rc;
}

/*
** If there are no outstanding cursors and we are not in the middle
** of a transaction but there is a read lock on the database, then
** this routine unrefs the first page of the database file which
** has the effect of releasing the read lock.
**
** If there are any outstanding cursors, this routine is a no-op.
**
** If there is a transaction in progress, this routine is a no-op.
*/
static void unlockBtreeIfUnused(Btree *pBt)
{
  if (pBt->inTrans == 0 && pBt->pCursor == 0 && pBt->page1 != 0)
  {
    sqlitepager_unref(pBt->page1);
    pBt->page1 = 0;
    pBt->inTrans = 0;
    pBt->inCkpt = 0;
  }
}

/*
** Create a new database by initializing the first two pages of the
** file.
** 通过初始化文件的前两个Page创建一个新的数据库
*/
static int newDatabase(Btree *pBt)
{
  MemPage *pRoot;
  PageOne *pP1;
  int rc;
  // 如果Page数量大于1则直接返回,第一次调用的时候,由于pager还没有commit。
  // 此时page的数量为0
  int page_count = sqlitepager_pagecount(pBt->pPager);
  printf("[newDatabase] page_count =====> %d\n", page_count);
  if (page_count > 1)
    return SQLITE_OK;
  pP1 = pBt->page1;
  rc = sqlitepager_write(pBt->page1);
  if (rc)
    return rc;
  // 获取pgno=2的Page
  rc = sqlitepager_get(pBt->pPager, 2, (void **)&pRoot);
  if (rc)
    return rc;
  rc = sqlitepager_write(pRoot);
  if (rc)
  {
    sqlitepager_unref(pRoot);
    return rc;
  }
  // 写入校验头
  strcpy(pP1->zMagic, zMagicHeader);
  pP1->iMagic = MAGIC;
  // 初始化root节点
  zeroPage(pRoot);
  sqlitepager_unref(pRoot);
  return SQLITE_OK;
}

/*
** Attempt to start a new transaction.
** 尝试开始一个新事物
**
** A transaction must be started before attempting any changes
** to the database.  None of the following routines will work
** unless a transaction is started first:
** 在对数据库进行任何更改前都必须启动事物。
** 如果没有,则以下方法都无效
**
**      sqliteBtreeCreateTable()
**      sqliteBtreeCreateIndex()
**      sqliteBtreeClearTable()
**      sqliteBtreeDropTable()
**      sqliteBtreeInsert()
**      sqliteBtreeDelete()
**      sqliteBtreeUpdateMeta()
*/
int sqliteBtreeBeginTrans(Btree *pBt)
{
  int rc;
  if (pBt->inTrans)
    return SQLITE_ERROR;
  if (pBt->page1 == 0)
  {
    // 获取pgno=1的Page的读权限，并对文件进行标头校验
    rc = lockBtree(pBt);
    if (rc != SQLITE_OK)
    {
      return rc;
    }
  }
  if (pBt->readOnly)
  {
    rc = SQLITE_OK;
  }
  else
  {
    // 开启Page事务
    rc = sqlitepager_begin(pBt->page1);
    if (rc == SQLITE_OK)
    {
      // 如果事务开启成功则创建一个新的数据库
      rc = newDatabase(pBt);
    }
  }

  if (rc == SQLITE_OK)
  {
    pBt->inTrans = 1;
    pBt->inCkpt = 0;
  }
  else
  {
    unlockBtreeIfUnused(pBt);
  }
  return rc;
}

/*
** Commit the transaction currently in progress.
**
** This will release the write lock on the database file.  If there
** are no active cursors, it also releases the read lock.
*/
int sqliteBtreeCommit(Btree *pBt)
{
  int rc;
  if (pBt->inTrans == 0)
    return SQLITE_ERROR;
  rc = pBt->readOnly ? SQLITE_OK : sqlitepager_commit(pBt->pPager);
  pBt->inTrans = 0;
  pBt->inCkpt = 0;
  unlockBtreeIfUnused(pBt);
  return rc;
}

/*
** Rollback the transaction in progress.  All cursors will be
** invalided by this operation.  Any attempt to use a cursor
** that was open at the beginning of this operation will result
** in an error.
**
** This will release the write lock on the database file.  If there
** are no active cursors, it also releases the read lock.
*/
int sqliteBtreeRollback(Btree *pBt)
{
  int rc;
  BtCursor *pCur;
  if (pBt->inTrans == 0)
    return SQLITE_OK;
  pBt->inTrans = 0;
  pBt->inCkpt = 0;
  for (pCur = pBt->pCursor; pCur; pCur = pCur->pNext)
  {
    if (pCur->pPage)
    {
      sqlitepager_unref(pCur->pPage);
      pCur->pPage = 0;
    }
  }
  rc = pBt->readOnly ? SQLITE_OK : sqlitepager_rollback(pBt->pPager);
  unlockBtreeIfUnused(pBt);
  return rc;
}

/*
** Set the checkpoint for the current transaction.  The checkpoint serves
** as a sub-transaction that can be rolled back independently of the
** main transaction.  You must start a transaction before starting a
** checkpoint.  The checkpoint is ended automatically if the transaction
** commits or rolls back.
**
** Only one checkpoint may be active at a time.  It is an error to try
** to start a new checkpoint if another checkpoint is already active.
*/
int sqliteBtreeBeginCkpt(Btree *pBt)
{
  int rc;
  if (!pBt->inTrans || pBt->inCkpt)
  {
    return SQLITE_ERROR;
  }
  rc = pBt->readOnly ? SQLITE_OK : sqlitepager_ckpt_begin(pBt->pPager);
  pBt->inCkpt = 1;
  return rc;
}

/*
** Commit a checkpoint to transaction currently in progress.  If no
** checkpoint is active, this is a no-op.
*/
int sqliteBtreeCommitCkpt(Btree *pBt)
{
  int rc;
  if (pBt->inCkpt && !pBt->readOnly)
  {
    rc = sqlitepager_ckpt_commit(pBt->pPager);
  }
  else
  {
    rc = SQLITE_OK;
  }
  pBt->inCkpt = 0;
  return rc;
}

/*
** Rollback the checkpoint to the current transaction.  If there
** is no active checkpoint or transaction, this routine is a no-op.
**
** All cursors will be invalided by this operation.  Any attempt
** to use a cursor that was open at the beginning of this operation
** will result in an error.
*/
int sqliteBtreeRollbackCkpt(Btree *pBt)
{
  int rc;
  BtCursor *pCur;
  if (pBt->inCkpt == 0 || pBt->readOnly)
    return SQLITE_OK;
  for (pCur = pBt->pCursor; pCur; pCur = pCur->pNext)
  {
    if (pCur->pPage)
    {
      sqlitepager_unref(pCur->pPage);
      pCur->pPage = 0;
    }
  }
  rc = sqlitepager_ckpt_rollback(pBt->pPager);
  pBt->inCkpt = 0;
  return rc;
}

/*
** Create a new cursor for the BTree whose root is on the page
** iTable.  The act of acquiring a cursor gets a read lock on
** the database file.
** 为BTree的root节点创建一个新的光标，获取游标的行为会在数据库文件上获得读锁.
** 此时一个这个游标应该指向root Page
**
** If wrFlag==0, then the cursor can only be used for reading.
** If wrFlag==1, then the cursor can be used for reading or writing.
** 如果wrFlag==0,则光标只能用来读取。如果wrFlag==1,则光标可用于读写
**
** A read/write cursor requires exclusive access to its table.  There
** cannot be two or more cursors open on the same table if any one of
** cursors is a read/write cursor.  But there can be two or more
** read-only cursors open on the same table.
** 一个读写光标需要独占表。不能在同一张表上打开两个或多个读写游标。
** 但是可以在同一张表上打开两个或多个只读游标
**
** No checking is done to make sure that page iTable really is the
** root page of a b-tree.  If it is not, then the cursor acquired
** will not work correctly.
*/
int sqliteBtreeCursor(Btree *pBt, int iTable, int wrFlag, BtCursor **ppCur)
{
  int rc;
  BtCursor *pCur;
  ptr nLock;

  if (pBt->page1 == 0)
  {
    rc = lockBtree(pBt);
    if (rc != SQLITE_OK)
    {
      *ppCur = 0;
      return rc;
    }
  }
  if (wrFlag && pBt->readOnly)
  {
    *ppCur = 0;
    return SQLITE_READONLY;
  }
  pCur = sqliteMalloc(sizeof(*pCur));
  if (pCur == 0)
  {
    rc = SQLITE_NOMEM;
    goto create_cursor_exception;
  }
  pCur->pgnoRoot = (Pgno)iTable;
  rc = sqlitepager_get(pBt->pPager, pCur->pgnoRoot, (void **)&pCur->pPage);
  if (rc != SQLITE_OK)
  {
    goto create_cursor_exception;
  }
  rc = initPage(pCur->pPage, pCur->pgnoRoot, 0);
  if (rc != SQLITE_OK)
  {
    goto create_cursor_exception;
  }
  nLock = (ptr)sqliteHashFind(&pBt->locks, 0, iTable);
  if (nLock < 0 || (nLock > 0 && wrFlag))
  {
    rc = SQLITE_LOCKED;
    goto create_cursor_exception;
  }
  nLock = wrFlag ? -1 : nLock + 1;
  sqliteHashInsert(&pBt->locks, 0, iTable, (void *)nLock);
  pCur->pBt = pBt;
  pCur->wrFlag = wrFlag;
  pCur->idx = 0;
  pCur->pNext = pBt->pCursor;
  if (pCur->pNext)
  {
    pCur->pNext->pPrev = pCur;
  }
  pCur->pPrev = 0;
  pBt->pCursor = pCur;
  *ppCur = pCur;
  return SQLITE_OK;

create_cursor_exception:
  *ppCur = 0;
  if (pCur)
  {
    if (pCur->pPage)
      sqlitepager_unref(pCur->pPage);
    sqliteFree(pCur);
  }
  unlockBtreeIfUnused(pBt);
  return rc;
}

/*
** Close a cursor.  The read lock on the database file is released
** when the last cursor is closed.
*/
int sqliteBtreeCloseCursor(BtCursor *pCur)
{
  ptr nLock;
  Btree *pBt = pCur->pBt;
  if (pCur->pPrev)
  {
    pCur->pPrev->pNext = pCur->pNext;
  }
  else
  {
    pBt->pCursor = pCur->pNext;
  }
  if (pCur->pNext)
  {
    pCur->pNext->pPrev = pCur->pPrev;
  }
  if (pCur->pPage)
  {
    sqlitepager_unref(pCur->pPage);
  }
  unlockBtreeIfUnused(pBt);
  nLock = (ptr)sqliteHashFind(&pBt->locks, 0, pCur->pgnoRoot);
  assert(nLock != 0 || sqlite_malloc_failed);
  nLock = nLock < 0 ? 0 : nLock - 1;
  sqliteHashInsert(&pBt->locks, 0, pCur->pgnoRoot, (void *)nLock);
  sqliteFree(pCur);
  return SQLITE_OK;
}

/*
** Make a temporary cursor by filling in the fields of pTempCur.
** The temporary cursor is not on the cursor list for the Btree.
*/
static void getTempCursor(BtCursor *pCur, BtCursor *pTempCur)
{
  memcpy(pTempCur, pCur, sizeof(*pCur));
  pTempCur->pNext = 0;
  pTempCur->pPrev = 0;
  if (pTempCur->pPage)
  {
    sqlitepager_ref(pTempCur->pPage);
  }
}

/*
** Delete a temporary cursor such as was made by the CreateTemporaryCursor()
** function above.
*/
static void releaseTempCursor(BtCursor *pCur)
{
  if (pCur->pPage)
  {
    sqlitepager_unref(pCur->pPage);
  }
}

/*
** Set *pSize to the number of bytes of key in the entry the
** cursor currently points to.  Always return SQLITE_OK.
** Failure is not possible.  If the cursor is not currently
** pointing to an entry (which can happen, for example, if
** the database is empty) then *pSize is set to 0.
*/
int sqliteBtreeKeySize(BtCursor *pCur, int *pSize)
{
  Cell *pCell;
  MemPage *pPage;

  pPage = pCur->pPage;
  if (pPage == 0 || pCur->idx >= pPage->nCell)
  {
    *pSize = 0;
  }
  else
  {
    pCell = pPage->apCell[pCur->idx];
    *pSize = NKEY(pCell->h);
  }
  return SQLITE_OK;
}

/*
** Read payload information from the entry that the pCur cursor is
** pointing to.  Begin reading the payload at "offset" and read
** a total of "amt" bytes.  Put the result in zBuf.
**
** This routine does not make a distinction between key and data.
** It just reads bytes from the payload area.
*/
static int getPayload(BtCursor *pCur, int offset, int amt, char *zBuf)
{
  char *aPayload;
  Pgno nextPage;
  int rc;
  assert(pCur != 0 && pCur->pPage != 0);
  assert(pCur->idx >= 0 && pCur->idx < pCur->pPage->nCell);
  aPayload = pCur->pPage->apCell[pCur->idx]->aPayload;
  if (offset < MX_LOCAL_PAYLOAD)
  {
    int a = amt;
    if (a + offset > MX_LOCAL_PAYLOAD)
    {
      a = MX_LOCAL_PAYLOAD - offset;
    }
    memcpy(zBuf, &aPayload[offset], a);
    if (a == amt)
    {
      return SQLITE_OK;
    }
    offset = 0;
    zBuf += a;
    amt -= a;
  }
  else
  {
    offset -= MX_LOCAL_PAYLOAD;
  }
  if (amt > 0)
  {
    nextPage = pCur->pPage->apCell[pCur->idx]->ovfl;
  }
  while (amt > 0 && nextPage)
  {
    OverflowPage *pOvfl;
    rc = sqlitepager_get(pCur->pBt->pPager, nextPage, (void **)&pOvfl);
    if (rc != 0)
    {
      return rc;
    }
    nextPage = pOvfl->iNext;
    if (offset < OVERFLOW_SIZE)
    {
      int a = amt;
      if (a + offset > OVERFLOW_SIZE)
      {
        a = OVERFLOW_SIZE - offset;
      }
      memcpy(zBuf, &pOvfl->aPayload[offset], a);
      offset = 0;
      amt -= a;
      zBuf += a;
    }
    else
    {
      offset -= OVERFLOW_SIZE;
    }
    sqlitepager_unref(pOvfl);
  }
  if (amt > 0)
  {
    return SQLITE_CORRUPT;
  }
  return SQLITE_OK;
}

/*
** Read part of the key associated with cursor pCur.  A maximum
** of "amt" bytes will be transfered into zBuf[].  The transfer
** begins at "offset".  The number of bytes actually read is
** returned.  The amount returned will be smaller than the
** amount requested if there are not enough bytes in the key
** to satisfy the request.
*/
int sqliteBtreeKey(BtCursor *pCur, int offset, int amt, char *zBuf)
{
  Cell *pCell;
  MemPage *pPage;

  if (amt < 0)
    return 0;
  if (offset < 0)
    return 0;
  if (amt == 0)
    return 0;
  pPage = pCur->pPage;
  if (pPage == 0)
    return 0;
  if (pCur->idx >= pPage->nCell)
  {
    return 0;
  }
  pCell = pPage->apCell[pCur->idx];
  if (amt + offset > NKEY(pCell->h))
  {
    amt = NKEY(pCell->h) - offset;
    if (amt <= 0)
    {
      return 0;
    }
  }
  getPayload(pCur, offset, amt, zBuf);
  return amt;
}

/*
** Set *pSize to the number of bytes of data in the entry the
** cursor currently points to.  Always return SQLITE_OK.
** Failure is not possible.  If the cursor is not currently
** pointing to an entry (which can happen, for example, if
** the database is empty) then *pSize is set to 0.
*/
int sqliteBtreeDataSize(BtCursor *pCur, int *pSize)
{
  Cell *pCell;
  MemPage *pPage;

  pPage = pCur->pPage;
  if (pPage == 0 || pCur->idx >= pPage->nCell)
  {
    *pSize = 0;
  }
  else
  {
    pCell = pPage->apCell[pCur->idx];
    *pSize = NDATA(pCell->h);
  }
  return SQLITE_OK;
}

/*
** Read part of the data associated with cursor pCur.  A maximum
** of "amt" bytes will be transfered into zBuf[].  The transfer
** begins at "offset".  The number of bytes actually read is
** returned.  The amount returned will be smaller than the
** amount requested if there are not enough bytes in the data
** to satisfy the request.
*/
int sqliteBtreeData(BtCursor *pCur, int offset, int amt, char *zBuf)
{
  Cell *pCell;
  MemPage *pPage;

  if (amt < 0)
    return 0;
  if (offset < 0)
    return 0;
  if (amt == 0)
    return 0;
  pPage = pCur->pPage;
  if (pPage == 0 || pCur->idx >= pPage->nCell)
  {
    return 0;
  }
  pCell = pPage->apCell[pCur->idx];
  if (amt + offset > NDATA(pCell->h))
  {
    amt = NDATA(pCell->h) - offset;
    if (amt <= 0)
    {
      return 0;
    }
  }
  getPayload(pCur, offset + NKEY(pCell->h), amt, zBuf);
  return amt;
}

/*
** Compare an external key against the key on the entry that pCur points to.
** 将外部key与游标指向的Page上的key进行比较
**
** The external key is pKey and is nKey bytes long.  The last nIgnore bytes
** of the key associated with pCur are ignored, as if they do not exist.
** (The normal case is for nIgnore to be zero in which case the entire
** internal key is used in the comparison.)
**
** The comparison result is written to *pRes as follows:
**
**    *pRes<0    This means pCur<pKey, pKey比游标指向的条目所有的Key要小
**
**    *pRes==0   This means pCur==pKey for all nKey bytes, pKey位于游标指向的Page中
**
**    *pRes>0    This means pCur>pKey, pKey比游标指向的条目所有的key要大
**
** When one key is an exact prefix of the other, the shorter key is
** considered less than the longer one.  In order to be equal the
** keys must be exactly the same length. (The length of the pCur key
** is the actual key length minus nIgnore bytes.)
*/
int sqliteBtreeKeyCompare(
    BtCursor *pCur,   /* Pointer to entry to compare against */
    const void *pKey, /* Key to compare against entry that pCur points to */
    int nKey,         /* Number of bytes in pKey */
    int nIgnore,      /* Ignore this many bytes at the end of pCur */
    int *pResult      /* Write the result here */
)
{
  Pgno nextPage;
  int n, c, rc, nLocal;
  Cell *pCell;
  const char *zKey = (const char *)pKey;

  assert(pCur->pPage);
  assert(pCur->idx >= 0 && pCur->idx < pCur->pPage->nCell);
  pCell = pCur->pPage->apCell[pCur->idx];
  nLocal = NKEY(pCell->h) - nIgnore;
  if (nLocal < 0)
    nLocal = 0;
  n = nKey < nLocal ? nKey : nLocal;
  if (n > MX_LOCAL_PAYLOAD)
  {
    n = MX_LOCAL_PAYLOAD;
  }
  c = memcmp(pCell->aPayload, zKey, n);
  if (c != 0)
  {
    *pResult = c;
    return SQLITE_OK;
  }
  zKey += n;
  nKey -= n;
  nLocal -= n;
  nextPage = pCell->ovfl;
  while (nKey > 0 && nLocal > 0)
  {
    OverflowPage *pOvfl;
    if (nextPage == 0)
    {
      return SQLITE_CORRUPT;
    }
    rc = sqlitepager_get(pCur->pBt->pPager, nextPage, (void **)&pOvfl);
    if (rc)
    {
      return rc;
    }
    nextPage = pOvfl->iNext;
    n = nKey < nLocal ? nKey : nLocal;
    if (n > OVERFLOW_SIZE)
    {
      n = OVERFLOW_SIZE;
    }
    c = memcmp(pOvfl->aPayload, zKey, n);
    sqlitepager_unref(pOvfl);
    if (c != 0)
    {
      *pResult = c;
      return SQLITE_OK;
    }
    nKey -= n;
    nLocal -= n;
    zKey += n;
  }
  if (c == 0)
  {
    c = nLocal - nKey;
  }
  *pResult = c;
  return SQLITE_OK;
}

/*
** Move the cursor down to a new child page.
*/
static int moveToChild(BtCursor *pCur, int newPgno)
{
  int rc;
  MemPage *pNewPage;

  rc = sqlitepager_get(pCur->pBt->pPager, newPgno, (void **)&pNewPage);
  if (rc)
    return rc;
  rc = initPage(pNewPage, newPgno, pCur->pPage);
  if (rc)
    return rc;
  sqlitepager_unref(pCur->pPage);
  pCur->pPage = pNewPage;
  pCur->idx = 0;
  return SQLITE_OK;
}

/*
** Move the cursor up to the parent page.
**
** pCur->idx is set to the cell index that contains the pointer
** to the page we are coming from.  If we are coming from the
** right-most child page then pCur->idx is set to one more than
** the largest cell index.
*/
static int moveToParent(BtCursor *pCur)
{
  Pgno oldPgno;
  MemPage *pParent;
  int i;
  pParent = pCur->pPage->pParent;
  if (pParent == 0)
    return SQLITE_INTERNAL;
  oldPgno = sqlitepager_pagenumber(pCur->pPage);
  sqlitepager_ref(pParent);
  sqlitepager_unref(pCur->pPage);
  pCur->pPage = pParent;
  pCur->idx = pParent->nCell;
  for (i = 0; i < pParent->nCell; i++)
  {
    if (pParent->apCell[i]->h.leftChild == oldPgno)
    {
      pCur->idx = i;
      break;
    }
  }
  return SQLITE_OK;
}

/*
** Move the cursor to the root page
*/
static int moveToRoot(BtCursor *pCur)
{
  MemPage *pNew;
  int rc;

  rc = sqlitepager_get(pCur->pBt->pPager, pCur->pgnoRoot, (void **)&pNew);
  if (rc)
    return rc;
  rc = initPage(pNew, pCur->pgnoRoot, 0);
  if (rc)
    return rc;
  sqlitepager_unref(pCur->pPage);
  pCur->pPage = pNew;
  pCur->idx = 0;
  return SQLITE_OK;
}

/*
** Move the cursor down to the left-most leaf entry beneath the
** entry to which it is currently pointing.
*/
static int moveToLeftmost(BtCursor *pCur)
{
  Pgno pgno;
  int rc;

  while ((pgno = pCur->pPage->apCell[pCur->idx]->h.leftChild) != 0)
  {
    rc = moveToChild(pCur, pgno);
    if (rc)
      return rc;
  }
  return SQLITE_OK;
}

/* Move the cursor to the first entry in the table.  Return SQLITE_OK
** on success.  Set *pRes to 0 if the cursor actually points to something
** or set *pRes to 1 if the table is empty.
*/
int sqliteBtreeFirst(BtCursor *pCur, int *pRes)
{
  int rc;
  if (pCur->pPage == 0)
    return SQLITE_ABORT;
  rc = moveToRoot(pCur);
  if (rc)
    return rc;
  if (pCur->pPage->nCell == 0)
  {
    *pRes = 1;
    return SQLITE_OK;
  }
  *pRes = 0;
  rc = moveToLeftmost(pCur);
  pCur->bSkipNext = 0;
  return rc;
}

/* Move the cursor to the last entry in the table.  Return SQLITE_OK
** on success.  Set *pRes to 0 if the cursor actually points to something
** or set *pRes to 1 if the table is empty.
*/
int sqliteBtreeLast(BtCursor *pCur, int *pRes)
{
  int rc;
  Pgno pgno;
  if (pCur->pPage == 0)
    return SQLITE_ABORT;
  rc = moveToRoot(pCur);
  if (rc)
    return rc;
  if (pCur->pPage->nCell == 0)
  {
    *pRes = 1;
    return SQLITE_OK;
  }
  *pRes = 0;
  while ((pgno = pCur->pPage->u.hdr.rightChild) != 0)
  {
    rc = moveToChild(pCur, pgno);
    if (rc)
      return rc;
  }
  pCur->idx = pCur->pPage->nCell - 1;
  pCur->bSkipNext = 0;
  return rc;
}

/* Move the cursor so that it points to an entry near pKey.
** Return a success code.
** 移动光标，使其指向pKey附近的条目(找到对应的page).返回成功代码
**
** If an exact match is not found, then the cursor is always
** left pointing at a leaf page which would hold the entry if it
** were present.  The cursor might point to an entry that comes
** before or after the key.
**
** The result of comparing the key with the entry to which the
** cursor is left pointing is stored in pCur->iMatch.  The same
** value is also written to *pRes if pRes!=NULL.  The meaning of
** this value is as follows:
**
**     *pRes<0      The cursor is left pointing at an entry that
**                  is smaller than pKey.
**                  光标指向一个小于pKey的条目
**
**     *pRes==0     The cursor is left pointing at an entry that
**                  exactly matches pKey.
**                  光标指向与pKey完全匹配的条目
**
**     *pRes>0      The cursor is left pointing at an entry that
**                  is larger than pKey.
**                  光标指向一个大于pKey的条目
*/
int sqliteBtreeMoveto(BtCursor *pCur, const void *pKey, int nKey, int *pRes)
{
  int rc;
  if (pCur->pPage == 0)
    return SQLITE_ABORT;
  pCur->bSkipNext = 0;
  // 先将光标指向root Page
  rc = moveToRoot(pCur);
  if (rc)
    return rc;
  for (;;)
  {
    int lwr, upr;
    Pgno chldPg;
    // 光标指向的Page
    MemPage *pPage = pCur->pPage;
    int c = -1;
    lwr = 0;
    // 光标指向的Page拥有的Cell
    upr = pPage->nCell - 1;
    while (lwr <= upr)
    {
      pCur->idx = (lwr + upr) / 2;
      rc = sqliteBtreeKeyCompare(pCur, pKey, nKey, 0, &c);
      if (rc)
        return rc;
      if (c == 0)
      {
        pCur->iMatch = c;
        if (pRes)
          *pRes = 0;
        return SQLITE_OK;
      }
      if (c < 0)
      {
        lwr = pCur->idx + 1;
      }
      else
      {
        upr = pCur->idx - 1;
      }
    }
    assert(lwr == upr + 1);
    if (lwr >= pPage->nCell)
    {
      chldPg = pPage->u.hdr.rightChild;
    }
    else
    {
      chldPg = pPage->apCell[lwr]->h.leftChild;
    }
    if (chldPg == 0)
    {
      pCur->iMatch = c;
      if (pRes)
        *pRes = c;
      return SQLITE_OK;
    }
    rc = moveToChild(pCur, chldPg);
    if (rc)
      return rc;
  }
  /* NOT REACHED */
}

/*
** Advance the cursor to the next entry in the database.  If
** successful and pRes!=NULL then set *pRes=0.  If the cursor
** was already pointing to the last entry in the database before
** this routine was called, then set *pRes=1 if pRes!=NULL.
*/
int sqliteBtreeNext(BtCursor *pCur, int *pRes)
{
  int rc;
  if (pCur->pPage == 0)
  {
    if (pRes)
      *pRes = 1;
    return SQLITE_ABORT;
  }
  if (pCur->bSkipNext && pCur->idx < pCur->pPage->nCell)
  {
    pCur->bSkipNext = 0;
    if (pRes)
      *pRes = 0;
    return SQLITE_OK;
  }
  pCur->idx++;
  if (pCur->idx >= pCur->pPage->nCell)
  {
    if (pCur->pPage->u.hdr.rightChild)
    {
      rc = moveToChild(pCur, pCur->pPage->u.hdr.rightChild);
      if (rc)
        return rc;
      rc = moveToLeftmost(pCur);
      if (rc)
        return rc;
      if (pRes)
        *pRes = 0;
      return SQLITE_OK;
    }
    do
    {
      if (pCur->pPage->pParent == 0)
      {
        if (pRes)
          *pRes = 1;
        return SQLITE_OK;
      }
      rc = moveToParent(pCur);
      if (rc)
        return rc;
    } while (pCur->idx >= pCur->pPage->nCell);
    if (pRes)
      *pRes = 0;
    return SQLITE_OK;
  }
  rc = moveToLeftmost(pCur);
  if (rc)
    return rc;
  if (pRes)
    *pRes = 0;
  return SQLITE_OK;
}

/*
** Allocate a new page from the database file.
** 从数据库文件中分配一个新的Page
**
** The new page is marked as dirty.  (In other words, sqlitepager_write()
** has already been called on the new page.)  The new page has also
** been referenced and the calling routine is responsible for calling
** sqlitepager_unref() on the new page when it is done.
** 新的Page被标记为脏页。(已经对这个Page调用了sqlitepager_write()方法).
** 新的Page也已被引用,调用程序负责在新的Page上调用sqlitepager_unref()
**
** SQLITE_OK is returned on success.  Any other return value indicates
** an error.  *ppPage and *pPgno are undefined in the event of an error.
** Do not invoke sqlitepager_unref() on *ppPage if an error is returned.
*/
static int allocatePage(Btree *pBt, MemPage **ppPage, Pgno *pPgno)
{
  PageOne *pPage1 = pBt->page1;
  int rc;
  // printf("allocatePage - pPage1.freeList =>> %d\n", pPage1->freeList);
  if (pPage1->freeList)
  {
    printf("是否溢出?");
    OverflowPage *pOvfl;
    FreelistInfo *pInfo;

    rc = sqlitepager_write(pPage1);
    if (rc)
      return rc;
    pPage1->nFree--;
    printf("[allocatePage] pPage1->freeList ==> %d \n", pPage1->freeList);
    rc = sqlitepager_get(pBt->pPager, pPage1->freeList, (void **)&pOvfl);
    if (rc)
      return rc;
    rc = sqlitepager_write(pOvfl);
    if (rc)
    {
      sqlitepager_unref(pOvfl);
      return rc;
    }
    pInfo = (FreelistInfo *)pOvfl->aPayload;
    if (pInfo->nFree == 0)
    {
      *pPgno = pPage1->freeList;
      pPage1->freeList = pOvfl->iNext;
      *ppPage = (MemPage *)pOvfl;
    }
    else
    {
      pInfo->nFree--;
      *pPgno = pInfo->aFree[pInfo->nFree];
      rc = sqlitepager_get(pBt->pPager, *pPgno, (void **)ppPage);
      sqlitepager_unref(pOvfl);
      if (rc == SQLITE_OK)
      {
        sqlitepager_dont_rollback(*ppPage);
        rc = sqlitepager_write(*ppPage);
      }
    }
  }
  else
  {
    // 获取当前pPgaer的Page个数
    *pPgno = sqlitepager_pagecount(pBt->pPager) + 1;
    printf("分配一个新的Page,pgno:%d \n", *pPgno);
    rc = sqlitepager_get(pBt->pPager, *pPgno, (void **)ppPage);
    if (rc)
      return rc;
    rc = sqlitepager_write(*ppPage);
  }
  return rc;
}

/*
** Add a page of the database file to the freelist.  Either pgno or
** pPage but not both may be 0.
**
** sqlitepager_unref() is NOT called for pPage.
*/
static int freePage(Btree *pBt, void *pPage, Pgno pgno)
{
  PageOne *pPage1 = pBt->page1;
  OverflowPage *pOvfl = (OverflowPage *)pPage;
  int rc;
  int needUnref = 0;
  MemPage *pMemPage;

  if (pgno == 0)
  {
    assert(pOvfl != 0);
    pgno = sqlitepager_pagenumber(pOvfl);
  }
  assert(pgno > 2);
  rc = sqlitepager_write(pPage1);
  if (rc)
  {
    return rc;
  }
  pPage1->nFree++;
  if (pPage1->nFree > 0 && pPage1->freeList)
  {
    OverflowPage *pFreeIdx;
    rc = sqlitepager_get(pBt->pPager, pPage1->freeList, (void **)&pFreeIdx);
    if (rc == SQLITE_OK)
    {
      FreelistInfo *pInfo = (FreelistInfo *)pFreeIdx->aPayload;
      if (pInfo->nFree < (sizeof(pInfo->aFree) / sizeof(pInfo->aFree[0])))
      {
        rc = sqlitepager_write(pFreeIdx);
        if (rc == SQLITE_OK)
        {
          pInfo->aFree[pInfo->nFree] = pgno;
          pInfo->nFree++;
          sqlitepager_unref(pFreeIdx);
          sqlitepager_dont_write(pBt->pPager, pgno);
          return rc;
        }
      }
      sqlitepager_unref(pFreeIdx);
    }
  }
  if (pOvfl == 0)
  {
    assert(pgno > 0);
    rc = sqlitepager_get(pBt->pPager, pgno, (void **)&pOvfl);
    if (rc)
      return rc;
    needUnref = 1;
  }
  rc = sqlitepager_write(pOvfl);
  if (rc)
  {
    if (needUnref)
      sqlitepager_unref(pOvfl);
    return rc;
  }
  pOvfl->iNext = pPage1->freeList;
  pPage1->freeList = pgno;
  memset(pOvfl->aPayload, 0, OVERFLOW_SIZE);
  pMemPage = (MemPage *)pPage;
  pMemPage->isInit = 0;
  if (pMemPage->pParent)
  {
    sqlitepager_unref(pMemPage->pParent);
    pMemPage->pParent = 0;
  }
  if (needUnref)
    rc = sqlitepager_unref(pOvfl);
  return rc;
}

/*
** Erase all the data out of a cell.  This involves returning overflow
** pages back the freelist.
** 擦除单元格内的错有数据。这会将溢出页面返回到空闲列表
*/
static int clearCell(Btree *pBt, Cell *pCell)
{
  Pager *pPager = pBt->pPager;
  OverflowPage *pOvfl;
  Pgno ovfl, nextOvfl;
  int rc;

  if (NKEY(pCell->h) + NDATA(pCell->h) <= MX_LOCAL_PAYLOAD)
  {
    return SQLITE_OK;
  }
  ovfl = pCell->ovfl;
  pCell->ovfl = 0;
  while (ovfl)
  {
    rc = sqlitepager_get(pPager, ovfl, (void **)&pOvfl);
    if (rc)
      return rc;
    nextOvfl = pOvfl->iNext;
    rc = freePage(pBt, pOvfl, ovfl);
    if (rc)
      return rc;
    sqlitepager_unref(pOvfl);
    ovfl = nextOvfl;
  }
  return SQLITE_OK;
}

/*
** Create a new cell from key and data.  Overflow pages are allocated as
** necessary and linked to this cell.
** 根据给定的key/value创建一个新的Cell.
** 溢出Page根据需要分配并链接到这个cell
**
*/
static int fillInCell(
    Btree *pBt,                  /* The whole Btree.  Needed to allocate pages */
    Cell *pCell,                 /* Populate this Cell structure */
    const void *pKey, int nKey,  /* The key */
    const void *pData, int nData /* The data */
)
{
  OverflowPage *pOvfl, *pPrior;
  Pgno *pNext;
  int spaceLeft;
  int n, rc;
  int nPayload;
  const char *pPayload;
  char *pSpace;

  // 设置CellHdr
  pCell->h.leftChild = 0;
  // key的长度最长为16字节
  pCell->h.nKey = nKey & 0xffff;
  pCell->h.nKeyHi = nKey >> 16;
  // data的长度最长为16字节
  pCell->h.nData = nData & 0xffff;
  pCell->h.nDataHi = nData >> 16;
  pCell->h.iNext = 0;

  pNext = &pCell->ovfl;
  pSpace = pCell->aPayload;

  // 这个Cell能保存的最大数据
  spaceLeft = MX_LOCAL_PAYLOAD;
  pPayload = pKey;
  pKey = 0;
  nPayload = nKey;
  pPrior = 0;
  while (nPayload > 0)
  {
    printf("[fillInCell] spaceLeft =====> %d\n", spaceLeft);
    if (spaceLeft == 0)
    {
      /**
       * 溢出页面与Cell相对应,当一条记录(包含key和data)无法完全保存在Cell中的时候,
       * 则分配一个Page作为溢出页面,这个页面的空闲会比Cell大得多(1020).
       */
      // 游标指向的Page空间耗尽，需要分配一个新的Page
      rc = allocatePage(pBt, (MemPage **)&pOvfl, pNext);
      if (rc)
      {
        *pNext = 0;
      }
      if (pPrior)
        sqlitepager_unref(pPrior);
      if (rc)
      {
        clearCell(pBt, pCell);
        return rc;
      }
      pPrior = pOvfl;
      spaceLeft = OVERFLOW_SIZE;
      pSpace = pOvfl->aPayload;
      pNext = &pOvfl->iNext;
      printf("[fillInCell] OVERFLOW_SIZE ====> %d, pOvfl->aPayload ===> %p\n", spaceLeft, pSpace);
    }

    n = nPayload;
    if (n > spaceLeft) // 如果key的长度超过spaceLeft,则固定为spaceLeft，多余的部分放到溢出Page
      n = spaceLeft;
    // 将key复制到cell的payload中
    memcpy(pSpace, pPayload, n);
    // 检查是否有剩余的key没有被写入
    nPayload -= n;
    if (nPayload == 0 && pData)
    {
      // 全部写入成功,将nPayload和pPayload指向data
      pPayload = pData;
      nPayload = nData;
      pData = 0;
    }
    else
    {
      // 还有剩余的key没写入,将pPayload指针移动到剩余的部分
      // nPayload已经在前面减去n了
      // 现在pPayload和nPayload都指向key的剩余数据段与剩余长度
      pPayload += n;
    }

    // 剩余空间减去n
    spaceLeft -= n;
    // 一口空间加上n
    pSpace += n;
  }
  *pNext = 0;
  if (pPrior)
  {
    sqlitepager_unref(pPrior);
  }
  return SQLITE_OK;
}

/*
** Change the MemPage.pParent pointer on the page whose number is
** given in the second argument so that MemPage.pParent holds the
** pointer in the third argument.
*/
static void reparentPage(Pager *pPager, Pgno pgno, MemPage *pNewParent)
{
  MemPage *pThis;

  if (pgno == 0)
    return;
  assert(pPager != 0);
  pThis = sqlitepager_lookup(pPager, pgno);
  if (pThis && pThis->isInit)
  {
    if (pThis->pParent != pNewParent)
    {
      if (pThis->pParent)
        sqlitepager_unref(pThis->pParent);
      pThis->pParent = pNewParent;
      if (pNewParent)
        sqlitepager_ref(pNewParent);
    }
    sqlitepager_unref(pThis);
  }
}

/*
** Reparent all children of the given page to be the given page.
** In other words, for every child of pPage, invoke reparentPage()
** to make sure that each child knows that pPage is its parent.
**
** This routine gets called after you memcpy() one page into
** another.
*/
static void reparentChildPages(Pager *pPager, MemPage *pPage)
{
  int i;
  for (i = 0; i < pPage->nCell; i++)
  {
    reparentPage(pPager, pPage->apCell[i]->h.leftChild, pPage);
  }
  reparentPage(pPager, pPage->u.hdr.rightChild, pPage);
}

/*
** Remove the i-th cell from pPage.  This routine effects pPage only.
** The cell content is not freed or deallocated.  It is assumed that
** the cell content has been copied someplace else.  This routine just
** removes the reference to the cell from pPage.
**
** "sz" must be the number of bytes in the cell.
**
** Do not bother maintaining the integrity of the linked list of Cells.
** Only the pPage->apCell[] array is important.  The relinkCellList()
** routine will be called soon after this routine in order to rebuild
** the linked list.
*/
static void dropCell(MemPage *pPage, int idx, int sz)
{
  int j;
  assert(idx >= 0 && idx < pPage->nCell);
  assert(sz == cellSize(pPage->apCell[idx]));
  assert(sqlitepager_iswriteable(pPage));
  freeSpace(pPage, Addr(pPage->apCell[idx]) - Addr(pPage), sz);
  for (j = idx; j < pPage->nCell - 1; j++)
  {
    pPage->apCell[j] = pPage->apCell[j + 1];
  }
  pPage->nCell--;
}

/*
** Insert a new cell on pPage at cell index "i".  pCell points to the
** content of the cell.
** 在pPage上的单元格索引i处插入一个新的单元格。pCell指向单元格的内容
**
** If the cell content will fit on the page, then put it there.  If it
** will not fit, then just make pPage->apCell[i] point to the content
** and set pPage->isOverfull.
**
** Do not bother maintaining the integrity of the linked list of Cells.
** Only the pPage->apCell[] array is important.  The relinkCellList()
** routine will be called soon after this routine in order to rebuild
** the linked list.
*/
static void insertCell(MemPage *pPage, int i, Cell *pCell, int sz)
{
  int idx, j;
  assert(i >= 0 && i <= pPage->nCell);
  assert(sz == cellSize(pCell));
  assert(sqlitepager_iswriteable(pPage));
  // 在这个Page中分配Cell空间(包括溢出页面的pgno，不包括溢出页面的数据)
  idx = allocateSpace(pPage, sz);
  for (j = pPage->nCell; j > i; j--)
  {
    pPage->apCell[j] = pPage->apCell[j - 1];
  }
  pPage->nCell++;
  if (idx <= 0)
  {
    pPage->isOverfull = 1;
    pPage->apCell[i] = pCell;
  }
  else
  {
    // 将cell写入到page数据中,aDisk就是实际存储在磁盘上的Page数据
    memcpy(&pPage->u.aDisk[idx], pCell, sz);
    // 在apCell中将cell对应的序号指向aDisk中的数据内容
    // 可以根据Page中某个Cell的序号,通过apCell[i]获取到Cell在aDisk中的起始地址
    // 然后可以在aDisk中获取到Cell
    pPage->apCell[i] = (Cell *)&pPage->u.aDisk[idx];
  }
}

/*
** Rebuild the linked list of cells on a page so that the cells
** occur in the order specified by the pPage->apCell[] array.
** Invoke this routine once to repair damage after one or more
** invocations of either insertCell() or dropCell().
*/
static void relinkCellList(MemPage *pPage)
{
  int i;
  u16 *pIdx;
  assert(sqlitepager_iswriteable(pPage));
  pIdx = &pPage->u.hdr.firstCell;
  for (i = 0; i < pPage->nCell; i++)
  {
    int idx = Addr(pPage->apCell[i]) - Addr(pPage);
    assert(idx > 0 && idx < SQLITE_PAGE_SIZE);
    *pIdx = idx;
    pIdx = &pPage->apCell[i]->h.iNext;
  }
  *pIdx = 0;
}

/*
** Make a copy of the contents of pFrom into pTo.  The pFrom->apCell[]
** pointers that point into pFrom->u.aDisk[] must be adjusted to point
** into pTo->u.aDisk[] instead.  But some pFrom->apCell[] entries might
** not point to pFrom->u.aDisk[].  Those are unchanged.
*/
static void copyPage(MemPage *pTo, MemPage *pFrom)
{
  uptr from, to;
  int i;
  memcpy(pTo->u.aDisk, pFrom->u.aDisk, SQLITE_PAGE_SIZE);
  pTo->pParent = 0;
  pTo->isInit = 1;
  pTo->nCell = pFrom->nCell;
  pTo->nFree = pFrom->nFree;
  pTo->isOverfull = pFrom->isOverfull;
  to = Addr(pTo);
  from = Addr(pFrom);
  for (i = 0; i < pTo->nCell; i++)
  {
    uptr x = Addr(pFrom->apCell[i]);
    if (x > from && x < from + SQLITE_PAGE_SIZE)
    {
      *((uptr *)&pTo->apCell[i]) = x + to - from;
    }
    else
    {
      pTo->apCell[i] = pFrom->apCell[i];
    }
  }
}

/*
** This routine redistributes Cells on pPage and up to two siblings
** of pPage so that all pages have about the same amount of free space.
** Usually one sibling on either side of pPage is used in the balancing,
** though both siblings might come from one side if pPage is the first
** or last child of its parent.  If pPage has fewer than two siblings
** (something which can only happen if pPage is the root page or a
** child of root) then all available siblings participate in the balancing.
**
** The number of siblings of pPage might be increased or decreased by
** one in an effort to keep pages between 66% and 100% full. The root page
** is special and is allowed to be less than 66% full. If pPage is
** the root page, then the depth of the tree might be increased
** or decreased by one, as necessary, to keep the root page from being
** overfull or empty.
**
** This routine calls relinkCellList() on its input page regardless of
** whether or not it does any real balancing.  Client routines will typically
** invoke insertCell() or dropCell() before calling this routine, so we
** need to call relinkCellList() to clean up the mess that those other
** routines left behind.
**
** pCur is left pointing to the same cell as when this routine was called
** even if that cell gets moved to a different page.  pCur may be NULL.
** Set the pCur parameter to NULL if you do not care about keeping track
** of a cell as that will save this routine the work of keeping track of it.
**
** Note that when this routine is called, some of the Cells on pPage
** might not actually be stored in pPage->u.aDisk[].  This can happen
** if the page is overfull.  Part of the job of this routine is to
** make sure all Cells for pPage once again fit in pPage->u.aDisk[].
**
** In the course of balancing the siblings of pPage, the parent of pPage
** might become overfull or underfull.  If that happens, then this routine
** is called recursively on the parent.
**
** If this routine fails for any reason, it might leave the database
** in a corrupted state.  So if this routine fails, the database should
** be rolled back.
*/
static int balance(Btree *pBt, MemPage *pPage, BtCursor *pCur)
{
  MemPage *pParent;              /* The parent of pPage */
  MemPage *apOld[3];             /* pPage and up to two siblings */
  Pgno pgnoOld[3];               /* Page numbers for each page in apOld[] */
  MemPage *apNew[4];             /* pPage and up to 3 siblings after balancing */
  Pgno pgnoNew[4];               /* Page numbers for each page in apNew[] */
  int idxDiv[3];                 /* Indices of divider cells in pParent */
  Cell *apDiv[3];                /* Divider cells in pParent */
  int nCell;                     /* Number of cells in apCell[] */
  int nOld;                      /* Number of pages in apOld[] */
  int nNew;                      /* Number of pages in apNew[] */
  int nDiv;                      /* Number of cells in apDiv[] */
  int i, j, k;                   /* Loop counters */
  int idx;                       /* Index of pPage in pParent->apCell[] */
  int nxDiv;                     /* Next divider slot in pParent->apCell[] */
  int rc;                        /* The return code */
  int iCur;                      /* apCell[iCur] is the cell of the cursor */
  MemPage *pOldCurPage;          /* The cursor originally points to this page */
  int totalSize;                 /* Total bytes for all cells */
  int subtotal;                  /* Subtotal of bytes in cells on one page */
  int cntNew[4];                 /* Index in apCell[] of cell after i-th page */
  int szNew[4];                  /* Combined size of cells place on i-th page */
  MemPage *extraUnref = 0;       /* A page that needs to be unref-ed */
  Pgno pgno;                     /* Page number */
  Cell *apCell[MX_CELL * 3 + 5]; /* All cells from pages being balanceed */
  int szCell[MX_CELL * 3 + 5];   /* Local size of all cells */
  Cell aTemp[2];                 /* Temporary holding area for apDiv[] */
  MemPage aOld[3];               /* Temporary copies of pPage and its siblings */

  /*
  ** Return without doing any work if pPage is neither overfull nor
  ** underfull.
  */
  assert(sqlitepager_iswriteable(pPage));
  if (!pPage->isOverfull && pPage->nFree < SQLITE_PAGE_SIZE / 2 && pPage->nCell >= 2)
  {
    relinkCellList(pPage);
    return SQLITE_OK;
  }

  /*
  ** Find the parent of the page to be balanceed.
  ** If there is no parent, it means this page is the root page and
  ** special rules apply.
  */
  pParent = pPage->pParent;
  if (pParent == 0)
  {
    Pgno pgnoChild;
    MemPage *pChild;
    if (pPage->nCell == 0)
    {
      if (pPage->u.hdr.rightChild)
      {
        /*
        ** The root page is empty.  Copy the one child page
        ** into the root page and return.  This reduces the depth
        ** of the BTree by one.
        */
        pgnoChild = pPage->u.hdr.rightChild;
        rc = sqlitepager_get(pBt->pPager, pgnoChild, (void **)&pChild);
        if (rc)
          return rc;
        memcpy(pPage, pChild, SQLITE_PAGE_SIZE);
        pPage->isInit = 0;
        rc = initPage(pPage, sqlitepager_pagenumber(pPage), 0);
        assert(rc == SQLITE_OK);
        reparentChildPages(pBt->pPager, pPage);
        if (pCur && pCur->pPage == pChild)
        {
          sqlitepager_unref(pChild);
          pCur->pPage = pPage;
          sqlitepager_ref(pPage);
        }
        freePage(pBt, pChild, pgnoChild);
        sqlitepager_unref(pChild);
      }
      else
      {
        relinkCellList(pPage);
      }
      return SQLITE_OK;
    }
    if (!pPage->isOverfull)
    {
      /* It is OK for the root page to be less than half full.
       */
      relinkCellList(pPage);
      return SQLITE_OK;
    }
    /*
    ** If we get to here, it means the root page is overfull.
    ** When this happens, Create a new child page and copy the
    ** contents of the root into the child.  Then make the root
    ** page an empty page with rightChild pointing to the new
    ** child.  Then fall thru to the code below which will cause
    ** the overfull child page to be split.
    */
    rc = sqlitepager_write(pPage);
    if (rc)
      return rc;
    rc = allocatePage(pBt, &pChild, &pgnoChild);
    if (rc)
      return rc;
    assert(sqlitepager_iswriteable(pChild));
    copyPage(pChild, pPage);
    pChild->pParent = pPage;
    sqlitepager_ref(pPage);
    pChild->isOverfull = 1;
    if (pCur && pCur->pPage == pPage)
    {
      sqlitepager_unref(pPage);
      pCur->pPage = pChild;
    }
    else
    {
      extraUnref = pChild;
    }
    zeroPage(pPage);
    pPage->u.hdr.rightChild = pgnoChild;
    pParent = pPage;
    pPage = pChild;
  }
  rc = sqlitepager_write(pParent);
  if (rc)
    return rc;

  /*
  ** Find the Cell in the parent page whose h.leftChild points back
  ** to pPage.  The "idx" variable is the index of that cell.  If pPage
  ** is the rightmost child of pParent then set idx to pParent->nCell
  */
  idx = -1;
  pgno = sqlitepager_pagenumber(pPage);
  for (i = 0; i < pParent->nCell; i++)
  {
    if (pParent->apCell[i]->h.leftChild == pgno)
    {
      idx = i;
      break;
    }
  }
  if (idx < 0 && pParent->u.hdr.rightChild == pgno)
  {
    idx = pParent->nCell;
  }
  if (idx < 0)
  {
    return SQLITE_CORRUPT;
  }

  /*
  ** Initialize variables so that it will be safe to jump
  ** directly to balance_cleanup at any moment.
  */
  nOld = nNew = 0;
  sqlitepager_ref(pParent);

  /*
  ** Find sibling pages to pPage and the Cells in pParent that divide
  ** the siblings.  An attempt is made to find one sibling on either
  ** side of pPage.  Both siblings are taken from one side, however, if
  ** pPage is either the first or last child of its parent.  If pParent
  ** has 3 or fewer children then all children of pParent are taken.
  */
  if (idx == pParent->nCell)
  {
    nxDiv = idx - 2;
  }
  else
  {
    nxDiv = idx - 1;
  }
  if (nxDiv < 0)
    nxDiv = 0;
  nDiv = 0;
  for (i = 0, k = nxDiv; i < 3; i++, k++)
  {
    if (k < pParent->nCell)
    {
      idxDiv[i] = k;
      apDiv[i] = pParent->apCell[k];
      nDiv++;
      pgnoOld[i] = apDiv[i]->h.leftChild;
    }
    else if (k == pParent->nCell)
    {
      pgnoOld[i] = pParent->u.hdr.rightChild;
    }
    else
    {
      break;
    }
    rc = sqlitepager_get(pBt->pPager, pgnoOld[i], (void **)&apOld[i]);
    if (rc)
      goto balance_cleanup;
    rc = initPage(apOld[i], pgnoOld[i], pParent);
    if (rc)
      goto balance_cleanup;
    nOld++;
  }

  /*
  ** Set iCur to be the index in apCell[] of the cell that the cursor
  ** is pointing to.  We will need this later on in order to keep the
  ** cursor pointing at the same cell.  If pCur points to a page that
  ** has no involvement with this rebalancing, then set iCur to a large
  ** number so that the iCur==j tests always fail in the main cell
  ** distribution loop below.
  */
  if (pCur)
  {
    iCur = 0;
    for (i = 0; i < nOld; i++)
    {
      if (pCur->pPage == apOld[i])
      {
        iCur += pCur->idx;
        break;
      }
      iCur += apOld[i]->nCell;
      if (i < nOld - 1 && pCur->pPage == pParent && pCur->idx == idxDiv[i])
      {
        break;
      }
      iCur++;
    }
    pOldCurPage = pCur->pPage;
  }

  /*
  ** Make copies of the content of pPage and its siblings into aOld[].
  ** The rest of this function will use data from the copies rather
  ** that the original pages since the original pages will be in the
  ** process of being overwritten.
  */
  for (i = 0; i < nOld; i++)
  {
    copyPage(&aOld[i], apOld[i]);
    rc = freePage(pBt, apOld[i], pgnoOld[i]);
    if (rc)
      goto balance_cleanup;
    sqlitepager_unref(apOld[i]);
    apOld[i] = &aOld[i];
  }

  /*
  ** Load pointers to all cells on sibling pages and the divider cells
  ** into the local apCell[] array.  Make copies of the divider cells
  ** into aTemp[] and remove the the divider Cells from pParent.
  */
  nCell = 0;
  for (i = 0; i < nOld; i++)
  {
    MemPage *pOld = apOld[i];
    for (j = 0; j < pOld->nCell; j++)
    {
      apCell[nCell] = pOld->apCell[j];
      szCell[nCell] = cellSize(apCell[nCell]);
      nCell++;
    }
    if (i < nOld - 1)
    {
      szCell[nCell] = cellSize(apDiv[i]);
      memcpy(&aTemp[i], apDiv[i], szCell[nCell]);
      apCell[nCell] = &aTemp[i];
      dropCell(pParent, nxDiv, szCell[nCell]);
      assert(apCell[nCell]->h.leftChild == pgnoOld[i]);
      apCell[nCell]->h.leftChild = pOld->u.hdr.rightChild;
      nCell++;
    }
  }

  /*
  ** Figure out the number of pages needed to hold all nCell cells.
  ** Store this number in "k".  Also compute szNew[] which is the total
  ** size of all cells on the i-th page and cntNew[] which is the index
  ** in apCell[] of the cell that divides path i from path i+1.
  ** cntNew[k] should equal nCell.
  **
  ** This little patch of code is critical for keeping the tree
  ** balanced.
  */
  totalSize = 0;
  for (i = 0; i < nCell; i++)
  {
    totalSize += szCell[i];
  }
  for (subtotal = k = i = 0; i < nCell; i++)
  {
    subtotal += szCell[i];
    if (subtotal > USABLE_SPACE)
    {
      szNew[k] = subtotal - szCell[i];
      cntNew[k] = i;
      subtotal = 0;
      k++;
    }
  }
  szNew[k] = subtotal;
  cntNew[k] = nCell;
  k++;
  for (i = k - 1; i > 0; i--)
  {
    while (szNew[i] < USABLE_SPACE / 2)
    {
      cntNew[i - 1]--;
      assert(cntNew[i - 1] > 0);
      szNew[i] += szCell[cntNew[i - 1]];
      szNew[i - 1] -= szCell[cntNew[i - 1] - 1];
    }
  }
  assert(cntNew[0] > 0);

  /*
  ** Allocate k new pages
  */
  for (i = 0; i < k; i++)
  {
    rc = allocatePage(pBt, &apNew[i], &pgnoNew[i]);
    if (rc)
      goto balance_cleanup;
    nNew++;
    zeroPage(apNew[i]);
    apNew[i]->isInit = 1;
  }

  /*
  ** Put the new pages in accending order.  This helps to
  ** keep entries in the disk file in order so that a scan
  ** of the table is a linear scan through the file.  That
  ** in turn helps the operating system to deliver pages
  ** from the disk more rapidly.
  **
  ** An O(n^2) insertion sort algorithm is used, but since
  ** n is never more than 3, that should not be a problem.
  **
  ** This one optimization makes the database about 25%
  ** faster for large insertions and deletions.
  */
  for (i = 0; i < k - 1; i++)
  {
    int minV = pgnoNew[i];
    int minI = i;
    for (j = i + 1; j < k; j++)
    {
      if (pgnoNew[j] < minV)
      {
        minI = j;
        minV = pgnoNew[j];
      }
    }
    if (minI > i)
    {
      int t;
      MemPage *pT;
      t = pgnoNew[i];
      pT = apNew[i];
      pgnoNew[i] = pgnoNew[minI];
      apNew[i] = apNew[minI];
      pgnoNew[minI] = t;
      apNew[minI] = pT;
    }
  }

  /*
  ** Evenly distribute the data in apCell[] across the new pages.
  ** Insert divider cells into pParent as necessary.
  */
  j = 0;
  for (i = 0; i < nNew; i++)
  {
    MemPage *pNew = apNew[i];
    while (j < cntNew[i])
    {
      assert(pNew->nFree >= szCell[j]);
      if (pCur && iCur == j)
      {
        pCur->pPage = pNew;
        pCur->idx = pNew->nCell;
      }
      insertCell(pNew, pNew->nCell, apCell[j], szCell[j]);
      j++;
    }
    assert(pNew->nCell > 0);
    assert(!pNew->isOverfull);
    relinkCellList(pNew);
    if (i < nNew - 1 && j < nCell)
    {
      pNew->u.hdr.rightChild = apCell[j]->h.leftChild;
      apCell[j]->h.leftChild = pgnoNew[i];
      if (pCur && iCur == j)
      {
        pCur->pPage = pParent;
        pCur->idx = nxDiv;
      }
      insertCell(pParent, nxDiv, apCell[j], szCell[j]);
      j++;
      nxDiv++;
    }
  }
  assert(j == nCell);
  apNew[nNew - 1]->u.hdr.rightChild = apOld[nOld - 1]->u.hdr.rightChild;
  if (nxDiv == pParent->nCell)
  {
    pParent->u.hdr.rightChild = pgnoNew[nNew - 1];
  }
  else
  {
    pParent->apCell[nxDiv]->h.leftChild = pgnoNew[nNew - 1];
  }
  if (pCur)
  {
    if (j <= iCur && pCur->pPage == pParent && pCur->idx > idxDiv[nOld - 1])
    {
      assert(pCur->pPage == pOldCurPage);
      pCur->idx += nNew - nOld;
    }
    else
    {
      assert(pOldCurPage != 0);
      sqlitepager_ref(pCur->pPage);
      sqlitepager_unref(pOldCurPage);
    }
  }

  /*
  ** Reparent children of all cells.
  */
  for (i = 0; i < nNew; i++)
  {
    reparentChildPages(pBt->pPager, apNew[i]);
  }
  reparentChildPages(pBt->pPager, pParent);

  /*
  ** balance the parent page.
  */
  rc = balance(pBt, pParent, pCur);

  /*
  ** Cleanup before returning.
  */
balance_cleanup:
  if (extraUnref)
  {
    sqlitepager_unref(extraUnref);
  }
  for (i = 0; i < nOld; i++)
  {
    if (apOld[i] != &aOld[i])
      sqlitepager_unref(apOld[i]);
  }
  for (i = 0; i < nNew; i++)
  {
    sqlitepager_unref(apNew[i]);
  }
  if (pCur && pCur->pPage == 0)
  {
    pCur->pPage = pParent;
    pCur->idx = 0;
  }
  else
  {
    sqlitepager_unref(pParent);
  }
  return rc;
}

/*
** Insert a new record into the BTree.  The key is given by (pKey,nKey)
** and the data is given by (pData,nData).
**
** The cursor is used only to define what database
** the record should be inserted into.  The cursor
** is left pointing at the new record.
** 游标仅用于定义记录应该插入的数据库。
** 游标左指针指向这条新的记录
*/
int sqliteBtreeInsert(
    BtCursor *pCur,              /* Insert data into the table of this cursor | 向该游标指向的Table插入数据 */
    const void *pKey, int nKey,  /* The key of the new record | key的数据指针以及数据长度 */
    const void *pData, int nData /* The data of the new record | value的数据指针以及数据长度 */
)
{
  Cell newCell;
  int rc;
  int loc;
  int szNew;
  MemPage *pPage;
  Btree *pBt = pCur->pBt;

  if (pCur->pPage == 0)
  {
    // rollback 破坏了这个游标,该游标没有指向具体的Page
    return SQLITE_ABORT; /* A rollback destroyed this cursor */
  }

  if (!pCur->pBt->inTrans || nKey + nData == 0)
  {
    // 尚未开启一个BTree事务
    return SQLITE_ERROR; /* Must start a transaction first */
  }

  if (!pCur->wrFlag)
  {
    // 这个游标不是读写游标,无法写入数据
    return SQLITE_PERM; /* Cursor not open for writing */
  }

  // 将光标移动到要写入的key附近的条目
  rc = sqliteBtreeMoveto(pCur, pKey, nKey, &loc);
  if (rc)
    return rc;
  pPage = pCur->pPage;
  rc = sqlitepager_write(pPage);
  if (rc)
    return rc;
  // 将数据写入到Cell中
  rc = fillInCell(pBt, &newCell, pKey, nKey, pData, nData);
  if (rc)
    return rc;
  // 获取这个cell在数据库空间上的大小(不包括溢出页面分配的空间)
  szNew = cellSize(&newCell);
  if (loc == 0)
  {
    // 光标指向与pKey相同的条目

    // 新的Cell替代旧的条目,并将原来的Cell清理掉
    newCell.h.leftChild = pPage->apCell[pCur->idx]->h.leftChild;

    rc = clearCell(pBt, pPage->apCell[pCur->idx]);
    if (rc)
      return rc;
    dropCell(pPage, pCur->idx, cellSize(pPage->apCell[pCur->idx]));
  }
  else if (loc < 0 && pPage->nCell > 0)
  {

    // 光标指向小于pKey的条目
    assert(pPage->u.hdr.rightChild == 0); /* Must be a leaf page|必须是一个叶子Page */
    pCur->idx++;
  }
  else
  {
    // 光标指向大于pKey的条目
    assert(pPage->u.hdr.rightChild == 0); /* Must be a leaf page|必须是一个叶子Page */
  }

  // 将Cell写入到Page中
  insertCell(pPage, pCur->idx, &newCell, szNew);
  rc = balance(pCur->pBt, pPage, pCur);
  /* sqliteBtreePageDump(pCur->pBt, pCur->pgnoRoot, 1); */
  /* fflush(stdout); */
  return rc;
}

/*
** Delete the entry that the cursor is pointing to.
**
** The cursor is left pointing at either the next or the previous
** entry.  If the cursor is left pointing to the next entry, then
** the pCur->bSkipNext flag is set which forces the next call to
** sqliteBtreeNext() to be a no-op.  That way, you can always call
** sqliteBtreeNext() after a delete and the cursor will be left
** pointing to the first entry after the deleted entry.
*/
int sqliteBtreeDelete(BtCursor *pCur)
{
  MemPage *pPage = pCur->pPage;
  Cell *pCell;
  int rc;
  Pgno pgnoChild;

  if (pCur->pPage == 0)
  {
    return SQLITE_ABORT; /* A rollback destroyed this cursor */
  }
  if (!pCur->pBt->inTrans)
  {
    return SQLITE_ERROR; /* Must start a transaction first */
  }
  if (pCur->idx >= pPage->nCell)
  {
    return SQLITE_ERROR; /* The cursor is not pointing to anything */
  }
  if (!pCur->wrFlag)
  {
    return SQLITE_PERM; /* Did not open this cursor for writing */
  }
  rc = sqlitepager_write(pPage);
  if (rc)
    return rc;
  pCell = pPage->apCell[pCur->idx];
  pgnoChild = pCell->h.leftChild;
  clearCell(pCur->pBt, pCell);
  if (pgnoChild)
  {
    /*
    ** The entry we are about to delete is not a leaf so if we do not
    ** do something we will leave a hole on an internal page.
    ** We have to fill the hole by moving in a cell from a leaf.  The
    ** next Cell after the one to be deleted is guaranteed to exist and
    ** to be a leaf so we can use it.
    */
    BtCursor leafCur;
    Cell *pNext;
    int szNext;
    getTempCursor(pCur, &leafCur);
    rc = sqliteBtreeNext(&leafCur, 0);
    if (rc != SQLITE_OK)
    {
      return SQLITE_CORRUPT;
    }
    rc = sqlitepager_write(leafCur.pPage);
    if (rc)
      return rc;
    dropCell(pPage, pCur->idx, cellSize(pCell));
    pNext = leafCur.pPage->apCell[leafCur.idx];
    szNext = cellSize(pNext);
    pNext->h.leftChild = pgnoChild;
    insertCell(pPage, pCur->idx, pNext, szNext);
    rc = balance(pCur->pBt, pPage, pCur);
    if (rc)
      return rc;
    pCur->bSkipNext = 1;
    dropCell(leafCur.pPage, leafCur.idx, szNext);
    rc = balance(pCur->pBt, leafCur.pPage, pCur);
    releaseTempCursor(&leafCur);
  }
  else
  {
    dropCell(pPage, pCur->idx, cellSize(pCell));
    if (pCur->idx >= pPage->nCell)
    {
      pCur->idx = pPage->nCell - 1;
      if (pCur->idx < 0)
      {
        pCur->idx = 0;
        pCur->bSkipNext = 1;
      }
      else
      {
        pCur->bSkipNext = 0;
      }
    }
    else
    {
      pCur->bSkipNext = 1;
    }
    rc = balance(pCur->pBt, pPage, pCur);
  }
  return rc;
}

/*
** Create a new BTree table.  Write into *piTable the page
** number for the root page of the new table.
** 创建一个新的BTree表.
** 写入到piTable指针的是这个新table的root页面编号
**
** In the current implementation, BTree tables and BTree indices are the
** the same.  But in the future, we may change this so that BTree tables
** are restricted to having a 4-byte integer key and arbitrary data and
** BTree indices are restricted to having an arbitrary key and no data.
** 在当前实现中,BTree表和BTree索引是相同的。在未来，我们可能会改变这一点,
** 使BTree表被限制为4个字节整数key和任意数据，而BTree索引被限制为具有任意key并且没有数据
*/
int sqliteBtreeCreateTable(Btree *pBt, int *piTable)
{
  // root Page的结构实例,这个对象的内存是Page的data部分
  MemPage *pRoot;
  Pgno pgnoRoot;
  int rc;
  if (!pBt->inTrans)
  {
    return SQLITE_ERROR; /* Must start a transaction first */
  }
  if (pBt->readOnly)
  {
    return SQLITE_READONLY;
  }
  //分配一个新的Page,这里分配得到的Page将会作为BTree的root节点
  rc = allocatePage(pBt, &pRoot, &pgnoRoot);
  if (rc)
    return rc;
  assert(sqlitepager_iswriteable(pRoot));

  zeroPage(pRoot);
  sqlitepager_unref(pRoot);
  *piTable = (int)pgnoRoot;
  return SQLITE_OK;
}

/*
** Create a new BTree index.  Write into *piTable the page
** number for the root page of the new index.
**
** In the current implementation, BTree tables and BTree indices are the
** the same.  But in the future, we may change this so that BTree tables
** are restricted to having a 4-byte integer key and arbitrary data and
** BTree indices are restricted to having an arbitrary key and no data.
*/
int sqliteBtreeCreateIndex(Btree *pBt, int *piIndex)
{
  return sqliteBtreeCreateTable(pBt, piIndex);
}

/*
** Erase the given database page and all its children.  Return
** the page to the freelist.
*/
static int clearDatabasePage(Btree *pBt, Pgno pgno, int freePageFlag)
{
  MemPage *pPage;
  int rc;
  Cell *pCell;
  int idx;

  rc = sqlitepager_get(pBt->pPager, pgno, (void **)&pPage);
  if (rc)
    return rc;
  rc = sqlitepager_write(pPage);
  if (rc)
    return rc;
  idx = pPage->u.hdr.firstCell;
  while (idx > 0)
  {
    pCell = (Cell *)&pPage->u.aDisk[idx];
    idx = pCell->h.iNext;
    if (pCell->h.leftChild)
    {
      rc = clearDatabasePage(pBt, pCell->h.leftChild, 1);
      if (rc)
        return rc;
    }
    rc = clearCell(pBt, pCell);
    if (rc)
      return rc;
  }
  if (pPage->u.hdr.rightChild)
  {
    rc = clearDatabasePage(pBt, pPage->u.hdr.rightChild, 1);
    if (rc)
      return rc;
  }
  if (freePageFlag)
  {
    rc = freePage(pBt, pPage, pgno);
  }
  else
  {
    zeroPage(pPage);
  }
  sqlitepager_unref(pPage);
  return rc;
}

/*
** Delete all information from a single table in the database.
*/
int sqliteBtreeClearTable(Btree *pBt, int iTable)
{
  int rc;
  ptr nLock;
  if (!pBt->inTrans)
  {
    return SQLITE_ERROR; /* Must start a transaction first */
  }
  if (pBt->readOnly)
  {
    return SQLITE_READONLY;
  }
  nLock = (ptr)sqliteHashFind(&pBt->locks, 0, iTable);
  if (nLock)
  {
    return SQLITE_LOCKED;
  }
  rc = clearDatabasePage(pBt, (Pgno)iTable, 0);
  if (rc)
  {
    sqliteBtreeRollback(pBt);
  }
  return rc;
}

/*
** Erase all information in a table and add the root of the table to
** the freelist.  Except, the root of the principle table (the one on
** page 2) is never added to the freelist.
*/
int sqliteBtreeDropTable(Btree *pBt, int iTable)
{
  int rc;
  MemPage *pPage;
  if (!pBt->inTrans)
  {
    return SQLITE_ERROR; /* Must start a transaction first */
  }
  if (pBt->readOnly)
  {
    return SQLITE_READONLY;
  }
  rc = sqlitepager_get(pBt->pPager, (Pgno)iTable, (void **)&pPage);
  if (rc)
    return rc;
  rc = sqliteBtreeClearTable(pBt, iTable);
  if (rc)
    return rc;
  if (iTable > 2)
  {
    rc = freePage(pBt, pPage, iTable);
  }
  else
  {
    zeroPage(pPage);
  }
  sqlitepager_unref(pPage);
  return rc;
}

/*
** Read the meta-information out of a database file.
*/
int sqliteBtreeGetMeta(Btree *pBt, int *aMeta)
{
  PageOne *pP1;
  int rc;

  rc = sqlitepager_get(pBt->pPager, 1, (void **)&pP1);
  if (rc)
    return rc;
  aMeta[0] = pP1->nFree;
  memcpy(&aMeta[1], pP1->aMeta, sizeof(pP1->aMeta));
  sqlitepager_unref(pP1);
  return SQLITE_OK;
}

/*
** Write meta-information back into the database.
*/
int sqliteBtreeUpdateMeta(Btree *pBt, int *aMeta)
{
  PageOne *pP1;
  int rc;
  if (!pBt->inTrans)
  {
    return SQLITE_ERROR; /* Must start a transaction first */
  }
  if (pBt->readOnly)
  {
    return SQLITE_READONLY;
  }
  pP1 = pBt->page1;
  rc = sqlitepager_write(pP1);
  if (rc)
    return rc;
  memcpy(pP1->aMeta, &aMeta[1], sizeof(pP1->aMeta));
  return SQLITE_OK;
}

/******************************************************************************
** The complete implementation of the BTree subsystem is above this line.
** All the code the follows is for testing and troubleshooting the BTree
** subsystem.  None of the code that follows is used during normal operation.
******************************************************************************/

/*
** Print a disassembly of the given page on standard output.  This routine
** is used for debugging and testing only.
*/
#ifdef SQLITE_TEST
int sqliteBtreePageDump(Btree *pBt, int pgno, int recursive)
{
  int rc;
  MemPage *pPage;
  int i, j;
  int nFree;
  u16 idx;
  char range[20];
  unsigned char payload[20];
  rc = sqlitepager_get(pBt->pPager, (Pgno)pgno, (void **)&pPage);
  if (rc)
  {
    return rc;
  }
  if (recursive)
    printf("PAGE %d:\n", pgno);
  i = 0;
  idx = pPage->u.hdr.firstCell;
  while (idx > 0 && idx <= SQLITE_PAGE_SIZE - MIN_CELL_SIZE)
  {
    Cell *pCell = (Cell *)&pPage->u.aDisk[idx];
    int sz = cellSize(pCell);
    sprintf(range, "%d..%d", idx, idx + sz - 1);
    sz = NKEY(pCell->h) + NDATA(pCell->h);
    if (sz > sizeof(payload) - 1)
      sz = sizeof(payload) - 1;
    memcpy(payload, pCell->aPayload, sz);
    for (j = 0; j < sz; j++)
    {
      if (payload[j] < 0x20 || payload[j] > 0x7f)
        payload[j] = '.';
    }
    payload[sz] = 0;
    printf(
        "cell %2d: i=%-10s chld=%-4d nk=%-4d nd=%-4d payload=%s\n",
        i, range, (int)pCell->h.leftChild, NKEY(pCell->h), NDATA(pCell->h),
        payload);
    if (pPage->isInit && pPage->apCell[i] != pCell)
    {
      printf("**** apCell[%d] does not match on prior entry ****\n", i);
    }
    i++;
    idx = pCell->h.iNext;
  }
  if (idx != 0)
  {
    printf("ERROR: next cell index out of range: %d\n", idx);
  }
  printf("right_child: %d\n", pPage->u.hdr.rightChild);
  nFree = 0;
  i = 0;
  idx = pPage->u.hdr.firstFree;
  while (idx > 0 && idx < SQLITE_PAGE_SIZE)
  {
    FreeBlk *p = (FreeBlk *)&pPage->u.aDisk[idx];
    sprintf(range, "%d..%d", idx, idx + p->iSize - 1);
    nFree += p->iSize;
    printf("freeblock %2d: i=%-10s size=%-4d total=%d\n",
           i, range, p->iSize, nFree);
    idx = p->iNext;
    i++;
  }
  if (idx != 0)
  {
    printf("ERROR: next freeblock index out of range: %d\n", idx);
  }
  if (recursive && pPage->u.hdr.rightChild != 0)
  {
    idx = pPage->u.hdr.firstCell;
    while (idx > 0 && idx < SQLITE_PAGE_SIZE - MIN_CELL_SIZE)
    {
      Cell *pCell = (Cell *)&pPage->u.aDisk[idx];
      sqliteBtreePageDump(pBt, pCell->h.leftChild, 1);
      idx = pCell->h.iNext;
    }
    sqliteBtreePageDump(pBt, pPage->u.hdr.rightChild, 1);
  }
  sqlitepager_unref(pPage);
  return SQLITE_OK;
}
#endif

#ifdef SQLITE_TEST
/*
** Fill aResult[] with information about the entry and page that the
** cursor is pointing to.
**
**   aResult[0] =  The page number
**   aResult[1] =  The entry number
**   aResult[2] =  Total number of entries on this page
**   aResult[3] =  Size of this entry
**   aResult[4] =  Number of free bytes on this page
**   aResult[5] =  Number of free blocks on the page
**   aResult[6] =  Page number of the left child of this entry
**   aResult[7] =  Page number of the right child for the whole page
**
** This routine is used for testing and debugging only.
*/
int sqliteBtreeCursorDump(BtCursor *pCur, int *aResult)
{
  int cnt, idx;
  MemPage *pPage = pCur->pPage;
  aResult[0] = sqlitepager_pagenumber(pPage);
  aResult[1] = pCur->idx;
  aResult[2] = pPage->nCell;
  if (pCur->idx >= 0 && pCur->idx < pPage->nCell)
  {
    aResult[3] = cellSize(pPage->apCell[pCur->idx]);
    aResult[6] = pPage->apCell[pCur->idx]->h.leftChild;
  }
  else
  {
    aResult[3] = 0;
    aResult[6] = 0;
  }
  aResult[4] = pPage->nFree;
  cnt = 0;
  idx = pPage->u.hdr.firstFree;
  while (idx > 0 && idx < SQLITE_PAGE_SIZE)
  {
    cnt++;
    idx = ((FreeBlk *)&pPage->u.aDisk[idx])->iNext;
  }
  aResult[5] = cnt;
  aResult[7] = pPage->u.hdr.rightChild;
  return SQLITE_OK;
}
#endif

#ifdef SQLITE_TEST
/*
** Return the pager associated with a BTree.  This routine is used for
** testing and debugging only.
*/
Pager *sqliteBtreePager(Btree *pBt)
{
  return pBt->pPager;
}
#endif

/*
** This structure is passed around through all the sanity checking routines
** in order to keep track of some global state information.
*/
typedef struct IntegrityCk IntegrityCk;
struct IntegrityCk
{
  Btree *pBt;    /* The tree being checked out */
  Pager *pPager; /* The associated pager.  Also accessible by pBt->pPager */
  int nPage;     /* Number of pages in the database */
  int *anRef;    /* Number of times each page is referenced */
  int nTreePage; /* Number of BTree pages */
  int nByte;     /* Number of bytes of data stored on BTree pages */
  char *zErrMsg; /* An error message.  NULL of no errors seen. */
};

/*
** Append a message to the error message string.
*/
static void checkAppendMsg(IntegrityCk *pCheck, char *zMsg1, char *zMsg2)
{
  if (pCheck->zErrMsg)
  {
    char *zOld = pCheck->zErrMsg;
    pCheck->zErrMsg = 0;
    sqliteSetString(&pCheck->zErrMsg, zOld, "\n", zMsg1, zMsg2, 0);
    sqliteFree(zOld);
  }
  else
  {
    sqliteSetString(&pCheck->zErrMsg, zMsg1, zMsg2, 0);
  }
}

/*
** Add 1 to the reference count for page iPage.  If this is the second
** reference to the page, add an error message to pCheck->zErrMsg.
** Return 1 if there are 2 ore more references to the page and 0 if
** if this is the first reference to the page.
**
** Also check that the page number is in bounds.
*/
static int checkRef(IntegrityCk *pCheck, int iPage, char *zContext)
{
  if (iPage == 0)
    return 1;
  if (iPage > pCheck->nPage)
  {
    char zBuf[100];
    sprintf(zBuf, "invalid page number %d", iPage);
    checkAppendMsg(pCheck, zContext, zBuf);
    return 1;
  }
  if (pCheck->anRef[iPage] == 1)
  {
    char zBuf[100];
    sprintf(zBuf, "2nd reference to page %d", iPage);
    checkAppendMsg(pCheck, zContext, zBuf);
    return 1;
  }
  return (pCheck->anRef[iPage]++) > 1;
}

/*
** Check the integrity of the freelist or of an overflow page list.
** Verify that the number of pages on the list is N.
*/
static void checkList(
    IntegrityCk *pCheck, /* Integrity checking context */
    int isFreeList,      /* True for a freelist.  False for overflow page list */
    int iPage,           /* Page number for first page in the list */
    int N,               /* Expected number of pages in the list */
    char *zContext       /* Context for error messages */
)
{
  int i;
  char zMsg[100];
  while (N-- > 0)
  {
    OverflowPage *pOvfl;
    if (iPage < 1)
    {
      sprintf(zMsg, "%d pages missing from overflow list", N + 1);
      checkAppendMsg(pCheck, zContext, zMsg);
      break;
    }
    if (checkRef(pCheck, iPage, zContext))
      break;
    if (sqlitepager_get(pCheck->pPager, (Pgno)iPage, (void **)&pOvfl))
    {
      sprintf(zMsg, "failed to get page %d", iPage);
      checkAppendMsg(pCheck, zContext, zMsg);
      break;
    }
    if (isFreeList)
    {
      FreelistInfo *pInfo = (FreelistInfo *)pOvfl->aPayload;
      for (i = 0; i < pInfo->nFree; i++)
      {
        checkRef(pCheck, pInfo->aFree[i], zMsg);
      }
      N -= pInfo->nFree;
    }
    iPage = (int)pOvfl->iNext;
    sqlitepager_unref(pOvfl);
  }
}

/*
** Return negative if zKey1<zKey2.
** Return zero if zKey1==zKey2.
** Return positive if zKey1>zKey2.
*/
static int keyCompare(
    const char *zKey1, int nKey1,
    const char *zKey2, int nKey2)
{
  int min = nKey1 > nKey2 ? nKey2 : nKey1;
  int c = memcmp(zKey1, zKey2, min);
  if (c == 0)
  {
    c = nKey1 - nKey2;
  }
  return c;
}

/*
** Do various sanity checks on a single page of a tree.  Return
** the tree depth.  Root pages return 0.  Parents of root pages
** return 1, and so forth.
**
** These checks are done:
**
**      1.  Make sure that cells and freeblocks do not overlap
**          but combine to completely cover the page.
**      2.  Make sure cell keys are in order.
**      3.  Make sure no key is less than or equal to zLowerBound.
**      4.  Make sure no key is greater than or equal to zUpperBound.
**      5.  Check the integrity of overflow pages.
**      6.  Recursively call checkTreePage on all children.
**      7.  Verify that the depth of all children is the same.
**      8.  Make sure this page is at least 33% full or else it is
**          the root of the tree.
*/
static int checkTreePage(
    IntegrityCk *pCheck,  /* Context for the sanity check */
    int iPage,            /* Page number of the page to check */
    MemPage *pParent,     /* Parent page */
    char *zParentContext, /* Parent context */
    char *zLowerBound,    /* All keys should be greater than this, if not NULL */
    int nLower,           /* Number of characters in zLowerBound */
    char *zUpperBound,    /* All keys should be less than this, if not NULL */
    int nUpper            /* Number of characters in zUpperBound */
)
{
  MemPage *pPage;
  int i, rc, depth, d2, pgno;
  char *zKey1, *zKey2;
  int nKey1, nKey2;
  BtCursor cur;
  char zMsg[100];
  char zContext[100];
  char hit[SQLITE_PAGE_SIZE];

  /* Check that the page exists
   */
  if (iPage == 0)
    return 0;
  if (checkRef(pCheck, iPage, zParentContext))
    return 0;
  sprintf(zContext, "On tree page %d: ", iPage);
  if ((rc = sqlitepager_get(pCheck->pPager, (Pgno)iPage, (void **)&pPage)) != 0)
  {
    sprintf(zMsg, "unable to get the page. error code=%d", rc);
    checkAppendMsg(pCheck, zContext, zMsg);
    return 0;
  }
  if ((rc = initPage(pPage, (Pgno)iPage, pParent)) != 0)
  {
    sprintf(zMsg, "initPage() returns error code %d", rc);
    checkAppendMsg(pCheck, zContext, zMsg);
    sqlitepager_unref(pPage);
    return 0;
  }

  /* Check out all the cells.
   */
  depth = 0;
  if (zLowerBound)
  {
    zKey1 = malloc(nLower + 1);
    memset(zKey1, 0, nLower + 1);
    memcpy(zKey1, zLowerBound, nLower);
    zKey1[nLower] = 0;
  }
  else
  {
    zKey1 = 0;
  }
  nKey1 = nLower;
  cur.pPage = pPage;
  cur.pBt = pCheck->pBt;
  for (i = 0; i < pPage->nCell; i++)
  {
    Cell *pCell = pPage->apCell[i];
    int sz;

    /* Check payload overflow pages
     */
    nKey2 = NKEY(pCell->h);
    sz = nKey2 + NDATA(pCell->h);
    sprintf(zContext, "On page %d cell %d: ", iPage, i);
    if (sz > MX_LOCAL_PAYLOAD)
    {
      int nPage = (sz - MX_LOCAL_PAYLOAD + OVERFLOW_SIZE - 1) / OVERFLOW_SIZE;
      checkList(pCheck, 0, pCell->ovfl, nPage, zContext);
    }

    /* Check that keys are in the right order
     */
    cur.idx = i;
    zKey2 = sqliteMalloc(nKey2 + 1);
    getPayload(&cur, 0, nKey2, zKey2);
    if (zKey1 && keyCompare(zKey1, nKey1, zKey2, nKey2) >= 0)
    {
      checkAppendMsg(pCheck, zContext, "Key is out of order");
    }

    /* Check sanity of left child page.
     */
    pgno = (int)pCell->h.leftChild;
    d2 = checkTreePage(pCheck, pgno, pPage, zContext, zKey1, nKey1, zKey2, nKey2);
    if (i > 0 && d2 != depth)
    {
      checkAppendMsg(pCheck, zContext, "Child page depth differs");
    }
    depth = d2;
    sqliteFree(zKey1);
    zKey1 = zKey2;
    nKey1 = nKey2;
  }
  pgno = pPage->u.hdr.rightChild;
  sprintf(zContext, "On page %d at right child: ", iPage);
  checkTreePage(pCheck, pgno, pPage, zContext, zKey1, nKey1, zUpperBound, nUpper);
  sqliteFree(zKey1);

  /* Check for complete coverage of the page
   */
  memset(hit, 0, sizeof(hit));
  memset(hit, 1, sizeof(PageHdr));
  for (i = pPage->u.hdr.firstCell; i > 0 && i < SQLITE_PAGE_SIZE;)
  {
    Cell *pCell = (Cell *)&pPage->u.aDisk[i];
    int j;
    for (j = i + cellSize(pCell) - 1; j >= i; j--)
      hit[j]++;
    i = pCell->h.iNext;
  }
  for (i = pPage->u.hdr.firstFree; i > 0 && i < SQLITE_PAGE_SIZE;)
  {
    FreeBlk *pFBlk = (FreeBlk *)&pPage->u.aDisk[i];
    int j;
    for (j = i + pFBlk->iSize - 1; j >= i; j--)
      hit[j]++;
    i = pFBlk->iNext;
  }
  for (i = 0; i < SQLITE_PAGE_SIZE; i++)
  {
    if (hit[i] == 0)
    {
      sprintf(zMsg, "Unused space at byte %d of page %d", i, iPage);
      checkAppendMsg(pCheck, zMsg, 0);
      break;
    }
    else if (hit[i] > 1)
    {
      sprintf(zMsg, "Multiple uses for byte %d of page %d", i, iPage);
      checkAppendMsg(pCheck, zMsg, 0);
      break;
    }
  }

  /* Check that free space is kept to a minimum
   */
#if 0
  if( pParent && pParent->nCell>2 && pPage->nFree>3*SQLITE_PAGE_SIZE/4 ){
    sprintf(zMsg, "free space (%d) greater than max (%d)", pPage->nFree,
       SQLITE_PAGE_SIZE/3);
    checkAppendMsg(pCheck, zContext, zMsg);
  }
#endif

  /* Update freespace totals.
   */
  pCheck->nTreePage++;
  pCheck->nByte += USABLE_SPACE - pPage->nFree;

  sqlitepager_unref(pPage);
  return depth;
}

/*
** This routine does a complete check of the given BTree file.  aRoot[] is
** an array of pages numbers were each page number is the root page of
** a table.  nRoot is the number of entries in aRoot.
**
** If everything checks out, this routine returns NULL.  If something is
** amiss, an error message is written into memory obtained from malloc()
** and a pointer to that error message is returned.  The calling function
** is responsible for freeing the error message when it is done.
*/
char *sqliteBtreeIntegrityCheck(Btree *pBt, int *aRoot, int nRoot)
{
  int i;
  int nRef;
  IntegrityCk sCheck;

  nRef = *sqlitepager_stats(pBt->pPager);
  if (lockBtree(pBt) != SQLITE_OK)
  {
    return sqliteStrDup("Unable to acquire a read lock on the database");
  }
  sCheck.pBt = pBt;
  sCheck.pPager = pBt->pPager;
  sCheck.nPage = sqlitepager_pagecount(sCheck.pPager);
  sCheck.anRef = malloc((sCheck.nPage + 1) * sizeof(sCheck.anRef[0]));
  memset(sCheck.anRef, 0, (sCheck.nPage + 1) * sizeof(sCheck.anRef[0]));

  sCheck.anRef[1] = 1;
  for (i = 2; i <= sCheck.nPage; i++)
  {
    sCheck.anRef[i] = 0;
  }
  sCheck.zErrMsg = 0;

  /* Check the integrity of the freelist
   */
  checkList(&sCheck, 1, pBt->page1->freeList, pBt->page1->nFree,
            "Main freelist: ");

  /* Check all the tables.
   */
  for (i = 0; i < nRoot; i++)
  {
    if (aRoot[i] == 0)
      continue;
    checkTreePage(&sCheck, aRoot[i], 0, "List of tree roots: ", 0, 0, 0, 0);
  }

  /* Make sure every page in the file is referenced
   */
  for (i = 1; i <= sCheck.nPage; i++)
  {
    if (sCheck.anRef[i] == 0)
    {
      char zBuf[100];
      sprintf(zBuf, "Page %d is never used", i);
      checkAppendMsg(&sCheck, zBuf, 0);
    }
  }

  /* Make sure this analysis did not leave any unref() pages
   */
  unlockBtreeIfUnused(pBt);
  if (nRef != *sqlitepager_stats(pBt->pPager))
  {
    char zBuf[100];
    sprintf(zBuf,
            "Outstanding page count goes from %d to %d during this analysis",
            nRef, *sqlitepager_stats(pBt->pPager));
    checkAppendMsg(&sCheck, zBuf, 0);
  }

  /* Clean  up and report errors.
   */
  sqliteFree(sCheck.anRef);
  return sCheck.zErrMsg;
}
