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
** This is the implementation of the page cache subsystem or "pager".
**
** The pager is used to access a database disk file.  It implements
** atomic commit and rollback through the use of a journal file that
** is separate from the database file.  The pager also implements file
** locking to prevent two processes from writing the same database
** file simultaneously, or one process from reading the database while
** another is writing.
**
** @(#) $Id: pager.c,v 1.46 2002/05/30 12:27:03 drh Exp $
*/
#include "sqliteInt.h"
#include "sqlite.h"
#include "pager.h"
#include "os.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

/*
** The page cache as a whole is always in one of the following
** states:
**
**   SQLITE_UNLOCK       The page cache is not currently reading or
**                       writing the database file.  There is no
**                       data held in memory.  This is the initial
**                       state.
**
**   SQLITE_READLOCK     The page cache is reading the database.
**                       Writing is not permitted.  There can be
**                       multiple readers accessing the same database
**                       file at the same time.
**
**   SQLITE_WRITELOCK    The page cache is writing the database.
**                       Access is exclusive.  No other processes or
**                       threads can be reading or writing while one
**                       process is writing.
**
** The page cache comes up in SQLITE_UNLOCK.  The first time a
** sqlite_page_get() occurs, the state transitions to SQLITE_READLOCK.
** After all pages have been released using sqlite_page_unref(),
** the state transitions back to SQLITE_UNLOCK.  The first time
** that sqlite_page_write() is called, the state transitions to
** SQLITE_WRITELOCK.  (Note that sqlite_page_write() can only be
** called on an outstanding page which means that the pager must
** be in SQLITE_READLOCK before it transitions to SQLITE_WRITELOCK.)
** The sqlite_page_rollback() and sqlite_page_commit() functions
** transition the state from SQLITE_WRITELOCK back to SQLITE_READLOCK.
*/
#define SQLITE_UNLOCK 0
#define SQLITE_READLOCK 1
#define SQLITE_WRITELOCK 2

/*
** Each in-memory image of a page begins with the following header.
** This header is only visible to this pager module.  The client
** code that calls pager sees only the data that follows the header.
** -
** page在内存中的结构
** 页面的描述,这个是不会给用户看到的
*/
typedef struct PgHdr PgHdr;
struct PgHdr
{
  Pager *pPager;                /* The pager to which this page belongs|这个Page对外能看到的结构,这看起来像是页面管理器？ */
  Pgno pgno;                    /* The page number for this page */
  PgHdr *pNextHash, *pPrevHash; /* Hash collision chain for PgHdr.pgno| PageId的哈希冲突链,也就是说,如果两个Page的pageId的哈希值相同，则这些产生哈希碰撞的Page也组成一段双向链表 */
  int nRef;                     /* Number of users of this page| 该Page的引用数 */
  PgHdr *pNextFree, *pPrevFree; /* Freelist of pages where nRef==0| 引用数为0的Page组成的双向链表 */
  PgHdr *pNextAll, *pPrevAll;   /* A list of all pages|有关所有Page的集合，这里通过保存Page链表的上下两个指针来表示，通过这两个指针可以以该Page为起始向两头遍历Page链表 */
  char inJournal;               /* TRUE if has been written to journal|该Page有被写入到journal中 */
  char inCkpt;                  /* TRUE if written to the checkpoint journal|该Page有被写入到检查点日志中 */
  char dirty;                   /* TRUE if we need to write back changes|该Page有被写入数据 */
  /* SQLITE_PAGE_SIZE bytes of page data follow this header */
  /* Pager.nExtra bytes of local data follow the page data */
  /* 额外数据跟随在这之后,也就是说，一个PgHdr的连续内存空间大小除了SQLLITE_PAGE_SIZE之外还有一段"隐藏"的nExtra数据，这部分数据跟随在PgHdr对象之后,通过下面的PGHDR_TO_EXTRA宏来获取, */
};

/*
** Convert a pointer to a PgHdr into a pointer to its data
** and back again.
*/
// 获取PgHdr的数据指针，在这里一个Page的实际内容紧跟在PgHdr内存区域之后
#define PGHDR_TO_DATA(P) ((void *)(&(P)[1]))
#define DATA_TO_PGHDR(D) (&((PgHdr *)(D))[-1])
/* 这里也是一样，在一块可以格式化为PgHdr对象的连续内存空间中，通过数据的偏移量找到Extra所在的内存区域并返回 */
// 获取Page的额外数据空间所在
#define PGHDR_TO_EXTRA(P) ((void *)&((char *)(&(P)[1]))[SQLITE_PAGE_SIZE])

/*
** How big to make the hash table used for locating in-memory pages
** by page number.  Knuth says this should be a prime number.
** 根据PageNumber定位Pager的哈希数组大小。最好是一个素数
*/
#define N_PG_HASH 2003

/*
** A open page cache is an instance of the following structure.
** 提供给使用者的结构类型，创建了Page之后使用者对Page进行操作需要通过该结构，而不是PgHdr结构体
** 页面管理对象
*/
struct Pager
{
  char *zFilename;             /* Name of the database file| 数据库文件 */
  char *zJournal;              /* Name of the journal file| Journal文件,跟事务相关 */
  OsFile fd, jfd;              /* File descriptors for database and journal| 数据库文件、journal文件描述符 */
  OsFile cpfd;                 /* File descriptor for the checkpoint journal| journal检查点文件描述符 */
  int dbSize;                  /* Number of pages in the file| 数据库文件中Page的数量 */
  int origDbSize;              /* dbSize before the current change |本次修改前的数据库大小 */
  int ckptSize, ckptJSize;     /* Size of database and journal at ckpt_begin()| 数据库大小和检查点的偏移量? 这里的数据库大小和上一个origDbSize有什么区别吗?  */
  int nExtra;                  /* Add this many bytes to each in-memory page | 用户额外数据大小 */
  void (*xDestructor)(void *); /* Call this routine when freeing pages | 释放Page时调用该函数。 释放是指从Page链表中将这个Page移除吗?*/
  int nPage;                   /* Total number of in-memory pages| 在内存中的Page总数量 */
  int nRef;                    /* Number of in-memory pages with PgHdr.nRef>0 | 引用数大于0的Page数量 */
  int mxPage;                  /* Maximum number of pages to hold in cache| 缓存中保存的最大Page数 */
  int nHit, nMiss, nOvfl;      /* Cache hits, missing, and LRU overflows| 缓存命中、丢失、LRU溢出数量，这里的缓存命中的统计是在获取Pager的时候是否从内存中获取.如果需要从磁盘获取，则nMiss+1 */
  u8 journalOpen;              /* True if journal file descriptors is valid| 如果journal文件已被打开(这里的打开表示Pager获取到了日志文件的读写锁)则为true  */
  u8 ckptOpen;                 /* True if the checkpoint journal is open | 如果journal检查点功能开启，则为trur。这样看来journal和journal checkpoint是两个不同的功能? */
  u8 ckptInUse;                /* True we are in a checkpoint| 如果Page在检查点中，则为true。这意思是这个Page已经被写入到checkpoint中吗? */
  u8 noSync;                   /* Do not sync the journal if true| 写入数据库文件完成后是否立刻刷入磁盘 */
  u8 state;                    /* SQLITE_UNLOCK, _READLOCK or _WRITELOCK| 状态：未加锁、读锁、写锁 */
  u8 errMask;                  /* One of several kinds of errors| 错误信息? */
  u8 tempFile;                 /* zFilename is a temporary file| 如果zFilename是一个临时文件，则为true */
  u8 readOnly;                 /* True for a read-only database| 如果是只读的数据库，则为true */
  u8 needSync;                 /* True if an fsync() is needed on the journal | 写入数据库文件之前是否将日志刷入磁盘 */
  u8 dirtyFile;                /* True if database file has changed in any way| 如果这个数据库文件有被修改，则为true  */
  u8 *aInJournal;              /* One bit for each page in the database file| 每个数据库文件的Page中都有一位，啥意思？ */
  u8 *aInCkpt;                 /* One bit for each page in the database | 每个数据库的Page中都有一位，啥意思？ */
  PgHdr *pFirst, *pLast;       /* List of free pages | 没有被使用的Page组成的链表的首尾节点 */
  PgHdr *pAll;                 /* List of all pages | 总的Page链表的头节点 */
  PgHdr *aHash[N_PG_HASH];     /* Hash table to map page number of PgHdr | 根据PageId查询PgHdr结构的哈希表, 这张表看起来值保存空闲Page */
};

/*
** These are bits that can be set in Pager.errMask.
*/
#define PAGER_ERR_FULL 0x01    /* a write() failed */
#define PAGER_ERR_MEM 0x02     /* malloc() failed */
#define PAGER_ERR_LOCK 0x04    /* error in the locking protocol */
#define PAGER_ERR_CORRUPT 0x08 /* database or journal corruption */
#define PAGER_ERR_DISK 0x10    /* general disk I/O error - bad hard drive? */

/*
** The journal file contains page records in the following
** format.
** journal文件包含以下格式的日志记录
*/
typedef struct PageRecord PageRecord;
struct PageRecord
{
  Pgno pgno;                    /* The page number| Page Number */
  char aData[SQLITE_PAGE_SIZE]; /* Original data for page pgno | Page的原始数据,不包含额外数据 */
};

/*
** Journal files begin with the following magic string.  The data
** was obtained from /dev/random.  It is used only as a sanity check.
** 日志文件开头的magic string,用于文件健全性检查
*/
static const unsigned char aJournalMagic[] = {
    0xd9,
    0xd5,
    0x05,
    0xf9,
    0x20,
    0xa1,
    0x63,
    0xd4,
};

/*
** Hash a page number
** 对一个Page Number求哈希
*/
#define pager_hash(PN) ((PN) % N_PG_HASH)

/*
** Enable reference count tracking here:
*/
#if SQLITE_TEST
int pager_refinfo_enable = 0;
static void pager_refinfo(PgHdr *p)
{
  static int cnt = 0;
  if (!pager_refinfo_enable)
    return;
  printf(
      "REFCNT: %4d addr=0x%08x nRef=%d\n",
      p->pgno, (int)PGHDR_TO_DATA(p), p->nRef);
  cnt++; /* Something to set a breakpoint on */
}
#define REFINFO(X) pager_refinfo(X)
#else
#define REFINFO(X)
#endif

/*
** Convert the bits in the pPager->errMask into an approprate
** return code.
** 根据Pager的errMask获取错误信息
*/
static int pager_errcode(Pager *pPager)
{
  int rc = SQLITE_OK;
  if (pPager->errMask & PAGER_ERR_LOCK)
    rc = SQLITE_PROTOCOL;
  if (pPager->errMask & PAGER_ERR_DISK)
    rc = SQLITE_IOERR;
  if (pPager->errMask & PAGER_ERR_FULL)
    rc = SQLITE_FULL;
  if (pPager->errMask & PAGER_ERR_MEM)
    rc = SQLITE_NOMEM;
  if (pPager->errMask & PAGER_ERR_CORRUPT)
    rc = SQLITE_CORRUPT;
  return rc;
}

/*
** Find a page in the hash table given its page number.  Return
** a pointer to the page or NULL if not found.
** 根据page number从哈希表中获取Page，返回的是一个指向Page的指针。
** 如果不存在Page则该指针为NULL
*/
static PgHdr *pager_lookup(Pager *pPager, Pgno pgno)
{
  PgHdr *p = pPager->aHash[pgno % N_PG_HASH];
  while (p && p->pgno != pgno)
  {
    // 由于存在哈希碰撞，在获取到哈希值所在的指针后还需要遍历哈希链表
    p = p->pNextHash;
  }
  return p;
}

/*
** Unlock the database and clear the in-memory cache.  This routine
** sets the state of the pager back to what it was when it was first
** opened.  Any outstanding pages are invalidated and subsequent attempts
** to access those pages will likely result in a coredump.
*/
static void pager_reset(Pager *pPager)
{
  PgHdr *pPg, *pNext;
  for (pPg = pPager->pAll; pPg; pPg = pNext)
  {
    pNext = pPg->pNextAll;
    sqliteFree(pPg);
  }
  pPager->pFirst = 0;
  pPager->pLast = 0;
  pPager->pAll = 0;
  memset(pPager->aHash, 0, sizeof(pPager->aHash));
  pPager->nPage = 0;
  if (pPager->state >= SQLITE_WRITELOCK)
  {
    sqlitepager_rollback(pPager);
  }
  sqliteOsUnlock(&pPager->fd);
  pPager->state = SQLITE_UNLOCK;
  pPager->dbSize = -1;
  pPager->nRef = 0;
  assert(pPager->journalOpen == 0);
}

/*
** When this routine is called, the pager has the journal file open and
** a write lock on the database.  This routine releases the database
** write lock and acquires a read lock in its place.  The journal file
** is deleted and closed.
*/
static int pager_unwritelock(Pager *pPager)
{
  int rc;
  PgHdr *pPg;
  if (pPager->state < SQLITE_WRITELOCK)
    return SQLITE_OK;
  sqlitepager_ckpt_commit(pPager);

  if (pPager->ckptOpen)
  {
    sqliteOsClose(&pPager->cpfd);
    pPager->ckptOpen = 0;
  }
  sqliteOsClose(&pPager->jfd);
  pPager->journalOpen = 0;
  sqliteOsDelete(pPager->zJournal);
  rc = sqliteOsReadLock(&pPager->fd);
  assert(rc == SQLITE_OK);
  sqliteFree(pPager->aInJournal);
  pPager->aInJournal = 0;
  for (pPg = pPager->pAll; pPg; pPg = pPg->pNextAll)
  {
    pPg->inJournal = 0;
    pPg->dirty = 0;
  }
  pPager->state = SQLITE_READLOCK;
  return rc;
}

/*
** Read a single page from the journal file opened on file descriptor
** jfd.  Playback this one page.
** 从打开的日志文件中读取一页数据,回放这页数据
*/
static int pager_playback_one_page(Pager *pPager, OsFile *jfd)
{
  int rc;
  PgHdr *pPg; /* An existing page in the cache */
  PageRecord pgRec;

  // 从jfd中读取一条Page记录
  rc = sqliteOsRead(jfd, &pgRec, sizeof(pgRec));
  if (rc != SQLITE_OK)
    return rc;

  /* Sanity checking on the page */
  /* 页面的健全性检查 */
  if (pgRec.pgno > pPager->dbSize || pgRec.pgno == 0)
    return SQLITE_CORRUPT;

  /* Playback the page.  Update the in-memory copy of the page
  ** at the same time, if there is one.
  ** 回放页面，如果页面有内存副本，则同时更新
  */
  pPg = pager_lookup(pPager, pgRec.pgno);
  if (pPg)
  {
    // 内存中有副本
    // 1. 将记录中的data复制到内存中page的末尾
    memcpy(PGHDR_TO_DATA(pPg), pgRec.aData, SQLITE_PAGE_SIZE);
    // 2. 将pager的额外信息(nExtra)部分置为0
    memset(PGHDR_TO_EXTRA(pPg), 0, pPager->nExtra);
  }
  rc = sqliteOsSeek(&pPager->fd, (pgRec.pgno - 1) * SQLITE_PAGE_SIZE);
  if (rc == SQLITE_OK)
  {
    // 覆盖Page
    rc = sqliteOsWrite(&pPager->fd, pgRec.aData, SQLITE_PAGE_SIZE);
  }
  return rc;
}

/*
** Playback the journal and thus restore the database file to
** the state it was in before we started making changes.
** 播放日志，从而将数据库文件恢复到开始进行更改之前的状态
**
** The journal file format is as follows:  There is an initial
** file-type string for sanity checking.  Then there is a single
** Pgno number which is the number of pages in the database before
** changes were made.  The database is truncated to this size.
** Next come zero or more page records where each page record
** consists of a Pgno and SQLITE_PAGE_SIZE bytes of data.  See
** the PageRecord structure for details.
** 日志文件格式如下：
**    首先是一个用于文件完整性检查的字符串(maginc number),
**    然后是一个无符号整数(用Pgno表示)用于记录在修改前数据库文件中page的数量,数据库被阶段到这个大小
**    然后是0或多个页面记录，其中每个页面记录由Pgno和SQLITE_PAGE_SIZE字节的数据组成
**    可以查看PageRecord结构查看记录详情
**
** If the file opened as the journal file is not a well-formed
** journal file (as determined by looking at the magic number
** at the beginning) then this routine returns SQLITE_PROTOCOL.
** If any other errors occur during playback, the database will
** likely be corrupted, so the PAGER_ERR_CORRUPT bit is set in
** pPager->errMask and SQLITE_CORRUPT is returned.  If it all
** works, then this routine returns SQLITE_OK.
** 如果打开的日志文件格式不正确，如开头不是magic number，则返回SQLITE_PROTOCOL
** 如果在播放日志的时候出现其他错误，则数据库可能已经损坏,所以会在pPager->errMask中设置为PAGER_ERR_CORRUPT比特标记
** 一切正常则返回OK
*/
static int pager_playback(Pager *pPager)
{
  int nRec;      /* Number of Records| 记录条数 */
  int i;         /* Loop counter| 循环计数器 */
  Pgno mxPg = 0; /* Size of the original file in pages| 原始文件的Page大小 */
  unsigned char aMagic[sizeof(aJournalMagic)];
  int rc;

  /* Figure out how many records are in the journal.  Abort early if
  ** the journal is empty.
  ** 找出日志中有多少条记录
  */
  assert(pPager->journalOpen);
  sqliteOsSeek(&pPager->jfd, 0);
  rc = sqliteOsFileSize(&pPager->jfd, &nRec);
  if (rc != SQLITE_OK)
  {
    goto end_playback;
  }
  // 这里的计算过程表示在写入日志的时候确保了记录不会写入一半
  nRec = (nRec - (sizeof(aMagic) + sizeof(Pgno))) / sizeof(PageRecord);
  if (nRec <= 0)
  {
    goto end_playback;
  }

  /* Read the beginning of the journal and truncate the
  ** database file back to its original size.
  ** 读取文件校验头,对文件进行格式校验
  */
  rc = sqliteOsRead(&pPager->jfd, aMagic, sizeof(aMagic));
  if (rc != SQLITE_OK || memcmp(aMagic, aJournalMagic, sizeof(aMagic)) != 0)
  {
    rc = SQLITE_PROTOCOL;
    goto end_playback;
  }
  // 读取mx_page参数
  rc = sqliteOsRead(&pPager->jfd, &mxPg, sizeof(mxPg));
  if (rc != SQLITE_OK)
  {
    goto end_playback;
  }

  // 将数据库文件截断为修改前的大小
  rc = sqliteOsTruncate(&pPager->fd, mxPg * SQLITE_PAGE_SIZE);
  if (rc != SQLITE_OK)
  {
    goto end_playback;
  }
  pPager->dbSize = mxPg;

  /* Copy original pages out of the journal and back into the database file.
   * 将原始页面从日志中复制出来并返回到数据库文件中
   */
  for (i = nRec - 1; i >= 0; i--)
  {
    // 复现日志，这里只会将日志的数据拷贝的fd文件中，并不会加载其中的page
    rc = pager_playback_one_page(pPager, &pPager->jfd);
    if (rc != SQLITE_OK)
      break;
  }

end_playback:
  if (rc != SQLITE_OK)
  {
    pager_unwritelock(pPager);
    pPager->errMask |= PAGER_ERR_CORRUPT;
    rc = SQLITE_CORRUPT;
  }
  else
  {
    rc = pager_unwritelock(pPager);
  }
  return rc;
}

/*
** Playback the checkpoint journal.
**
** This is similar to playing back the transaction journal but with
** a few extra twists.
**
**    (1)  The number of pages in the database file at the start of
**         the checkpoint is stored in pPager->ckptSize, not in the
**         journal file itself.
**
**    (2)  In addition to playing back the checkpoint journal, also
**         playback all pages of the transaction journal beginning
**         at offset pPager->ckptJSize.
*/
static int pager_ckpt_playback(Pager *pPager)
{
  int nRec; /* Number of Records */
  int i;    /* Loop counter */
  int rc;

  /* Truncate the database back to its original size.
   */
  rc = sqliteOsTruncate(&pPager->fd, pPager->ckptSize * SQLITE_PAGE_SIZE);
  pPager->dbSize = pPager->ckptSize;

  /* Figure out how many records are in the checkpoint journal.
   */
  assert(pPager->ckptInUse && pPager->journalOpen);
  sqliteOsSeek(&pPager->cpfd, 0);
  rc = sqliteOsFileSize(&pPager->cpfd, &nRec);
  if (rc != SQLITE_OK)
  {
    goto end_ckpt_playback;
  }
  nRec /= sizeof(PageRecord);

  /* Copy original pages out of the checkpoint journal and back into the
  ** database file.
  */
  for (i = nRec - 1; i >= 0; i--)
  {
    rc = pager_playback_one_page(pPager, &pPager->cpfd);
    if (rc != SQLITE_OK)
      goto end_ckpt_playback;
  }

  /* Figure out how many pages need to be copied out of the transaction
  ** journal.
  */
  rc = sqliteOsSeek(&pPager->jfd, pPager->ckptJSize);
  if (rc != SQLITE_OK)
  {
    goto end_ckpt_playback;
  }
  rc = sqliteOsFileSize(&pPager->jfd, &nRec);
  if (rc != SQLITE_OK)
  {
    goto end_ckpt_playback;
  }
  nRec = (nRec - pPager->ckptJSize) / sizeof(PageRecord);
  for (i = nRec - 1; i >= 0; i--)
  {
    rc = pager_playback_one_page(pPager, &pPager->jfd);
    if (rc != SQLITE_OK)
      goto end_ckpt_playback;
  }

end_ckpt_playback:
  if (rc != SQLITE_OK)
  {
    pPager->errMask |= PAGER_ERR_CORRUPT;
    rc = SQLITE_CORRUPT;
  }
  return rc;
}

/*
** Change the maximum number of in-memory pages that are allowed.
**
** The maximum number is the absolute value of the mxPage parameter.
** If mxPage is negative, the noSync flag is also set.  noSync bypasses
** calls to sqliteOsSync().  The pager runs much faster with noSync on,
** but if the operating system crashes or there is an abrupt power
** failure, the database file might be left in an inconsistent and
** unrepairable state.
*/
void sqlitepager_set_cachesize(Pager *pPager, int mxPage)
{
  if (mxPage >= 0)
  {
    pPager->noSync = pPager->tempFile;
  }
  else
  {
    pPager->noSync = 1;
    mxPage = -mxPage;
  }
  if (mxPage > 10)
  {
    pPager->mxPage = mxPage;
  }
}

/*
** Open a temporary file.  Write the name of the file into zName
** (zName must be at least SQLITE_TEMPNAME_SIZE bytes long.)  Write
** the file descriptor into *fd.  Return SQLITE_OK on success or some
** other error code if we fail.
**
** The OS will automatically delete the temporary file when it is
** closed.
*/
static int sqlitepager_opentemp(char *zFile, OsFile *fd)
{
  int cnt = 8;
  int rc;
  do
  {
    cnt--;
    sqliteOsTempFileName(zFile);
    rc = sqliteOsOpenExclusive(zFile, fd, 1);
  } while (cnt > 0 && rc != SQLITE_OK);
  return rc;
}

/*
** Create a new page cache and put a pointer to the page cache in *ppPager.
** The file to be cached need not exist.  The file is not locked until
** the first call to sqlitepager_get() and is only held open until the
** last page is released using sqlitepager_unref().
** 创建一个新的page缓存，并在ppPager中放置一个指向page缓存的指针。
** 要缓存的文件不必存在,该文件在第一次调用sqlitepager_get()之前不会被锁定，并且仅在使用sqlitepager_unref()释放最后一页之前保持打开状态
**
** If zFilename is NULL then a randomly-named temporary file is created
** and used as the file to be cached.  The file will be deleted
** automatically when it is closed.
** 如果zFilename为NULL，则创建一个临时文件，并在程序结束的时候自动删除
**
**
*/
int sqlitepager_open(
    Pager **ppPager,       /* Return the Pager structure here| 创建的Pager指针 */
    const char *zFilename, /* Name of the database file to open| 要打开的数据库文件 */
    int mxPage,            /* Max number of in-memory cache pages| 内存中Page的最大缓存数 */
    int nExtra             /* Extra bytes append to each in-memory page| Page中的额外信息大小 */
)
{
  Pager *pPager;
  int nameLen;
  OsFile fd;
  int rc;
  int tempFile;
  int readOnly = 0;
  char zTemp[SQLITE_TEMPNAME_SIZE];

  *ppPager = 0;
  if (sqlite_malloc_failed)
  {
    // sqlite获取内存失败
    return SQLITE_NOMEM;
  }
  if (zFilename)
  {
    // 从zFilename获取文件读写句柄，这时候并没有锁定文件
    rc = sqliteOsOpenReadWrite(zFilename, &fd, &readOnly);
    tempFile = 0;
  }
  else
  {
    // 从临时文件夹中创建一个文件
    rc = sqlitepager_opentemp(zTemp, &fd);
    zFilename = zTemp;
    tempFile = 1;
  }

  if (rc != SQLITE_OK)
  {
    return SQLITE_CANTOPEN;
  }
  // 获取zFilename长度
  nameLen = strlen(zFilename);

  // 开辟一块内存空间,创建对象
  pPager = malloc(sizeof(*pPager) + nameLen * 2 + 30);
  memset(pPager, 0, sizeof(*pPager) + nameLen * 2 + 30);

  if (pPager == 0)
  {
    // 如果获取内存空间失败
    sqliteOsClose(&fd);
    return SQLITE_NOMEM;
  }

  // 在pPager之后获取一块内存空间
  pPager->zFilename = (char *)&pPager[1];
  // 在zFilename之后获取一块空间
  pPager->zJournal = &pPager->zFilename[nameLen + 1];
  // 内存空间的布局应该是这样[Pager][zFilename][zJournal]
  // 这种数组的写法就是对指针的移动
  strcpy(pPager->zFilename, zFilename);
  strcpy(pPager->zJournal, zFilename);
  strcpy(&pPager->zJournal[nameLen], "-journal");

  pPager->fd = fd;
  pPager->journalOpen = 0;
  pPager->ckptOpen = 0;
  pPager->ckptInUse = 0;
  pPager->nRef = 0;
  pPager->dbSize = -1;
  pPager->ckptSize = 0;
  pPager->ckptJSize = 0;
  pPager->nPage = 0;
  pPager->mxPage = mxPage > 5 ? mxPage : 10;
  pPager->state = SQLITE_UNLOCK;
  pPager->errMask = 0;
  pPager->tempFile = tempFile;
  pPager->readOnly = readOnly;
  pPager->needSync = 0;
  pPager->noSync = pPager->tempFile;
  pPager->pFirst = 0;
  pPager->pLast = 0;
  pPager->nExtra = nExtra;
  memset(pPager->aHash, 0, sizeof(pPager->aHash));
  *ppPager = pPager;

  return SQLITE_OK;
}

/*
** Set the destructor for this pager.  If not NULL, the destructor is called
** when the reference count on each page reaches zero.  The destructor can
** be used to clean up information in the extra segment appended to each page.
**
** The destructor is not called as a result sqlitepager_close().
** Destructors are only called by sqlitepager_unref().
*/
void sqlitepager_set_destructor(Pager *pPager, void (*xDesc)(void *))
{
  pPager->xDestructor = xDesc;
}

/*
** Return the total number of pages in the disk file associated with
** pPager.
** 获取磁盘上数据库文件中所有Page数量
*/
int sqlitepager_pagecount(Pager *pPager)
{
  int n;
  assert(pPager != 0);
  if (pPager->dbSize >= 0)
  {
    return pPager->dbSize;
  }
  if (sqliteOsFileSize(&pPager->fd, &n) != SQLITE_OK)
  {
    pPager->errMask |= PAGER_ERR_DISK;
    return 0;
  }
  n /= SQLITE_PAGE_SIZE;
  if (pPager->state != SQLITE_UNLOCK)
  {
    pPager->dbSize = n;
  }
  return n;
}

/*
** Shutdown the page cache.  Free all memory and close all files.
**
** If a transaction was in progress when this routine is called, that
** transaction is rolled back.  All outstanding pages are invalidated
** and their memory is freed.  Any attempt to use a page associated
** with this page cache after this function returns will likely
** result in a coredump.
*/
int sqlitepager_close(Pager *pPager)
{
  PgHdr *pPg, *pNext;
  switch (pPager->state)
  {
  case SQLITE_WRITELOCK:
  {
    sqlitepager_rollback(pPager);
    sqliteOsUnlock(&pPager->fd);
    assert(pPager->journalOpen == 0);
    break;
  }
  case SQLITE_READLOCK:
  {
    sqliteOsUnlock(&pPager->fd);
    break;
  }
  default:
  {
    /* Do nothing */
    break;
  }
  }
  for (pPg = pPager->pAll; pPg; pPg = pNext)
  {
    pNext = pPg->pNextAll;
    sqliteFree(pPg);
  }
  sqliteOsClose(&pPager->fd);
  assert(pPager->journalOpen == 0);
  /* Temp files are automatically deleted by the OS
  ** if( pPager->tempFile ){
  **   sqliteOsDelete(pPager->zFilename);
  ** }
  */
  sqliteFree(pPager);
  return SQLITE_OK;
}

/*
** Return the page number for the given page data.
*/
Pgno sqlitepager_pagenumber(void *pData)
{
  PgHdr *p = DATA_TO_PGHDR(pData);
  return p->pgno;
}

/*
** Increment the reference count for a page.  If the page is
** currently on the freelist (the reference count is zero) then
** remove it from the freelist.
** 增加page引用计数，如果当前page在空闲链表中，则将其删除
*/
static void page_ref(PgHdr *pPg)
{
  if (pPg->nRef == 0)
  {
    /* The page is currently on the freelist.  Remove it. 
      根据这个page所在freelist的位置，将其移出链表
    */
    if (pPg->pPrevFree)
    {
      pPg->pPrevFree->pNextFree = pPg->pNextFree;
    }
    else
    {
      pPg->pPager->pFirst = pPg->pNextFree;
    }


    if (pPg->pNextFree)
    {
      pPg->pNextFree->pPrevFree = pPg->pPrevFree;
    }
    else
    {
      pPg->pPager->pLast = pPg->pPrevFree;
    }

    // 给这个Page所在的页面管理器引用计数+1
    pPg->pPager->nRef++;
  }
  pPg->nRef++;
  REFINFO(pPg);
}

/*
** Increment the reference count for a page.  The input pointer is
** a reference to the page data.
*/
int sqlitepager_ref(void *pData)
{
  PgHdr *pPg = DATA_TO_PGHDR(pData);
  page_ref(pPg);
  return SQLITE_OK;
}

/*
** Sync the journal and then write all free dirty pages to the database
** file.
**
** Writing all free dirty pages to the database after the sync is a
** non-obvious optimization.  fsync() is an expensive operation so we
** want to minimize the number ot times it is called. After an fsync() call,
** we are free to write dirty pages back to the database.  It is best
** to go ahead and write as many dirty pages as possible to minimize
** the risk of having to do another fsync() later on.  Writing dirty
** free pages in this way was observed to make database operations go
** up to 10 times faster.
**
** If we are writing to temporary database, there is no need to preserve
** the integrity of the journal file, so we can save time and skip the
** fsync().
*/
static int syncAllPages(Pager *pPager)
{
  PgHdr *pPg;
  int rc = SQLITE_OK;
  if (pPager->needSync)
  {
    if (!pPager->tempFile)
    {
      rc = sqliteOsSync(&pPager->jfd);
      if (rc != 0)
        return rc;
    }
    pPager->needSync = 0;
  }
  for (pPg = pPager->pFirst; pPg; pPg = pPg->pNextFree)
  {
    if (pPg->dirty)
    {
      sqliteOsSeek(&pPager->fd, (pPg->pgno - 1) * SQLITE_PAGE_SIZE);
      rc = sqliteOsWrite(&pPager->fd, PGHDR_TO_DATA(pPg), SQLITE_PAGE_SIZE);
      if (rc != SQLITE_OK)
        break;
      pPg->dirty = 0;
    }
  }
  return rc;
}

/*
** Acquire a page.
** 获取Page
**
** A read lock on the disk file is obtained when the first page is acquired.
** This read lock is dropped when the last page is released.
** 当获取这个文件的第一个Page时会对其添加读锁，在在这文件最后一个Page被释放时读锁才会被释放
**
** A _get works for any page number greater than 0.  If the database
** file is smaller than the requested page, then no actual disk
** read occurs and the memory image of the page is initialized to
** all zeros.  The extra data appended to a page is always initialized
** to zeros the first time a page is loaded into memory.
** _get适用于任何大于0的page number, 如果数据库文件小于要请求的page，则不会发生实际的磁盘读取,
** 并且page的磁盘镜像被初始化为0. Page第一次加载到内存中时，附加到Page的额外数据始终初始化为0
** _get操作如果没有查询到对应pgno的page，那么会在磁盘中生成编号对应的page
**
** The acquisition might fail for several reasons.  In all cases,
** an appropriate error code is returned and *ppPage is set to NULL.
** 获取Page失败可能会有很多原因，一旦出现该问题，都会将ppPage指针指向NULL
**
** See also sqlitepager_lookup().  Both this routine and _lookup() attempt
** to find a page in the in-memory cache first.  If the page is not already
** in memory, this routine goes to disk to read it in whereas _lookup()
** just returns 0.  This routine acquires a read-lock the first time it
** has to go to disk, and could also playback an old journal if necessary.
** Since _lookup() never goes to disk, it never has to deal with locks
** or journal files.
**
** 另外可以参照sqlitepager_lookup()。改函数和_lookup()都首先尝试在内存缓存中查询Page。
** 如果Page还不在内存中，这个函数回去磁盘读取，而_lookup()只返回0。
** 该函数在第一次进入磁盘时获取一个读锁，并且在必要的时候会复现日志。
** 由于_lookup()永远不会进入磁盘，因此它永远不必处理锁或者日志文件
** > _lookup()函数是尝试从内存中获取Page的操作，如果内存中不存在，则直接返回，而不是像_get一样会尝试从磁盘中获取
**
*/
int sqlitepager_get(Pager *pPager, Pgno pgno, void **ppPage)
{
  PgHdr *pPg;

  /* Make sure we have not hit any critical errors.
    错误检查
  */
  if (pPager == 0 || pgno == 0)
  {
    return SQLITE_ERROR;
  }
  if (pPager->errMask & ~(PAGER_ERR_FULL))
  {
    return pager_errcode(pPager);
  }
 
  /* If this is the first page accessed, then get a read lock
  ** on the database file.
  ** 如果pager的引用数为0
  ** 意味着尚未有任何一个Page被引用，该阶段在初始运行阶段必定出现。
  **
  */
  if (pPager->nRef == 0)
  {
    // 修改pPager的状态,改为读锁状态
    if (sqliteOsReadLock(&pPager->fd) != SQLITE_OK)
    {
      *ppPage = 0;
      return SQLITE_BUSY;
    }
    pPager->state = SQLITE_READLOCK;

    /* If a journal file exists, try to play it back.
    如果日志文件存在，则尝试复现
    在Pager启动后，客户端尝试获取首个Page的时候，会先执行日志复现操作。
    */
    if (sqliteOsFileExists(pPager->zJournal))
    {
      int rc, dummy;

      /* Get a write lock on the database
      对数据库文件获取写锁
      */
      rc = sqliteOsWriteLock(&pPager->fd);
      if (rc != SQLITE_OK)
      {
        // 获取失败，释放所有的锁
        rc = sqliteOsUnlock(&pPager->fd);
        assert(rc == SQLITE_OK);
        *ppPage = 0;
        // 数据库文件已经被锁定
        return SQLITE_BUSY;
      }
      pPager->state = SQLITE_WRITELOCK;

      /* Open the journal for exclusive access.  Return SQLITE_BUSY if
      ** we cannot get exclusive access to the journal file.
      **
      ** Even though we will only be reading from the journal, not writing,
      ** we have to open the journal for writing in order to obtain an
      ** exclusive access lock.
      ** 获取日志文件的读写锁
      */
      rc = sqliteOsOpenReadWrite(pPager->zJournal, &pPager->jfd, &dummy);
      if (rc != SQLITE_OK)
      {
        rc = sqliteOsUnlock(&pPager->fd);
        assert(rc == SQLITE_OK);
        *ppPage = 0;
        return SQLITE_BUSY;
      }
      // 设置日志文件打开标记
      pPager->journalOpen = 1;

      /* Playback and delete the journal.  Drop the database write
      ** lock and reacquire the read lock.
      ** 将日志中的修改都应用到数据库文件后将日志删除，并释放数据库文件的写锁，并重新尝试获取读锁
      */
      rc = pager_playback(pPager);
      if (rc != SQLITE_OK)
      {
        return rc;
      }
    }
    pPg = 0;
  }
  else
  {
    /* Search for page in cache */
    // Pager的引用大于0，表示内存中已经存在同一文件的Pager。则尝试从内存中获取
    pPg = pager_lookup(pPager, pgno);
  }

  if (pPg == 0)
  {
    // 请求的Page不在缓存中
    /* The requested page is not in the page cache. */
    int h;
    // 缓存未命中计数器
    pPager->nMiss++;
    if (pPager->nPage < pPager->mxPage || pPager->pFirst == 0)
    {
      /* Create a new page */
      // 新建一个PgHdr,这里请求分配的内存空间大小为：PgHdr结构体大小+PAGE_SIZE+额外空间大小
      pPg = malloc(sizeof(*pPg) + SQLITE_PAGE_SIZE + pPager->nExtra);
      memset(pPg, 0, sizeof(*pPg) + SQLITE_PAGE_SIZE + pPager->nExtra);
      if (pPg == 0)
      {
        // 内存分配失败
        *ppPage = 0;
        pager_unwritelock(pPager);
        pPager->errMask |= PAGER_ERR_MEM;
        return SQLITE_NOMEM;
      }

      // 将新建的Page放入总链表中

      // PgHdr的页面指向Pager
      pPg->pPager = pPager;
      // 如果是第一个Page，这里应该为NULL
      pPg->pNextAll = pPager->pAll;
      if (pPager->pAll)
      {
        // 将新创建的PgHdr放入链表头结点
        pPager->pAll->pPrevAll = pPg;
      }
      // 新的PgHdr是头结点，所有prevAll为0
      pPg->pPrevAll = 0;
      // 设置Pager上的头结点
      pPager->pAll = pPg;
      // Page计数器递增
      pPager->nPage++;
    }
    else
    {
      // 缓存中的Page已满，需要释放旧Page
      /* Recycle an older page.  First locate the page to be recycled.
      ** Try to find one that is not dirty and is near the head of
      ** of the free list */
      // 定位回收Page，尝试寻找一个没有修改且靠近空闲列表头部的。
      // 这个Page可以减少相关操作，不需要有写入流程

      // 获取空闲链表头结点
      pPg = pPager->pFirst;
      while (pPg && pPg->dirty)
      {
        // 寻找没有被修改的PgHdr
        pPg = pPg->pNextFree;
      }

      /* If we could not find a page that has not been used recently
      ** and which is not dirty, then sync the journal and write all
      ** dirty free pages into the database file, thus making them
      ** clean pages and available for recycling.
      **
      ** 如果找不到最近没有被修改的空闲页面，则同步日志，并将所有脏的空闲页面写入数据库文件中。
      ** 使他们成为干净的页面可供回收
      **
      ** We have to sync the journal before writing a page to the main
      ** database.  But syncing is a very slow operation.  So after a
      ** sync, it is best to write everything we can back to the main
      ** database to minimize the risk of having to sync again in the
      ** near future.  That is way we write all dirty pages after a
      ** sync.
      **
      ** 在将page写入主数据库之前，我们必须同步日志。
      ** 但是同步是一个非常缓慢的操作。因此，在同步之后，最好将我们可以写的内容全部写入到主数据库，以减少
      ** 在将来需要再次同步的风险。
      */
      if (pPg == 0)
      {
        // 没有未被修改的空闲page
        // 同步所有page
        int rc = syncAllPages(pPager);
        if (rc != 0)
        {
          sqlitepager_rollback(pPager);
          *ppPage = 0;
          return SQLITE_IOERR;
        }
        // 释放完成后，PgHdr就可以从空闲链表的头结点直接获取
        pPg = pPager->pFirst;
      }
      assert(pPg->nRef == 0);
      assert(pPg->dirty == 0);

      /* Unlink the old page from the free list and the hash table
      从空闲链表和哈希表中取消链接旧页面
      */

      // 这一段代码就是从双向链表中删除某个节点是的修改前后节点指针的逻辑
      if (pPg->pPrevFree)
      {
        // 如果分配的PgHdr的前缀空闲节点存在，则将前缀空闲节点的next指针指向自身的next空闲节点
        pPg->pPrevFree->pNextFree = pPg->pNextFree;
      }
      else
      {
        // PgHdr位于空闲链表头结点
        assert(pPager->pFirst == pPg);
        // 空闲链表头结点指针指向next
        pPager->pFirst = pPg->pNextFree;
      }


      // 同上，这里是修改next节点的prev指针指向
      if (pPg->pNextFree)
      {
        pPg->pNextFree->pPrevFree = pPg->pPrevFree;
      }
      else
      {
        assert(pPager->pLast == pPg);
        pPager->pLast = pPg->pPrevFree;
      }

      // PgHdr已被移出空闲链表，两个指针置为0
      pPg->pNextFree = pPg->pPrevFree = 0;


      // 哈希链表也跟双向链表删除一个逻辑
      if (pPg->pNextHash)
      {
        pPg->pNextHash->pPrevHash = pPg->pPrevHash;
      }
      if (pPg->pPrevHash)
      {
        pPg->pPrevHash->pNextHash = pPg->pNextHash;
      }
      else
      {
        // 如果这个PgHdr是哈希链表的头结点, 则将Page中的Hash数组的值修改为他的next哈希节点
        // Page中的aHash保存的是哈希值相同的空闲节点
        h = pager_hash(pPg->pgno);
        assert(pPager->aHash[h] == pPg);
        pPager->aHash[h] = pPg->pNextHash;
      }
      // 移除哈希链表
      pPg->pNextHash = pPg->pPrevHash = 0;
      // LRU计数器递增
      pPager->nOvfl++;
    }

    pPg->pgno = pgno;
    // ?
    if (pPager->aInJournal && (int)pgno <= pPager->origDbSize)
    {
      pPg->inJournal = (pPager->aInJournal[pgno / 8] & (1 << (pgno & 7))) != 0;
    }
    else
    {
      pPg->inJournal = 0;
    }
    if (pPager->aInCkpt && (int)pgno <= pPager->ckptSize)
    {
      pPg->inCkpt = (pPager->aInCkpt[pgno / 8] & (1 << (pgno & 7))) != 0;
    }
    else
    {
      pPg->inCkpt = 0;
    }

    // 新建的PgHdr未被修改，且在get的时候才会创建，因此引用计数为1
    pPg->dirty = 0;
    pPg->nRef = 1;
    REFINFO(pPg);
    // Page的计数器也要加1
    pPager->nRef++;
    // 求要获取的Page number的哈希值
    // 将新的PgHdr放入哈希链表的头结点
    h = pager_hash(pgno);
    pPg->pNextHash = pPager->aHash[h];
    pPager->aHash[h] = pPg;
    if (pPg->pNextHash)
    {
      // 确保aHash[h]是头结点
      assert(pPg->pNextHash->pPrevHash == 0);
      pPg->pNextHash->pPrevHash = pPg;
    }

    // 该文件在程序运行过程中的第一个Page,需要获取数据库文件中的page数量
    if (pPager->dbSize < 0)
      sqlitepager_pagecount(pPager);
    if (pPager->dbSize < (int)pgno)
    {
      // 数据库文件page数量小于page number, 这是一个新的page
      memset(PGHDR_TO_DATA(pPg), 0, SQLITE_PAGE_SIZE);
    }
    else
    {
      // 从数据库文件中读取page内容
      int rc;
      sqliteOsSeek(&pPager->fd, (pgno - 1) * SQLITE_PAGE_SIZE);
      rc = sqliteOsRead(&pPager->fd, PGHDR_TO_DATA(pPg), SQLITE_PAGE_SIZE);
      if (rc != SQLITE_OK)
      {
        return rc;
      }
    }

    // 如果page中存在额外数据空间，则将page后的额外数据内存块初始化为0
    if (pPager->nExtra > 0)
    {
      memset(PGHDR_TO_EXTRA(pPg), 0, pPager->nExtra);
    }
  }
  else
  {
    /* The requested page is in the page cache. */
    // 请求的page在内存中，缓存命中计数器递增
    pPager->nHit++;
    // 引用计数递增
    page_ref(pPg);
  }
  // 指针指向该Page的数据位
  *ppPage = PGHDR_TO_DATA(pPg);
  return SQLITE_OK;
}

/*
** Acquire a page if it is already in the in-memory cache.  Do
** not read the page from disk.  Return a pointer to the page,
** or 0 if the page is not in cache.
** 如果Page已经位于内存缓存中，不要从磁盘读取该Page.
** 返回一个指向内存中Page的数据块起始位置的指针,如果Page不在内存中，则返回0
**
** See also sqlitepager_get().  The difference between this routine
** and sqlitepager_get() is that _get() will go to the disk and read
** in the page if the page is not already in cache.  This routine
** returns NULL if the page is not in cache or if a disk I/O error
** has ever happened.
** 该方法和sqlitepager_get()的区别在于所有_get()方法在Page不存在内存中的时候都会去磁盘里获取
** 该方法则会在Page不存在内存中或者曾经发生IO异常的时候返回NULL
*/
void *sqlitepager_lookup(Pager *pPager, Pgno pgno)
{
  PgHdr *pPg;

  /* Make sure we have not hit any critical errors.
   */
  if (pPager == 0 || pgno == 0)
  {
    return 0;
  }
  if (pPager->errMask & ~(PAGER_ERR_FULL))
  {
    return 0;
  }
  if (pPager->nRef == 0)
  {
    return 0;
  }
  pPg = pager_lookup(pPager, pgno);
  if (pPg == 0)
    return 0;
  page_ref(pPg);
  return PGHDR_TO_DATA(pPg);
}

/*
** Release a page.
**
** If the number of references to the page drop to zero, then the
** page is added to the LRU list.  When all references to all pages
** are released, a rollback occurs and the lock on the database is
** removed.
*/
int sqlitepager_unref(void *pData)
{
  PgHdr *pPg;

  /* Decrement the reference count for this page
   */
  pPg = DATA_TO_PGHDR(pData);
  assert(pPg->nRef > 0);
  pPg->nRef--;
  REFINFO(pPg);

  /* When the number of references to a page reach 0, call the
  ** destructor and add the page to the freelist.
  */
  if (pPg->nRef == 0)
  {
    Pager *pPager;
    pPager = pPg->pPager;
    pPg->pNextFree = 0;
    pPg->pPrevFree = pPager->pLast;
    pPager->pLast = pPg;
    if (pPg->pPrevFree)
    {
      pPg->pPrevFree->pNextFree = pPg;
    }
    else
    {
      pPager->pFirst = pPg;
    }
    if (pPager->xDestructor)
    {
      pPager->xDestructor(pData);
    }

    /* When all pages reach the freelist, drop the read lock from
    ** the database file.
    */
    pPager->nRef--;
    assert(pPager->nRef >= 0);
    if (pPager->nRef == 0)
    {
      pager_reset(pPager);
    }
  }
  return SQLITE_OK;
}

/*
** Acquire a write-lock on the database.  The lock is removed when
** the any of the following happen:
** 获得一个基于数据库的写锁,这个锁只有当以下情况的时候才会被释放
**
**   *  sqlitepager_commit() is called.事务提交
**   *  sqlitepager_rollback() is called.事务回滚
**   *  sqlitepager_close() is called. Page缓存被关闭
**   *  sqlitepager_unref() is called to on every outstanding page. 释放程序内存?
**
** The parameter to this routine is a pointer to any open page of the
** database file.  Nothing changes about the page - it is used merely
** to acquire a pointer to the Pager structure and as proof that there
** is already a read-lock on the database.
** 在开始事务前必须要获取到数据库的读锁
** 注意:这个读锁是数据库级别的读锁,一个数据库允许多个读锁,但只要还有其他读锁存在就无法再分配出写锁
**
** If the database is already write-locked, this routine is a no-op.
*/
int sqlitepager_begin(void *pData)
{
  // 根据数据指针获取拥有这个数据的Page指针
  PgHdr *pPg = DATA_TO_PGHDR(pData);
  // 获取Pager指针
  Pager *pPager = pPg->pPager;
  int rc = SQLITE_OK;
  assert(pPg->nRef > 0);
  assert(pPager->state != SQLITE_UNLOCK);
  if (pPager->state == SQLITE_READLOCK)
  {
    // 必须要得到改Page的读锁
    // 如果page拥有读锁,则尝试升格为写锁
    assert(pPager->aInJournal == 0);
    rc = sqliteOsWriteLock(&pPager->fd);
    if (rc != SQLITE_OK)
    {
      return rc;
    }
    // 给Pager的AInJournal开辟一块内存空间,空间大小跟当前数据库中Page数量相关
    pPager->aInJournal = malloc(pPager->dbSize / 8 + 1);
    memset(pPager->aInJournal, 0, pPager->dbSize / 8 + 1);

    if (pPager->aInJournal == 0)
    {
      // 开辟内存失败
      sqliteOsReadLock(&pPager->fd);
      return SQLITE_NOMEM;
    }
    // 为此进程打开一个独占的可供读写的文件
    rc = sqliteOsOpenExclusive(pPager->zJournal, &pPager->jfd, 0);
    if (rc != SQLITE_OK)
    {
      // 文件打开失败，释放aInJournal的内存空间，并释放写锁
      sqliteFree(pPager->aInJournal);
      pPager->aInJournal = 0;
      sqliteOsReadLock(&pPager->fd);
      return SQLITE_CANTOPEN;
    }

    /**
     * 这里打开的文件是zJournal,开启事务期间对数据库的写入之前不会将日志刷入磁盘,
     * Page不是脏页,状态修改为写锁
     */
    pPager->journalOpen = 1;
    pPager->needSync = 0;
    pPager->dirtyFile = 0;
    pPager->state = SQLITE_WRITELOCK;
    // 更新磁盘上所有Page的数量
    sqlitepager_pagecount(pPager);
    // 将当前磁盘上Page的数量额外记录
    pPager->origDbSize = pPager->dbSize;
    // 写入日志文件的magic
    // 此处创建回滚用的备份文件
    rc = sqliteOsWrite(&pPager->jfd, aJournalMagic, sizeof(aJournalMagic));
    if (rc == SQLITE_OK)
    {
      // 将数据库中Page数量保存在日志文件中
      rc = sqliteOsWrite(&pPager->jfd, &pPager->dbSize, sizeof(Pgno));
    }
    if (rc != SQLITE_OK)
    {
      // 写入失败,释放锁
      rc = pager_unwritelock(pPager);
      if (rc == SQLITE_OK)
        rc = SQLITE_FULL;
    }
  }
  return rc;
}

/*
** Mark a data page as writeable.  The page is written into the journal
** if it is not there already.  This routine must be called before making
** changes to a page.
** 将数据页标记为可写。如果页面不存在则将其写入日志。在对页面进行修改时必须调用该函数
**
** The first time this routine is called, the pager creates a new
** journal and acquires a write lock on the database.  If the write
** lock could not be acquired, this routine returns SQLITE_BUSY.  The
** calling routine must check for that return value and be careful not to
** change any page data until this routine returns SQLITE_OK.
** 第一次调用该函数的时候，pager创建一个新的日志并获取数据库上的写锁。
** 如果无法获取写锁，则返回SQLITE_BUSY。调用该函数必须检查返回值，并注意在返回SQLITE_OK前不要修改任何Page
**
** If the journal file could not be written because the disk is full,
** then this routine returns SQLITE_FULL and does an immediate rollback.
** All subsequent write attempts also return SQLITE_FULL until there
** is a call to sqlitepager_commit() or sqlitepager_rollback() to
** reset.
** 如果磁盘已满而无法写入日志，则返回SQLITE_FULL并立即回滚.
** 所有后续的写入尝试也会返回SQLITE_FULL，直到调用sqlitepager_commit()
** 或sqlitepager_rollback()来重置日志文件
** commit/rollback操作会删除日志文件?
**
*/
int sqlitepager_write(void *pData)
{
  // 从data指针获取页面指针
  PgHdr *pPg = DATA_TO_PGHDR(pData);
  // 从页面指针获取页面管理指针
  Pager *pPager = pPg->pPager;
  int rc = SQLITE_OK;

  /* Check for errors
  检查写入前是否发生异常
  */
  if (pPager->errMask)
  {
    return pager_errcode(pPager);
  }
  if (pPager->readOnly)
  {
    // 如果是只读状态，则无法写入
    return SQLITE_PERM;
  }

  /* Mark the page as dirty.  If the page has already been written
  ** to the journal then we can return right away.
  ** 将Page修改为脏页，如果页面已经写入日志，那么我们可以立即返回
  */
  pPg->dirty = 1;
  // 该Page已被写入到日志文件中，且该Page已被写入到checkpoint文件中或者尚未有其他线程在写入检查点文件
  if (pPg->inJournal && (pPg->inCkpt || pPager->ckptInUse == 0))
  {
    // 该数据库文件已被修改，可以直接被写入
    pPager->dirtyFile = 1;
    return SQLITE_OK;
  }

  /* If we get this far, it means that the page needs to be
  ** written to the transaction journal or the ckeckpoint journal
  ** or both.
  ** 如果走到这一步，意味着Page需要写入事务日志或检查点日志或两者都要有
  **
  ** First check to see that the transaction journal exists and
  ** create it if it does not.
  ** 首先检查事务日志是否存在，如果不存在则创建
  */
  assert(pPager->state != SQLITE_UNLOCK);
  rc = sqlitepager_begin(pData);
  pPager->dirtyFile = 1;
  if (rc != SQLITE_OK)
    return rc;
  assert(pPager->state == SQLITE_WRITELOCK);
  assert(pPager->journalOpen);

  /* The transaction journal now exists and we have a write lock on the
  ** main database file.  Write the current page to the transaction
  ** journal if it is not there already.
  ** 事务日志现在存在并且我们对数据库文件拥有写锁.
  ** 如果当前页面不存在，则将当前页面写入事务日志
  */
  if (!pPg->inJournal && (int)pPg->pgno <= pPager->origDbSize)
  {
    // 事务开启后在事务文件中记录下当前page的id
    rc = sqliteOsWrite(&pPager->jfd, &pPg->pgno, sizeof(Pgno));
    if (rc == SQLITE_OK)
    {
      // pageId写入到事务文件中后，将Page本次要写入的数据内容写入到journal文件中
      rc = sqliteOsWrite(&pPager->jfd, pData, SQLITE_PAGE_SIZE);
    }
    if (rc != SQLITE_OK)
    {
      // 内容写入失败，则回滚
      sqlitepager_rollback(pPager);
      pPager->errMask |= PAGER_ERR_FULL;
      return rc;
    }
    assert(pPager->aInJournal != 0);

    /**
     * 1 << (pPg->pgno&7): 截取这个Page的id最低3位,将1往左移动这个位
     * aInJournal[pPg->pgno/8]: ?
     */
    pPager->aInJournal[pPg->pgno / 8] |= 1 << (pPg->pgno & 7);
    pPager->needSync = !pPager->noSync;
    pPg->inJournal = 1;
    if (pPager->ckptInUse)
    {
      pPager->aInCkpt[pPg->pgno / 8] |= 1 << (pPg->pgno & 7);
      pPg->inCkpt = 1;
    }
  }

  /* If the checkpoint journal is open and the page is not in it,
  ** then write the current page to the checkpoint journal.
  */
  if (pPager->ckptInUse && !pPg->inCkpt && (int)pPg->pgno <= pPager->ckptSize)
  {
    assert(pPg->inJournal || (int)pPg->pgno > pPager->origDbSize);
    rc = sqliteOsWrite(&pPager->cpfd, &pPg->pgno, sizeof(Pgno));
    if (rc == SQLITE_OK)
    {
      rc = sqliteOsWrite(&pPager->cpfd, pData, SQLITE_PAGE_SIZE);
    }
    if (rc != SQLITE_OK)
    {
      sqlitepager_rollback(pPager);
      pPager->errMask |= PAGER_ERR_FULL;
      return rc;
    }
    assert(pPager->aInCkpt != 0);
    pPager->aInCkpt[pPg->pgno / 8] |= 1 << (pPg->pgno & 7);
    pPg->inCkpt = 1;
  }

  /* Update the database size and return.
   */
  if (pPager->dbSize < (int)pPg->pgno)
  {
    pPager->dbSize = pPg->pgno;
  }
  return rc;
}

/*
** Return TRUE if the page given in the argument was previously passed
** to sqlitepager_write().  In other words, return TRUE if it is ok
** to change the content of the page.
*/
int sqlitepager_iswriteable(void *pData)
{
  PgHdr *pPg = DATA_TO_PGHDR(pData);
  return pPg->dirty;
}

/*
** A call to this routine tells the pager that it is not necessary to
** write the information on page "pgno" back to the disk, even though
** that page might be marked as dirty.
**
** The overlying software layer calls this routine when all of the data
** on the given page is unused.  The pager marks the page as clean so
** that it does not get written to disk.
**
** Tests show that this optimization, together with the
** sqlitepager_dont_rollback() below, more than double the speed
** of large INSERT operations and quadruple the speed of large DELETEs.
*/
void sqlitepager_dont_write(Pager *pPager, Pgno pgno)
{
  PgHdr *pPg;
  pPg = pager_lookup(pPager, pgno);
  if (pPg && pPg->dirty)
  {
    pPg->dirty = 0;
  }
}

/*
** A call to this routine tells the pager that if a rollback occurs,
** it is not necessary to restore the data on the given page.  This
** means that the pager does not have to record the given page in the
** rollback journal.
*/
void sqlitepager_dont_rollback(void *pData)
{
  PgHdr *pPg = DATA_TO_PGHDR(pData);
  Pager *pPager = pPg->pPager;

  if (pPager->state != SQLITE_WRITELOCK || pPager->journalOpen == 0)
    return;
  if (!pPg->inJournal && (int)pPg->pgno <= pPager->origDbSize)
  {
    assert(pPager->aInJournal != 0);
    pPager->aInJournal[pPg->pgno / 8] |= 1 << (pPg->pgno & 7);
    pPg->inJournal = 1;
    if (pPager->ckptInUse)
    {
      pPager->aInCkpt[pPg->pgno / 8] |= 1 << (pPg->pgno & 7);
      pPg->inCkpt = 1;
    }
  }
  if (pPager->ckptInUse && !pPg->inCkpt && (int)pPg->pgno <= pPager->ckptSize)
  {
    assert(pPg->inJournal || (int)pPg->pgno > pPager->origDbSize);
    assert(pPager->aInCkpt != 0);
    pPager->aInCkpt[pPg->pgno / 8] |= 1 << (pPg->pgno & 7);
    pPg->inCkpt = 1;
  }
}

/*
** Commit all changes to the database and release the write lock.
** 提交所有修改到数据库并释放写锁
**
** If the commit fails for any reason, a rollback attempt is made
** and an error code is returned.  If the commit worked, SQLITE_OK
** is returned.
** 如果提交失败则回滚数据并返回错误码。如果提交成功则返回SQLITE_OK
*/
int sqlitepager_commit(Pager *pPager)
{
  int rc;
  PgHdr *pPg;

  if (pPager->errMask == PAGER_ERR_FULL)
  {
    // 写入时发生异常，回滚数据
    rc = sqlitepager_rollback(pPager);
    if (rc == SQLITE_OK)
      rc = SQLITE_FULL;
    return rc;
  }
  if (pPager->errMask != 0)
  {
    // 出现出写入异常外的其他异常
    rc = pager_errcode(pPager);
    return rc;
  }
  if (pPager->state != SQLITE_WRITELOCK)
  {
    return SQLITE_ERROR;
  }
  assert(pPager->journalOpen);
  if (pPager->dirtyFile == 0)
  {
    /* Exit early (without doing the time-consuming sqliteOsSync() calls)
    ** if there have been no changes to the database file. */
    // 如果文件没有被修改，则不需要进行写入
    // 释放数据库文件的写锁并获取读锁，同时删除日志文件
    rc = pager_unwritelock(pPager);
    pPager->dbSize = -1;
    return rc;
  }
  if (pPager->needSync && sqliteOsSync(&pPager->jfd) != SQLITE_OK)
  {
    // 开始写入先必须将日志刷入磁盘
    // 如果Page需要同步刷入磁盘，且将日志文件缓存刷入磁盘失败后进行数据回滚
    goto commit_abort;
  }
  for (pPg = pPager->pAll; pPg; pPg = pPg->pNextAll)
  {
    // 遍历整个链表
    if (pPg->dirty == 0)
      continue;
    // 获取数据头指针
    rc = sqliteOsSeek(&pPager->fd, (pPg->pgno - 1) * SQLITE_PAGE_SIZE);
    if (rc != SQLITE_OK)
      goto commit_abort;
    // 将数据写入数据库文件
    rc = sqliteOsWrite(&pPager->fd, PGHDR_TO_DATA(pPg), SQLITE_PAGE_SIZE);
    if (rc != SQLITE_OK)
      goto commit_abort;
  }
  if (!pPager->noSync && sqliteOsSync(&pPager->fd) != SQLITE_OK)
  {
    // 写入完成之后就立刻将修改刷入磁盘
    // 如果写入日志时不需要同步写入且在将数据库文件强制刷入磁盘失败
    goto commit_abort;
  }
  rc = pager_unwritelock(pPager);
  pPager->dbSize = -1;
  return rc;

  /* Jump here if anything goes wrong during the commit process.
  如果写入出现错误，进入到这里进行数据的回滚
  */
commit_abort:
  rc = sqlitepager_rollback(pPager);
  if (rc == SQLITE_OK)
  {
    rc = SQLITE_FULL;
  }
  return rc;
}

/*
** Rollback all changes.  The database falls back to read-only mode.
** All in-memory cache pages revert to their original data contents.
** The journal is deleted.
** 回滚所有修改，并且将数据库的读写状态修改为只读。
** 所有位于内存中的页面都恢复为原始内容，并删除日志文件
**
** This routine cannot fail unless some other process is not following
** the correct locking protocol (SQLITE_PROTOCOL) or unless some other
** process is writing trash into the journal file (SQLITE_CORRUPT) or
** unless a prior malloc() failed (SQLITE_NOMEM).  Appropriate error
** codes are returned for all these occasions.  Otherwise,
** SQLITE_OK is returned.
** 该函数不会失败，除非其他进程未遵循正确的锁协议(SQLITE_PROTOCOL),或者
** 其他进程正在将垃圾数据写入日志文件(SQLITE_CORRUPT),或者先前的malloc()
** 执行失败。以上三者会返回错误代码，其他情况都会返回SQLITE_OK
*/
int sqlitepager_rollback(Pager *pPager)
{
  int rc;
  if (pPager->errMask != 0 && pPager->errMask != PAGER_ERR_FULL)
  {
    // 有错误信息
    if (pPager->state >= SQLITE_WRITELOCK)
    {
      // 只有获取到数据库写锁才可以回滚
      pager_playback(pPager);
    }
    return pager_errcode(pPager);
  }
  if (pPager->state != SQLITE_WRITELOCK)
  {
    // 只有获取写锁的Pager才能够回滚
    return SQLITE_OK;
  }
  rc = pager_playback(pPager);
  if (rc != SQLITE_OK)
  {
    // 回滚失败
    rc = SQLITE_CORRUPT;
    pPager->errMask |= PAGER_ERR_CORRUPT;
  }
  // 为什么进行回滚之后需要将dbSize置为-1?
  pPager->dbSize = -1;
  return rc;
}

/*
** Return TRUE if the database file is opened read-only.  Return FALSE
** if the database is (in theory) writable.
*/
int sqlitepager_isreadonly(Pager *pPager)
{
  return pPager->readOnly;
}

/*
** This routine is used for testing and analysis only.
*/
int *sqlitepager_stats(Pager *pPager)
{
  static int a[9];
  a[0] = pPager->nRef;
  a[1] = pPager->nPage;
  a[2] = pPager->mxPage;
  a[3] = pPager->dbSize;
  a[4] = pPager->state;
  a[5] = pPager->errMask;
  a[6] = pPager->nHit;
  a[7] = pPager->nMiss;
  a[8] = pPager->nOvfl;
  return a;
}

/*
** Set the checkpoint.
**
** This routine should be called with the transaction journal already
** open.  A new checkpoint journal is created that can be used to rollback
** changes of a single SQL command within a larger transaction.
*/
int sqlitepager_ckpt_begin(Pager *pPager)
{
  int rc;
  char zTemp[SQLITE_TEMPNAME_SIZE];
  assert(pPager->journalOpen);
  assert(!pPager->ckptInUse);
  pPager->aInCkpt = malloc(pPager->dbSize / 8 + 1);
  memset(pPager->aInCkpt, 0, pPager->dbSize / 8 + 1);
  if (pPager->aInCkpt == 0)
  {
    sqliteOsReadLock(&pPager->fd);
    return SQLITE_NOMEM;
  }
  rc = sqliteOsFileSize(&pPager->jfd, &pPager->ckptJSize);
  if (rc)
    goto ckpt_begin_failed;
  pPager->ckptSize = pPager->dbSize;
  if (!pPager->ckptOpen)
  {
    rc = sqlitepager_opentemp(zTemp, &pPager->cpfd);
    if (rc)
      goto ckpt_begin_failed;
    pPager->ckptOpen = 1;
  }
  pPager->ckptInUse = 1;
  return SQLITE_OK;

ckpt_begin_failed:
  if (pPager->aInCkpt)
  {
    sqliteFree(pPager->aInCkpt);
    pPager->aInCkpt = 0;
  }
  return rc;
}

/*
** Commit a checkpoint.
*/
int sqlitepager_ckpt_commit(Pager *pPager)
{
  if (pPager->ckptInUse)
  {
    PgHdr *pPg;
    sqliteOsTruncate(&pPager->cpfd, 0);
    pPager->ckptInUse = 0;
    sqliteFree(pPager->aInCkpt);
    pPager->aInCkpt = 0;
    for (pPg = pPager->pAll; pPg; pPg = pPg->pNextAll)
    {
      pPg->inCkpt = 0;
    }
  }
  return SQLITE_OK;
}

/*
** Rollback a checkpoint.
*/
int sqlitepager_ckpt_rollback(Pager *pPager)
{
  int rc;
  if (pPager->ckptInUse)
  {
    rc = pager_ckpt_playback(pPager);
    sqlitepager_ckpt_commit(pPager);
  }
  else
  {
    rc = SQLITE_OK;
  }
  return rc;
}

#if SQLITE_TEST
/*
** Print a listing of all referenced pages and their ref count.
*/
void sqlitepager_refdump(Pager *pPager)
{
  PgHdr *pPg;
  for (pPg = pPager->pAll; pPg; pPg = pPg->pNextAll)
  {
    if (pPg->nRef <= 0)
      continue;
    printf("PAGE %3d addr=0x%08x nRef=%d\n",
           pPg->pgno, (int)PGHDR_TO_DATA(pPg), pPg->nRef);
  }
}
#endif
