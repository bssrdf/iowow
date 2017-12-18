#include "iwkv.h"
#include "iwlog.h"
#include "iwarr.h"
#include "iwutils.h"
#include "iwfsmfile.h"
#include "iwcfg.h"
#include "khash.h"
#include <stdbool.h>
#include <pthread.h>

// IWKV magic number
#define IWKV_MAGIC 0x69776b76

// IWDB magic number
#define IWDB_MAGIC 0x69776462

// Max key + value size: 255Mb
#define IWKV_MAX_KVSZ 0xfffffff

// Max database file size: ~255Gb
#define IWKV_MAX_DBSZ 0x3fffffffc0

// Size of KV fsm block as power of 2
#define IWKV_FSM_BPOW 6

// Length of KV fsm header in bytes
#define KVHDRSZ 255

// Number of skip list levels
#define SLEVELS 30

#define AANUM (2 * SLEVELS + 1)

// Lower key length in SBLK
#define SBLK_LKLEN 61

// Size of `SBLK` as power of 2
#define SBLK_SZPOW 8

// Size of `IWDB` as power of 2
#define DB_SZPOW 8

// Number of `KV` blocks in KVBLK
#define KVBLK_IDXNUM 63

// Initial `KVBLK` size power of 2 (256 bytes)
#define KVBLK_INISZPOW 8

// KVBLK header size: blen:u1,idxsz:u2
#define KVBLK_HDRSZ 3

// Max non KV size [blen:u1,idxsz:u2,[ps1:vn,pl1:vn,...,ps63,pl63]
#define KVBLK_MAX_NKV_SZ (KVBLK_HDRSZ + KVBLK_IDXNUM * 8)

#define IWKV_ISLIGHT_ERROR(rc_) \
  ((rc_) == IWKV_ERROR_NOTFOUND || (rc_) == IWKV_ERROR_KEY_EXISTS)

#define ADDR2BLK(addr_) ((addr_) >> IWKV_FSM_BPOW)

#define BLK2ADDR(blk_) (((off_t) (blk_)) << IWKV_FSM_BPOW)

volatile int8_t iwkv_next_level = -1;

struct IWKV;
struct IWDB;

typedef uint32_t blkn_t;
typedef uint32_t dbid_t;

/* Key/Value pair */
typedef struct KV {
  uint8_t *key;
  uint8_t *val;
  size_t keysz;
  size_t valsz;
} KV;

/* KV index: Offset and length. */
typedef struct KVP {
  uint32_t off;   /**< KV block offset relative to `end` of KVBLK */
  uint32_t len;   /**< Length of kv pair block */
  uint8_t  ridx;  /**< Position of the auctually persisted slot in `KVBLK` */
} KVP;

typedef enum {
  KVBLK_DURTY = 1 /**< KVBLK data is dury and should be flushed to mm */
} kvblk_flags_t;

typedef enum {
  RMKV_SYNC = 1,
  RMKV_NO_RESIZE = 1 << 1
} kvblk_rmkv_opts_t;

/* KVBLK: [blen:u1,idxsz:u2,[pp1:vn,pl1:vn,...,pp63,pl63]____[[pair],...]] */
typedef struct KVBLK {
  IWDB db;
  off_t addr;                 /**< Block address */
  uint32_t maxoff;            /**< Max pair offset */
  uint16_t idxsz;             /**< Size of KV pairs index in bytes */
  int8_t zidx;                /**< Index of first empty pair slot, or -1 */
  uint8_t szpow;              /**< Block size power of 2 */
  KVP pidx[KVBLK_IDXNUM];     /**< KV pairs index */
  kvblk_flags_t flags;        /**< Flags */
} KVBLK;

typedef enum {
  SBLK_FULL_LKEY = 1,         /**< The lowest `SBLK` key is fully contained in `SBLK`. Persistent flag. */
  SBLK_DB = 1 << 1,           /**< This block is the database block. */
  SBLK_PINNED = 1 << 2,       /**< `SBH` pinned and should not be released. */
  SBLK_WLOCKED = 1 << 3,      /**< `SBH` write locked */
  SBLK_NO_LOCK = 1 << 4,      /**< Do not use locks when accessing `SBH`(used in debug print routines) */
  SBLK_DURTY = 1 << 5
} sblk_flags_t;

#define SBLK_PERSISTENT_FLAGS (SBLK_FULL_LKEY)

/* Address lock node */
typedef struct ALN {
  pthread_rwlock_t rwl;       /**< RW lock */
  int64_t refs;               /**< Locked address refs count */
  bool write_pending;         /**< Pending write lock */
  bool write_upgraded;        /**< Lock is write upgraded */
} ALN;

KHASH_MAP_INIT_INT(ALN, ALN *)

/* Database: [magic:u4,flags:u1,dbid:u4,next_blk:u4,p0:u4,n0-n29:u4] */
struct IWDB {
  // SBH
  IWDB db;                    /**< Database ref */
  off_t addr;                 /**< Block address */
  sblk_flags_t flags;         /**< Flags */
  // !SBH
  IWKV iwkv;
  iwdb_flags_t dbflg;         /**< Database flags */
  pthread_spinlock_t sl;      /**< Main control spinlock */
  pthread_rwlock_t rwl;       /**< DB API RW lock */
  dbid_t id;                  /**< Database ID */
  uint64_t next_db_addr;      /**< Next IWDB addr */
  struct IWDB *next;          /**< Next IWDB meta */
  struct IWDB *prev;          /**< Prev IWDB meta */
  khash_t(ALN) *aln;          /**< Block id -> ALN node mapping */
};

/* Skiplist block */
typedef struct SBLK {
  // SBH
  IWDB db;                    /**< Database ref */
  off_t addr;                 /**< Block address */
  sblk_flags_t flags;         /**< Flags */
  uint8_t lvl;                /**< Skip list node level */
  blkn_t p0;                  /**< Prev node, if IWDB it is the last node */
  blkn_t n[SLEVELS];          /**< Next nodes */
  // !SBH
  KVBLK *kvblk;               /**< Associated KVBLK */
  blkn_t kvblkn;              /**< Associated KVBLK block number */
  int8_t pnum;
  int8_t pi[KVBLK_IDXNUM];
} SBLK;


KHASH_MAP_INIT_INT(DBS, IWDB)

/** IWKV instance */
struct IWKV {
  pthread_rwlock_t rwl_api;   /**< API RW lock */
  IWFS_FSM fsm;               /**< FSM pool */
  blkn_t  metablk;            /**< Database meta block */
  khash_t(DBS) *dbs;          /**< Database id -> IWDB mapping */
  IWDB last_db;               /**< Last database in chain */
  IWDB first_db;              /**< First database in chain */
  iwkv_openflags oflags;      /**< Open flags */
  bool open;                  /**< True if kvstore is in OPEN state */
};

typedef enum {
  IWLCTX_PUT = 1,             /**< Put key value operation */
  IWLCTX_DEL = 1 << 1,        /**< Delete key operation */
} iwlctx_op_t;

/** Database lookup context */
typedef struct IWLCTX {
  IWDB db;
  const IWKV_val *key;        /**< Search key */
  IWKV_val *val;              /**< Update value */
  SBLK *lower;                /**< Next to upper bound block */
  SBLK *upper;                /**< Upper bound block */
  SBLK *nb;                   /**< New block */
  SBLK *plower[SLEVELS];      /**< Pinned lower nodes per level */
  SBLK *pupper[SLEVELS];      /**< Pinned upper nodes per level */
  SBLK saa[AANUM];            /**< `SBLK` allocation area */
  KVBLK kaa[AANUM];           /**< `KVBLK` allocation area */
  uint8_t saa_pos;            /**< Position of next free `SBLK` element in the `saa` area */
  uint8_t kaa_pos;            /**< Position of next free `KVBLK` element in the `kaa` area */
  int8_t lvl;                 /**< Current level */
  int8_t nlvl;                /**< Level of new inserted `SBLK` node. -1 if no new node inserted */
  iwlctx_op_t op;             /**< Context operation */
  iwkv_opflags op_flags;      /**< Operation flags */
  sblk_flags_t sblk_flags;    /**< `SBLK` flags applied to new blocks in this context */
  
} IWLCTX;

#define AAPOS_INC(aapos_)        \
  do {                           \
    if ((aapos_) < AANUM - 1) {  \
      (aapos_) = (aapos_) + 1;   \
    } else {                     \
      (aapos_) = 0;              \
    }                            \
  } while(0)

void iwkvd_kvblk(FILE *f, KVBLK *kb);
void iwkvd_sblk(FILE *f, SBLK *sb, int flags);
void iwkvd_db(FILE *f, IWDB db, int flags);

#define ENSURE_OPEN(iwkv_) \
  if (!iwkv_ || !(iwkv_->open)) return IW_ERROR_INVALID_STATE;

#define API_RLOCK(iwkv_, rci_) \
  rci_ = pthread_rwlock_rdlock(&(iwkv_)->rwl_api); \
  if (rci_) return iwrc_set_errno(IW_ERROR_THREADING_ERRNO, rci_)

#define API_WLOCK(iwkv_, rci_) \
  rci_ = pthread_rwlock_wrlock(&(iwkv_)->rwl_api); \
  if (rci_) return iwrc_set_errno(IW_ERROR_THREADING_ERRNO, rci_)

#define API_UNLOCK(iwkv_, rci_, rc_)  \
  rci_ = pthread_rwlock_unlock(&(iwkv_)->rwl_api); \
  if (rci_) IWRC(iwrc_set_errno(IW_ERROR_THREADING_ERRNO, rci_), rc_)

#define SBLK_SZ 256
#define DB_SZ   137

// SBLK
// [u1:flags,lkl:u1,lk:u61,lvl:u1,p0:u4,n0-n29:u4,pnum:u1,kblk:u4,[pi0:u1,...pi62]]:u256
#define SOFF_FLAGS_U1     0
#define SOFF_LKL_U1       (SOFF_FLAGS_U1 + 1)
#define SOFF_LK_U61       (SOFF_LKL_U1 + 1)
#define SOFF_LVL_U1       (SOFF_LK_U61 + SBLK_LKLEN)
#define SOFF_P0_U4        (SOFF_LVL_U1 + 1)
#define SOFF_N0_U4        (SOFF_P0_U4 + 4)
#define SOFF_PNUM_U1      (SOFF_N0_U4 + 4 * SLEVELS)
#define SOFF_KBLK_U4      (SOFF_PNUM_U1 + 1)
#define SOFF_PI0_U1       (SOFF_KBLK_U4 + 4)
#define SOFF_END          (SOFF_PI0_U1 + 1 * KVBLK_IDXNUM)

// DB
// [magic:u4,dbflg:u1,dbid:u4,next_db_blk:u4,p0:u4,n0-n29:u4]
#define DOFF_MAGIC_U4     0
#define DOFF_DBFLG_U1     (DOFF_MAGIC_U4 + 4)
#define DOFF_DBID_U4      (DOFF_DBFLG_U1 + 1)
#define DOFF_NEXTDB_U4    (DOFF_DBID_U4 + 4)
#define DOFF_P0_U4        (DOFF_NEXTDB_U4 + 4)
#define DOFF_N0_U4        (DOFF_P0_U4 + 4)
#define DOFF_END          (DOFF_N0_U4 + 4 * SLEVELS)

//-------------------------- MM ACCESS

#define _mm_u1(mm_, off_) (*((uint8_t*)((mm_) + (off_))))

#define _mm_set_u1(mm_, off_, v_) (*((mm_) + (off_)) = (v_))

IW_INLINE uint32_t _mm_u4(const uint8_t *mm, off_t off) {
  uint32_t lv;
  memcpy(&lv, mm + off, sizeof(lv));
  return IW_ITOHL(lv);
}

IW_INLINE void _mm_set_u4(uint8_t *mm, off_t off, uint32_t lv) {
  lv = IW_HTOIL(lv);
  memcpy(mm + off, &lv, sizeof(lv));
}

IW_INLINE uint64_t _mm_u8(const uint8_t *mm, off_t off) {
  uint64_t llv;
  memcpy(&llv, mm + off, sizeof(llv));
  return IW_ITOHLL(llv);
}

IW_INLINE void _mm_set_u8(uint8_t *mm, off_t off, uint64_t llv) {
  llv = IW_HTOILL(llv);
  memcpy(mm + off, &llv, sizeof(llv));
}

//--------------------------  ADDRESS LOCKING

IW_INLINE iwrc _aln_release(IWDB db,
                            blkn_t blkn) {
  int rci = pthread_spin_lock(&db->sl);
  if (rci) return iwrc_set_errno(IW_ERROR_THREADING_ERRNO, rci);
  khiter_t k = kh_get(ALN, db->aln, blkn);
  if (k != kh_end(db->aln)) {
    ALN *aln = kh_value(db->aln, k);
    assert(aln);
    pthread_rwlock_unlock(&aln->rwl);
    if (--aln->refs < 1 && !aln->write_pending) {
      kh_del(ALN, db->aln, k);
      free(aln);
    }
  }
  pthread_spin_unlock(&db->sl);
  return 0;
}

IW_INLINE iwrc _aln_acquire_read(IWDB db,
                                 blkn_t blkn) {
  ALN *aln;
  int rci;
  iwrc rc = 0;
  rci = pthread_spin_lock(&db->sl);
  if (rci) return iwrc_set_errno(IW_ERROR_THREADING_ERRNO, rci);
  khiter_t k = kh_get(ALN, db->aln, blkn);
  if (k == kh_end(db->aln)) {
    aln = malloc(sizeof(*aln));
    if (!aln) {
      rc = iwrc_set_errno(IW_ERROR_ALLOC, errno);
      goto finish;
    }
    aln->write_pending = false;
    k = kh_put(ALN, db->aln, blkn, &rci);
    if (rci != -1) {
      kh_value(db->aln, k) = aln;
    } else {
      rc = IW_ERROR_FAIL;
      free(aln);
      goto finish;
    }
    aln->refs = 1;
    pthread_rwlock_init(&aln->rwl, 0);
  } else {
    aln = kh_value(db->aln, k);
    aln->refs++;
  }
finish:
  pthread_spin_unlock(&db->sl);
  if (!rc) {
    rci = pthread_rwlock_rdlock(&aln->rwl);
    if (rci) {
      return iwrc_set_errno(IW_ERROR_THREADING_ERRNO, rci);
    }
  }
  return rc;
}

IW_INLINE iwrc _aln_write_upgrade(IWDB db,
                                  blkn_t blkn) {
  ALN *aln;
  int rci;
  iwrc rc = 0;
  rci = pthread_spin_lock(&db->sl);
  if (rci) return iwrc_set_errno(IW_ERROR_THREADING_ERRNO, rci);
  khiter_t k = kh_get(ALN, db->aln, blkn);
  if (k == kh_end(db->aln)) {
    aln = malloc(sizeof(*aln));
    if (!aln) {
      rc = iwrc_set_errno(IW_ERROR_ALLOC, errno);
      goto finish;
    }
    k = kh_put(ALN, db->aln, blkn, &rci);
    if (rci != -1) {
      kh_value(db->aln, k) = aln;
    } else {
      rc = IW_ERROR_FAIL;
      free(aln);
      goto finish;
    }
    aln->refs = 1;
    pthread_rwlock_init(&aln->rwl, 0);
  } else {
    aln = kh_value(db->aln, k);
    pthread_rwlock_unlock(&aln->rwl);
    assert(aln->refs > 0);
    aln->refs--;
  }
  aln->write_pending = true;
finish:
  pthread_spin_unlock(&db->sl);
  if (!rc) {
    rci = pthread_rwlock_wrlock(&aln->rwl);
    if (rci) {
      if (aln->refs < 1) {
        rci = pthread_spin_lock(&db->sl);
        if (rci) return iwrc_set_errno(IW_ERROR_THREADING_ERRNO, rci);
        if (aln->refs < 1) {
          kh_del(ALN, db->aln, k);
          free(aln);
        }
        pthread_spin_unlock(&db->sl);
      }
      return iwrc_set_errno(IW_ERROR_THREADING_ERRNO, rci);
    }
    aln->refs = 1;
    aln->write_pending = false;
  }
  return rc;
}

IW_INLINE void _kv_dispose(IWKV_val *key,
                           IWKV_val *val) {
  if (key) {
    if (key->data) {
      free(key->data);
    }
    key->size = 0;
    key->data = 0;
  }
  if (val) {
    if (val->data) {
      free(val->data);
    }
    val->size = 0;
    val->data = 0;
  }
}

//--------------------------  DB

static iwrc _db_at(IWKV iwkv,
                   IWDB *dbp,
                   off_t addr,
                   uint8_t *mm) {
  iwrc rc = 0;
  uint8_t *rp;
  uint32_t lv;
  int rci;
  IWDB db = calloc(1, sizeof(struct IWDB));
  *dbp = 0;
  if (!db) {
    return iwrc_set_errno(IW_ERROR_ALLOC, errno);
  }
  rci = pthread_spin_init(&db->sl, 0);
  if (rci) {
    free(db);
    return iwrc_set_errno(IW_ERROR_THREADING_ERRNO, rci);
  }
  rci = pthread_rwlock_init(&db->rwl, 0);
  if (rci) {
    pthread_spin_destroy(&db->sl);
    free(db);
    return iwrc_set_errno(IW_ERROR_THREADING_ERRNO, rci);
  }
  // [magic:u4,dbflg:u1,dbid:u4,next_db_addr:u4,p0:u4,n0-n29:u4]
  db->flags = SBLK_DB;
  db->addr = addr;
  db->db = db;
  db->iwkv = iwkv;
  db->aln = kh_init(ALN);
  rp = mm + addr;
  IW_READLV(rp, lv, lv);
  if (lv != IWDB_MAGIC) {
    rc = IWKV_ERROR_CORRUPTED;
    goto finish;
  }
  IW_READBV(rp, lv, db->dbflg);
  IW_READLV(rp, lv, db->id);
  IW_READLV(rp, lv, db->next_db_addr);
  db->next_db_addr = BLK2ADDR(db->next_db_addr); // blknum -> addr
  *dbp = db;
finish:
  if (rc)  {
    kh_destroy(ALN, (*dbp)->aln);
    pthread_spin_destroy(&db->sl);
    pthread_rwlock_destroy(&db->rwl);
    free(db);
  }
  return rc;
}

static void _db_save(IWDB db, uint8_t *mm) {
  uint32_t lv;
  uint8_t *wp = mm + db->addr;
  db->next_db_addr = db->next ? db->next->addr : 0;
  // [magic:u4,dbflg:u1,dbid:u4,next_db_addr:u4,p0:u4,n0-n29:u4]
  IW_WRITELV(wp, lv, IWDB_MAGIC);
  IW_WRITEBV(wp, lv, db->dbflg);
  IW_WRITELV(wp, lv, db->id);
  IW_WRITELV(wp, lv, ADDR2BLK(db->next_db_addr));
}

static iwrc _db_load_chain(IWKV iwkv, off_t addr, uint8_t *mm) {
  iwrc rc;
  int rci;
  IWDB db = 0, ndb;
  if (!addr) return 0;
  do {
    rc = _db_at(iwkv, &ndb, addr, mm);
    RCRET(rc);
    if (db) {
      db->next = ndb;
      ndb->prev = db;
    } else {
      iwkv->first_db = ndb;
    }
    db = ndb;
    addr = db->next_db_addr;
    iwkv->last_db = db;
    khiter_t k = kh_put(DBS, iwkv->dbs, db->id, &rci);
    if (rci != -1) {
      kh_value(iwkv->dbs, k) = db;
    } else {
      rc = IW_ERROR_FAIL;
      return rc;
    }
  } while (db->next_db_addr);
  return rc;
}

static void _db_release_lw(IWDB *dbp) {
  assert(dbp && *dbp);
  pthread_spin_destroy(&(*dbp)->sl);
  pthread_rwlock_destroy(&(*dbp)->rwl);
  kh_destroy(ALN, (*dbp)->aln);
  free(*dbp);
  *dbp = 0;
}

static iwrc _db_destroy_lw(IWDB *dbp) {
  iwrc rc;
  uint8_t *mm;
  IWDB db = *dbp;
  IWDB prev = db->prev;
  IWDB next = db->next;
  IWFS_FSM *fsm = &db->iwkv->fsm;
  
  kh_del(DBS, db->iwkv->dbs, db->id);
  rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
  if (prev) {
    prev->next = next;
    _db_save(prev, mm);
  }
  if (next) {
    next->prev = prev;
    _db_save(next, mm);
  }
  fsm->release_mmap(fsm);
  if (db->iwkv->first_db && db->iwkv->first_db->addr == db->addr) {
    uint64_t llv;
    db->iwkv->first_db = next;
    llv = next ? next->addr : 0;
    llv = IW_HTOILL(llv);
    rc = fsm->writehdr(fsm, sizeof(uint32_t) /*skip magic*/, &llv, sizeof(llv));
  }
  if (db->iwkv->last_db && db->iwkv->last_db->addr == db->addr) {
    db->iwkv->last_db = prev;
  }
  //
  // TODO!!!: dispose all of `SBLK` & `KVBLK` blocks used by db
  //
  IWRC(fsm->deallocate(fsm, db->addr, (1 << DB_SZPOW)), rc);
  _db_release_lw(dbp);
  return rc;
}

static iwrc _db_create_lw(IWKV iwkv,
                          dbid_t dbid,
                          iwdb_flags_t dbflg,
                          IWDB *odb) {
  iwrc rc;
  int rci;
  uint8_t *mm;
  off_t baddr = 0, blen;
  IWFS_FSM *fsm = &iwkv->fsm;
  *odb = 0;
  IWDB db = calloc(1, sizeof(struct IWDB));
  if (!db) {
    return iwrc_set_errno(IW_ERROR_ALLOC, errno);
  }
  rc = fsm->allocate(fsm, (1 << DB_SZPOW), &baddr, &blen,
                     IWFSM_ALLOC_NO_OVERALLOCATE | IWFSM_SOLID_ALLOCATED_SPACE);
  if (rc) {
    _db_release_lw(&db);
    return rc;
  }
  db->iwkv = iwkv;
  db->dbflg = dbflg;
  db->addr = baddr;
  db->id = dbid;
  db->prev = iwkv->last_db;
  db->aln = kh_init(ALN);
  if (!iwkv->first_db) {
    uint64_t llv;
    iwkv->first_db = db;
    llv = db->addr;
    llv = IW_HTOILL(llv);
    rc = fsm->writehdr(fsm, sizeof(uint32_t) /*skip magic*/, &llv, sizeof(llv));
  } else if (iwkv->last_db) {
    iwkv->last_db->next = db;
  }
  iwkv->last_db = db;
  khiter_t k = kh_put(DBS, iwkv->dbs, db->id, &rci);
  if (rci != -1) {
    kh_value(iwkv->dbs, k) = db;
  } else {
    RCGO(IW_ERROR_FAIL, finish);
  }
  rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
  RCGO(rc, finish);
  _db_save(db, mm);
  if (db->prev) {
    _db_save(db->prev, mm);
  }
  fsm->release_mmap(fsm);
  *odb = db;
finish:
  if (rc) {
    fsm->deallocate(fsm, baddr, blen);
    _db_release_lw(&db);
  }
  return rc;
}

//--------------------------  KVBLK

static iwrc _kvblk_create(IWLCTX *lx,
                          int8_t kvbpow,
                          KVBLK **oblk) {
  KVBLK *kblk;
  off_t baddr = 0, blen;
  IWFS_FSM *fsm = &lx->db->iwkv->fsm;
  if (kvbpow < KVBLK_INISZPOW) {
    kvbpow = KVBLK_INISZPOW;
  }
  assert(lx->kaa_pos < AANUM);
  kblk = &lx->kaa[lx->kaa_pos];
  memset(lx->kaa, 0, sizeof(lx->kaa[0]));
  iwrc rc = fsm->allocate(fsm, (1ULL << kvbpow), &baddr, &blen,
                          IWFSM_ALLOC_NO_OVERALLOCATE | IWFSM_SOLID_ALLOCATED_SPACE);
  RCRET(rc);
  kblk->db = lx->db;
  kblk->addr = baddr;
  kblk->szpow = KVBLK_INISZPOW;
  kblk->idxsz = 2 * IW_VNUMSIZE(0) * KVBLK_IDXNUM;
  kblk->flags = KVBLK_DURTY;
  *oblk = kblk;
  AAPOS_INC(lx->kaa_pos);
  return rc;
}

IW_INLINE iwrc _kvblk_destroy(KVBLK **kbp) {
  assert(kbp && *kbp && (*kbp)->db && (*kbp)->szpow && (*kbp)->addr);
  KVBLK *blk = *kbp;
  IWFS_FSM *fsm = &blk->db->iwkv->fsm;
  iwrc rc = fsm->deallocate(fsm, blk->addr, 1ULL << blk->szpow);
  return rc;
}

IW_INLINE void _kvblk_peek_key(const KVBLK *kb,
                               uint8_t idx,
                               const uint8_t *mm,
                               uint8_t **obuf,
                               uint32_t *olen) {
  assert(idx < KVBLK_IDXNUM);
  if (kb->pidx[idx].len) {
    uint32_t klen, step;
    const uint8_t *rp = mm + kb->addr + (1ULL << kb->szpow) - kb->pidx[idx].off;
    IW_READVNUMBUF(rp, klen, step);
    assert(klen);
    rp += step;
    *obuf = (uint8_t *) rp;
    *olen = klen;
  } else {
    *obuf = 0;
    *olen = 0;
  }
}

IW_INLINE void _kvblk_peek_val(const KVBLK *kb,
                               uint8_t idx,
                               const uint8_t *mm,
                               uint8_t **obuf,
                               uint32_t *olen) {
  assert(idx < KVBLK_IDXNUM);
  if (kb->pidx[idx].len) {
    uint32_t klen, step;
    const uint8_t *rp = mm + kb->addr + (1ULL << kb->szpow) - kb->pidx[idx].off;
    IW_READVNUMBUF(rp, klen, step);
    rp += step;
    rp += klen;
    *obuf = (uint8_t *) rp;
    *olen = kb->pidx[idx].len - klen - step;
  } else {
    *obuf = 0;
    *olen = 0;
  }
}

static iwrc _kvblk_getkey(KVBLK *kb,
                          uint8_t *mm,
                          uint8_t idx,
                          IWKV_val *key) {
  assert(mm && idx < KVBLK_IDXNUM);
  int32_t klen;
  int step;
  KVP *kvp = &kb->pidx[idx];
  if (!kvp->len) {
    key->data = 0;
    key->size = 0;
    return 0;
  }
  // [klen:vn,key,value]
  uint8_t *rp = mm + kb->addr + (1ULL << kb->szpow) - kvp->off;
  IW_READVNUMBUF(rp, klen, step);
  rp += step;
  if (klen < 1 || klen > kvp->len || klen > kvp->off) {
    return IWKV_ERROR_CORRUPTED;
  }
  key->size = klen;
  key->data = malloc(key->size);
  if (!key->data) {
    return iwrc_set_errno(IW_ERROR_ALLOC, errno);
  }
  memcpy(key->data, rp, key->size);
  rp += key->size;
  return 0;
}

static iwrc _kvblk_getvalue(KVBLK *kb,
                            uint8_t *mm,
                            uint8_t idx,
                            IWKV_val *val) {
  assert(mm && idx < KVBLK_IDXNUM);
  int32_t klen;
  int step;
  KVP *kvp = &kb->pidx[idx];
  if (!kvp->len) {
    val->data = 0;
    val->size = 0;
    return 0;
  }
  // [klen:vn,key,value]
  uint8_t *rp = mm + kb->addr + (1ULL << kb->szpow) - kvp->off;
  IW_READVNUMBUF(rp, klen, step);
  rp += step;
  if (klen < 1 || klen > kvp->len || klen > kvp->off) {
    return IWKV_ERROR_CORRUPTED;
  }
  rp += klen;
  if (kvp->len > klen + step) {
    val->size = kvp->len - klen - step;
    val->data = malloc(val->size);
    if (!val->data) {
      iwrc rc = iwrc_set_errno(IW_ERROR_ALLOC, errno);
      val->data = 0;
      val->size = 0;
      return rc;
    }
    memcpy(val->data, rp, val->size);
  } else {
    val->data = 0;
    val->size = 0;
  }
  return 0;
}

static iwrc _kvblk_getkv(uint8_t *mm,
                         KVBLK *kb,
                         uint8_t idx,
                         IWKV_val *key,
                         IWKV_val *val) {
  assert(mm && idx < KVBLK_IDXNUM);
  int32_t klen;
  int step;
  KVP *kvp = &kb->pidx[idx];
  if (!kvp->len) {
    key->data = 0;
    key->size = 0;
    val->data = 0;
    val->size = 0;
    return 0;
  }
  // [klen:vn,key,value]
  uint8_t *rp = mm + kb->addr + (1ULL << kb->szpow) - kvp->off;
  IW_READVNUMBUF(rp, klen, step);
  rp += step;
  if (klen < 1 || klen > kvp->len || klen > kvp->off) {
    return IWKV_ERROR_CORRUPTED;
  }
  key->size = klen;
  key->data = malloc(key->size);
  if (!key->data) {
    return iwrc_set_errno(IW_ERROR_ALLOC, errno);
  }
  memcpy(key->data, rp, key->size);
  rp += klen;
  if (kvp->len > klen + step) {
    val->size = kvp->len - klen - step;
    val->data = malloc(val->size);
    if (!val->data) {
      iwrc rc = iwrc_set_errno(IW_ERROR_ALLOC, errno);
      free(key->data);
      key->data = 0;
      key->size = 0;
      val->size = 0;
      return rc;
    }
    memcpy(val->data, rp, val->size);
  } else {
    val->data = 0;
    val->size = 0;
  }
  return 0;
}

static iwrc _kvblk_at_mm(IWLCTX *lx,
                         off_t addr,
                         uint8_t *mm,
                         KVBLK *kbp,
                         KVBLK **blkp) {
  uint8_t *rp, *sp;
  uint16_t sv;
  int step;
  iwrc rc = 0;
  KVBLK *kb = kbp ? kbp : &lx->kaa[lx->kaa_pos];
  memset(kb, 0, sizeof(*kb));
  
  *blkp = 0;
  rp = mm + addr;
  kb->db = lx->db;
  kb->addr = addr;
  kb->maxoff = 0;
  kb->zidx = -1;
  IW_READBV(rp, kb->szpow, kb->szpow);
  IW_READSV(rp, sv, kb->idxsz);
  if (IW_UNLIKELY(kb->idxsz > 2 * 4 * KVBLK_IDXNUM)) {
    rc = IWKV_ERROR_CORRUPTED;
    goto finish;
  }
  sp = rp;
  for (int i = 0; i < KVBLK_IDXNUM; ++i) {
    IW_READVNUMBUF(rp, kb->pidx[i].off, step);
    rp += step;
    IW_READVNUMBUF(rp, kb->pidx[i].len, step);
    rp += step;
    if (IW_UNLIKELY(rp - sp > kb->idxsz)) {
      rc = IWKV_ERROR_CORRUPTED;
      goto finish;
    }
    kb->pidx[i].ridx = i;
    if (kb->pidx[i].len) {
      if (IW_UNLIKELY(!kb->pidx[i].off)) {
        rc = IWKV_ERROR_CORRUPTED;
        goto finish;
      }
      if (kb->pidx[i].off > kb->maxoff) {
        kb->maxoff = kb->pidx[i].off;
      }
    } else if (kb->zidx == -1) {
      kb->zidx = i;
    }
  }
  *blkp = kb;
  if (!kbp) {
    AAPOS_INC(lx->kaa_pos);
  }
finish:
  return rc;
}

IW_INLINE iwrc _kvblk_at(IWLCTX *lx,
                         SBLK *sblk,
                         KVBLK **blkp) {
  if (sblk->kvblk) {
    *blkp = sblk->kvblk;
    return 0;
  }
  iwrc rc;
  uint8_t *mm;
  IWFS_FSM *fsm = &lx->db->iwkv->fsm;
  *blkp = 0;
  rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
  RCRET(rc);
  rc = _kvblk_at_mm(lx, BLK2ADDR(sblk->kvblkn), mm, 0, blkp);
  IWRC(fsm->release_mmap(fsm), rc);
  return rc;
}

static void _kvblk_sync_mm(KVBLK *kb,
                           uint8_t *mm) {
  if (!(kb->flags & KVBLK_DURTY)) {
    return;
  }
  uint8_t *szp;
  uint16_t sp;
  uint8_t *wp = mm + kb->addr;
  memcpy(wp, &kb->szpow, 1);
  wp += 1;
  szp = wp;
  wp += sizeof(uint16_t);
  for (int i = 0; i < KVBLK_IDXNUM; ++i) {
    KVP *kvp = &kb->pidx[i];
    IW_SETVNUMBUF(sp, wp, kvp->off);
    wp += sp;
    IW_SETVNUMBUF(sp, wp, kvp->len);
    wp += sp;
  }
  sp = wp - szp - sizeof(uint16_t);
  kb->idxsz = sp;
  sp = IW_HTOIS(sp);
  memcpy(szp, &sp, sizeof(uint16_t));
  kb->flags &= ~KVBLK_DURTY;
}

IW_INLINE off_t _kvblk_compacted_offset(KVBLK *kb) {
  off_t coff = 0;
  for (int i = 0; i < KVBLK_IDXNUM; ++i) {
    KVP *kvp = kb->pidx + i;
    coff += kvp->len;
  }
  return coff;
}

static int _kvblk_sort_kv(const void *v1,
                          const void *v2) {
  uint32_t o1 = ((KVP *) v1)->off > 0 ? ((KVP *) v1)->off : -1UL;
  uint32_t o2 = ((KVP *) v2)->off > 0 ? ((KVP *) v2)->off : -1UL;
  return o1 > o2 ? 1 : o1 < o2 ? -1 : 0;
}

static void _kvblk_compact(KVBLK *kb,
                           uint8_t *mm) {
  uint8_t i;
  off_t coff = _kvblk_compacted_offset(kb);
  if (coff == kb->maxoff) { // already compacted
    return;
  }
  KVP tidx[KVBLK_IDXNUM];
  uint8_t *wp = mm + kb->addr + (1ULL << kb->szpow);
  memcpy(tidx, kb->pidx, sizeof(tidx));
  qsort(tidx, KVBLK_IDXNUM, sizeof(KVP), _kvblk_sort_kv);
  coff = 0;
  for (i = 0; i < KVBLK_IDXNUM && tidx[i].off; ++i) {
#ifndef NDEBUG
    if (i > 0) {
      assert(tidx[i - 1].off < tidx[i].off);
    }
#endif
    KVP *kvp = &kb->pidx[tidx[i].ridx];
    off_t noff = coff + kvp->len;
    if (kvp->off > noff) {
      memmove(wp - noff, wp - kvp->off, kvp->len);
      kvp->off = noff;
    }
    coff += kvp->len;
    kb->maxoff = coff;
  }
  for (i = 0; i < KVBLK_IDXNUM; ++i) {
    if (!kb->pidx[i].len)  {
      kb->zidx = i;
      break;
    }
  }
  if (i == KVBLK_IDXNUM) {
    kb->zidx = -1;
  }
  kb->flags |= KVBLK_DURTY;
  _kvblk_sync_mm(kb, mm);
}

IW_INLINE uint64_t _kvblk_datasize(KVBLK *kb) {
  uint64_t dsz = KVBLK_HDRSZ + kb->idxsz;
  for (int i = 0; i < KVBLK_IDXNUM; ++i) {
    dsz += kb->pidx[i].len;
  }
  return dsz;
}

IW_INLINE off_t _kvblk_maxkvoff(KVBLK *kb) {
  off_t off = 0;
  for (int i = 0; i < KVBLK_IDXNUM; ++i) {
    if (kb->pidx[i].off > off) {
      off = kb->pidx[i].off;
    }
  }
  return off;
}

iwrc _kvblk_rmkv(KVBLK *kb,
                 uint8_t idx,
                 kvblk_rmkv_opts_t opts) {
  uint64_t sz;
  iwrc rc = 0;
  uint8_t *mm = 0;
  IWFS_FSM *fsm = &kb->db->iwkv->fsm;
  if (kb->pidx[idx].off >= kb->maxoff) {
    kb->maxoff = 0;
    for (int i = 0; i < KVBLK_IDXNUM; ++i) {
      if (i != idx && kb->pidx[i].off > kb->maxoff) {
        kb->maxoff = kb->pidx[i].off;
      }
    }
  }
  kb->pidx[idx].len = 0;
  kb->pidx[idx].off = 0;
  if (kb->zidx < 0 || idx < kb->zidx) {
    kb->zidx = idx;
  }
  kb->flags |= KVBLK_DURTY;
  if (!(RMKV_NO_RESIZE & opts)) {
    rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
    RCGO(rc, finish);
    uint64_t kbsz = 1ULL << kb->szpow;
    uint64_t dsz = _kvblk_datasize(kb);
    uint8_t dpow = 1;
    sz = kbsz / 2;
    while ((kb->szpow - dpow) > KVBLK_INISZPOW && dsz < sz / 2) {
      sz = sz / 2;
      dpow++;
    }
    if ((kb->szpow - dpow) >= KVBLK_INISZPOW && dsz < kbsz / 2) { // We can shrink kvblock
      _kvblk_compact(kb, mm);
      off_t naddr = kb->addr, nlen = kbsz;
      off_t maxoff = _kvblk_maxkvoff(kb);
      memmove(mm + kb->addr + sz - maxoff,
              mm + kb->addr + kbsz - maxoff,
              maxoff);
      fsm->release_mmap(fsm);
      mm = 0;
      rc = fsm->reallocate(fsm, sz, &naddr, &nlen, IWFSM_ALLOC_NO_OVERALLOCATE | IWFSM_SOLID_ALLOCATED_SPACE);
      RCGO(rc, finish);
      kb->addr = naddr;
      kb->szpow = kb->szpow - dpow;
      opts |= RMKV_SYNC;
    }
  }
  if (RMKV_SYNC & opts) {
    if (!mm) {
      rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
      RCGO(rc, finish);
    }
    _kvblk_sync_mm(kb, mm);
  }
finish:
  if (mm) {
    fsm->release_mmap(fsm);
  }
  return rc;
}

static iwrc _kvblk_addkv(KVBLK *kb,
                         const IWKV_val *key,
                         const IWKV_val *val,
                         int8_t *oidx) {
  iwrc rc = 0;
  off_t msz;    // max available free space
  off_t rsz;    // required size to add new key/value pair
  off_t noff;   // offset of new kvpair from end of block
  uint8_t *mm, *wp;
  size_t i, sp;
  KVP *kvp;
  IWFS_FSM *fsm = &kb->db->iwkv->fsm;
  off_t psz = (key->size + val->size) + IW_VNUMSIZE(key->size); // required size
  bool compacted = false;
  *oidx = -1;
  
  if (psz > IWKV_MAX_KVSZ) {
    return IWKV_ERROR_MAXKVSZ;
  }
  if (kb->zidx < 0) {
    return _IWKV_ERROR_KVBLOCK_FULL;
  }
  
start:
  msz = (1ULL << kb->szpow) - KVBLK_HDRSZ - kb->idxsz - kb->maxoff;
  noff = kb->maxoff + psz;
  rsz = psz + IW_VNUMSIZE(noff) + IW_VNUMSIZE(psz) - 2;
  if (msz < rsz) { // not enough space
    if (!compacted) {
      rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
      RCGO(rc, finish);
      _kvblk_compact(kb, mm);
      compacted = true;
      fsm->release_mmap(fsm);
      goto start;
    } else { // resize the whole block
      off_t nsz = (rsz - msz) + (1ULL << kb->szpow);
      uint8_t npow = kb->szpow;
      while ((1ULL << ++npow) < nsz);
      off_t naddr = kb->addr,
            nlen = (1ULL << kb->szpow);
      rc = fsm->reallocate(fsm, (1ULL << npow), &naddr, &nlen, IWFSM_ALLOC_NO_OVERALLOCATE | IWFSM_SOLID_ALLOCATED_SPACE);
      RCGO(rc, finish);
      assert(nlen == (1ULL << npow));
      // Move pairs area
      // [hdr..[pairs]] =reallocate=> [hdr..[pairs]_____] =memove=> [hdr.._____[pairs]]
      rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
      RCGO(rc, finish);
      memmove(mm + naddr + nlen - kb->maxoff, mm + naddr + (1ULL << kb->szpow) - kb->maxoff, kb->maxoff);
      fsm->release_mmap(fsm);
      kb->addr = naddr;
      kb->szpow = npow;
      goto start;
    }
  }
  *oidx = kb->zidx;
  kvp = &kb->pidx[kb->zidx];
  kvp->len = psz;
  kvp->off = noff;
  kvp->ridx = kb->zidx;
  kb->maxoff = noff;
  kb->flags |= KVBLK_DURTY;
  for (i = 0; i < KVBLK_IDXNUM; ++i) {
    if (!kb->pidx[i].len && i != kb->zidx) {
      kb->zidx = i;
      break;
    }
  }
  if (i >= KVBLK_IDXNUM) {
    kb->zidx = -1;
  }
  rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
  RCGO(rc, finish);
  wp = mm + kb->addr + (1ULL << kb->szpow) - kvp->off;
  // [klen:vn,key,value]
  IW_SETVNUMBUF(sp, wp, key->size);
  wp += sp;
  memcpy(wp, key->data, key->size);
  wp += key->size;
  memcpy(wp, val->data, val->size);
  fsm->release_mmap(fsm);
finish:
  return rc;
}

static iwrc _kvblk_updatev(KVBLK *kb,
                           int8_t *idxp,
                           const IWKV_val *key,
                           const IWKV_val *val) {
  assert(*idxp < KVBLK_IDXNUM);
  int32_t klen, i;
  size_t sz;
  int8_t idx = *idxp;
  uint8_t *mm, *wp, *sp;
  KVP *kvp = &kb->pidx[idx];
  IWFS_FSM *fsm = &kb->db->iwkv->fsm;
  size_t rsize = IW_VNUMSIZE(key->size) + key->size + val->size; // required size
  iwrc rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
  RCRET(rc);
  wp = mm + kb->addr + (1ULL << kb->szpow) - kvp->off;
  sp = wp;
  IW_READVNUMBUF(wp, klen, sz);
  wp += sz;
  if (klen != key->size || memcmp(wp, key->data, key->size)) {
    rc = IWKV_ERROR_CORRUPTED;
    goto finish;
  }
  wp += klen;
  if (rsize <= kvp->len) {
    memcpy(wp, val->data, val->size);
    wp += val->size;
    if ((wp - sp) != kvp->len) {
      kvp->len = wp - sp;
      kb->flags |= KVBLK_DURTY;
    }
  } else {
    KVP tidx[KVBLK_IDXNUM];
    uint32_t koff = kb->pidx[idx].off;
    memcpy(tidx, kb->pidx, KVBLK_IDXNUM * sizeof(kb->pidx[0]));
    qsort(tidx, KVBLK_IDXNUM, sizeof(KVP), _kvblk_sort_kv);
    kb->flags |= KVBLK_DURTY;
    for (i = 0; i < KVBLK_IDXNUM; ++i) {
      if (tidx[i].off == koff) {
        if (koff - (i > 0 ? tidx[i - 1].off : 0) >= rsize) {
          memcpy(wp, val->data, val->size);
          wp += val->size;
          kvp->len = wp - sp;
        } else {
          fsm->release_mmap(fsm);
          mm = 0;
          rc = _kvblk_rmkv(kb, idx, RMKV_NO_RESIZE);
          RCGO(rc, finish);
          rc = _kvblk_addkv(kb, key, val, idxp);
        }
        break;
      }
    }
  }
finish:
  if (mm) {
    IWRC(fsm->release_mmap(fsm), rc);
  }
  return rc;
}

//--------------------------  SBLK

IW_INLINE void _sblk_release(IWLCTX *lx, SBLK **sblkp) {
  assert(sblkp && *sblkp);
  if (!((*sblkp)->flags & SBLK_NO_LOCK)) {
    _aln_release(lx->db, ADDR2BLK((*sblkp)->addr));
  }
  (*sblkp)->kvblk = 0;
  *sblkp = 0;
}

IW_INLINE iwrc _sblk_destroy(IWLCTX *lx,
                             SBLK **sblkp) {
  assert(sblkp && *sblkp && (*sblkp)->addr);
  iwrc rc = 0;
  SBLK *sblk = *sblkp;
  if (!(sblk->flags & SBLK_DB)) {
    IWFS_FSM *fsm = &lx->db->iwkv->fsm;
    off_t kvb_addr = BLK2ADDR(sblk->kvblkn),
          sblk_addr = sblk->addr;
    uint8_t kvb_szpow = sblk->kvblk->szpow;
    _sblk_release(lx, sblkp);
    rc = fsm->deallocate(fsm, sblk_addr, 1 << SBLK_SZPOW);
    IWRC(fsm->deallocate(fsm, kvb_addr, 1ULL << kvb_szpow), rc);
  } else {
    _sblk_release(lx, sblkp);
  }
  return rc;
}

static uint8_t _sblk_genlevel() {
  int8_t lvl;
  if (iwkv_next_level >= 0) {
    lvl = iwkv_next_level;
    iwkv_next_level = -1;
    return lvl;
  }
  uint32_t r = iwu_rand_u32();
  for (lvl = 0; lvl < SLEVELS && !(r & 1); ++lvl) {
    r >>= 1;
  }
  return IW_UNLIKELY(lvl >= SLEVELS) ? SLEVELS - 1 : lvl;
}

static iwrc _sblk_create(IWLCTX *lx,
                         int8_t nlevel,
                         int8_t kvbpow,
                         SBLK **oblk) {
  iwrc rc;
  SBLK *sblk;
  KVBLK *kvblk;
  off_t baddr = 0, blen;
  IWFS_FSM *fsm = &lx->db->iwkv->fsm;
  *oblk = 0;
  rc = _kvblk_create(lx, kvbpow, &kvblk);
  RCRET(rc);
  rc = fsm->allocate(fsm, 1 << SBLK_SZPOW, &baddr, &blen,
                     IWFSM_ALLOC_NO_OVERALLOCATE | IWFSM_SOLID_ALLOCATED_SPACE);
  if (IW_UNLIKELY(rc)) {
    IWRC(_kvblk_destroy(&kvblk), rc);
    return rc;
  }
  sblk = &lx->saa[lx->saa_pos];
  memset(sblk, 0, sizeof(*sblk));
  sblk->db = lx->db;
  sblk->addr = baddr;
  sblk->lvl = nlevel;
  sblk->kvblk = kvblk;
  sblk->kvblkn = ADDR2BLK(kvblk->addr);
  sblk->flags |= SBLK_DURTY;
  rc = _aln_acquire_read(lx->db, sblk->addr);
  if (!rc) {
    *oblk = sblk;
    AAPOS_INC(lx->saa_pos);
  }
  return rc;
}

static iwrc _sblk_at_mm(IWLCTX *lx, off_t addr, sblk_flags_t flags, uint8_t *mm, SBLK **sblkp) {
  iwrc rc = 0;
  if (!(flags & SBLK_NO_LOCK)) {
    rc = _aln_acquire_read(lx->db, addr);
    RCRET(rc);
  }
  uint32_t lv;
  uint8_t *rp = mm + addr;
  SBLK *sblk = &lx->saa[lx->saa_pos];
  memset(sblk, 0, sizeof(*sblk));
  if (IW_UNLIKELY(addr == lx->db->addr)) {
    // [magic:u4,dbflg:u1,dbid:u4,next_db_blk:u4,p0:u4,n0-n29:u4]:u137
    sblk->addr = addr;
    sblk->flags = SBLK_DB | flags;
    sblk->db = lx->db;
    rp += DOFF_P0_U4;
    sblk->lvl = 0;
    sblk->pnum = KVBLK_IDXNUM;
    IW_READLV(rp, lv, sblk->p0);
    for (int i = 0; i < SLEVELS; ++i) {
      IW_READLV(rp, lv, sblk->n[i]);
      if (sblk->n[i]) ++sblk->lvl;
    }
  } else {
    sblk->db = lx->db;
    sblk->addr = addr;
    // [u1:flags,lkl:u1,lk:u61,lvl:u1,p0:u4,n0-n29:u4,pnum:u1,kblk:u4,[pi0:u1,...pi62]]:u256
    memcpy(&sblk->flags, rp + SOFF_FLAGS_U1, 1);
    sblk->flags |= flags;
    rp += SOFF_LVL_U1;
    memcpy(&sblk->lvl, rp, 1);
    rp += 1;
    IW_READLV(rp, lv, sblk->p0);
    for (int i = 0; i < SLEVELS; ++i) {
      IW_READLV(rp, lv, sblk->n[i]);
    }
    memcpy(&sblk->pnum, rp, 1);
    rp += 1;
    IW_READLV(rp, lv, sblk->kvblkn);
    memcpy(sblk->pi, rp, KVBLK_IDXNUM);
  }
  AAPOS_INC(lx->saa_pos);
  return 0;
}


IW_INLINE iwrc _sblk_write_upgrade_mm(IWLCTX *lx, SBLK *sblk, uint8_t *mm) {
  if (sblk->flags & SBLK_WLOCKED) {
    return 0;
  }
  uint32_t lv;
  uint8_t *mmp = mm;
  sblk_flags_t flags;
  IWFS_FSM *fsm = &lx->db->iwkv->fsm;
  iwrc rc = _aln_write_upgrade(sblk->db, ADDR2BLK(sblk->addr));
  RCRET(rc);
  if (!mmp) {
    rc = fsm->acquire_mmap(fsm, 0, &mmp, 0);
    RCRET(rc);
  }
  uint8_t *rp = mmp + sblk->addr;
  if (sblk->flags & SBLK_DB) {
    // [magic:u4,dbflg:u1,dbid:u4,next_db_blk:u4,p0:u4,n0-n29:u4]:u137
    rp += DOFF_P0_U4;
    sblk->lvl = 0;
    IW_READLV(rp, lv, sblk->p0);
    for (int i = 0; i < SLEVELS; ++i) {
      IW_READLV(rp, lv, sblk->n[i]);
      if (sblk->n[i]) ++sblk->lvl;
    }
  } else {
    // [u1:flags,lkl:u1,lk:u61,lvl:u1,p0:u4,n0-n29:u4,pnum:u1,kblk:u4,[pi0:u1,...pi62]]:u256
    memcpy(&flags, rp + SOFF_FLAGS_U1, 1);
    sblk->flags &= ~SBLK_PERSISTENT_FLAGS;
    sblk->flags |= flags;
    rp += SOFF_LVL_U1;
    memcpy(&sblk->lvl, rp, 1);
    rp += 1;
    IW_READLV(rp, lv, sblk->p0);
    for (int i = 0; i < SLEVELS; ++i) {
      IW_READLV(rp, lv, sblk->n[i]);
    }
    memcpy(&sblk->pnum, rp, 1);
    rp += 1;
    IW_READLV(rp, lv, sblk->kvblkn);
    memcpy(sblk->pi, rp, KVBLK_IDXNUM);
    if (sblk->kvblk && sblk->kvblk->addr != BLK2ADDR(sblk->kvblkn)) {
      // Re-read kvblk since it changed (reallocated)
      rc = _kvblk_at_mm(lx, BLK2ADDR(sblk->kvblkn), mm, sblk->kvblk, &sblk->kvblk);
    }
  }
  sblk->flags |= SBLK_WLOCKED;
  if (mmp != mm) {
    fsm->release_mmap(fsm);
  }
  return rc;
}

IW_INLINE void _sblk_sync_mm(SBLK *sblk, uint8_t *mm) {
  assert(sblk && sblk->addr);
  if (sblk->flags & SBLK_DURTY) {
    uint32_t lv;
    uint8_t *wp = mm + sblk->addr;
    sblk->flags &= ~SBLK_DURTY;
    if (IW_UNLIKELY(sblk->flags & SBLK_DB)) {
      // [magic:u4,dbflg:u1,dbid:u4,next_db_blk:u4,p0:u4,n0-n29:u4]:u137
      wp += DOFF_P0_U4;
      IW_WRITELV(wp, lv, sblk->p0);
      for (int i = 0; i < SLEVELS; ++i) {
        IW_WRITELV(wp, lv, sblk->n[i]);
      }
      return;
    } else {
      // [u1:flags,lkl:u1,lk:u61,lvl:u1,p0:u4,n0-n29:u4,pnum:u1,kblk:u4,[pi0:u1,...pi62]]:u256
      wp += SOFF_FLAGS_U1;
      sblk_flags_t flags = (sblk->flags & SBLK_PERSISTENT_FLAGS);
      memcpy(wp, &flags, 1);
      wp = mm + sblk->addr + SOFF_LVL_U1;
      memcpy(wp, &sblk->lvl, 1);
      wp += 1;
      IW_WRITELV(wp, lv, sblk->p0);
      for (int i = 0; i < SLEVELS; ++i) {
        IW_WRITELV(wp, lv, sblk->n[i]);
      }
      memcpy(wp, &sblk->pnum, 1);
      wp += 1;
      IW_WRITELV(wp, lv, sblk->kvblkn);
      memcpy(wp, sblk->pi, KVBLK_IDXNUM);
    }
  }
  if (sblk->kvblk && (sblk->kvblk->flags & KVBLK_DURTY)) {
    _kvblk_sync_mm(sblk->kvblk, mm);
  }
}

IW_INLINE void _sblk_sync_and_release_mm(IWLCTX *lx, SBLK **sblkp, uint8_t *mm) {
  if (mm) {
    _sblk_sync_mm(*sblkp, mm);
  }
  _sblk_release(lx, sblkp);
}

static int _sblk_find_pi(SBLK *sblk,
                         const IWKV_val *key,
                         const uint8_t *mm,
                         bool *found) {
  if (sblk->flags & SBLK_DB) {
    *found = false;
    return KVBLK_IDXNUM;
  }
  uint8_t *k;
  uint32_t kl;
  int idx = 0,
      lb = 0,
      ub = sblk->pnum - 1;
  *found = false;
  if (sblk->pnum < 1) {
    return 0;
  }
  while (1) {
    int cr;
    idx = (ub + lb) / 2;
    _kvblk_peek_key(sblk->kvblk, sblk->pi[idx], mm, &k, &kl);
    assert(kl > 0);
    IW_CMP(cr, k, kl, key->data, key->size);
    if (!cr) {
      *found = true;
      break;
    } else if (cr < 0) {
      lb = idx + 1;
      if (lb > ub) {
        idx = lb;
        break;
      }
    } else {
      ub = idx - 1;
      if (lb > ub) {
        break;
      }
    }
  }
  return idx;
}

static int _sblk_insert_pi(SBLK *sblk,
                           int8_t nidx,
                           const IWKV_val *key,
                           const uint8_t *mm) {
  uint8_t *k;
  uint32_t kl;
  int idx = 0,
      lb = 0,
      ub = sblk->pnum - 1,
      nels = sblk->pnum;
  if (nels < 1) {
    sblk->pi[0] = nidx;
    sblk->pnum++;
    return 0;
  }
  while (1) {
    int cr;
    idx = (ub + lb) / 2;
    _kvblk_peek_key(sblk->kvblk, sblk->pi[idx], mm, &k, &kl);
    assert(kl > 0);
    IW_CMP(cr, k, kl, key->data, key->size);
    if (!cr) {
      break;
    } else if (cr < 0) {
      lb = idx + 1;
      if (lb > ub) {
        idx = lb;
        sblk->pnum++;
        break;
      }
    } else {
      ub = idx - 1;
      if (lb > ub) {
        sblk->pnum++;
        break;
      }
    }
  }
  if (nels - idx > 0) {
    memmove(sblk->pi + idx + 1, sblk->pi + idx, nels - idx);
  }
  sblk->pi[idx] = nidx;
  return idx;
}

IW_INLINE iwrc _sblk_addkv2(IWLCTX *lx,
                            SBLK *sblk,
                            int8_t idx,
                            const IWKV_val *key,
                            const IWKV_val *val) {
  assert(lx && sblk && key && key->size && key->data && val && idx >= 0 && sblk->kvblk);
  uint8_t *mm;
  int8_t kvidx;
  KVBLK *kvblk = sblk->kvblk;
  IWFS_FSM *fsm = &sblk->db->iwkv->fsm;
  if (sblk->pnum >= KVBLK_IDXNUM) {
    return _IWKV_ERROR_KVBLOCK_FULL;
  }
  iwrc rc = _kvblk_addkv(kvblk, key, val, &kvidx);
  RCRET(rc);
  if (sblk->pnum - idx > 0) {
    memmove(sblk->pi + idx + 1, sblk->pi + idx, sblk->pnum - idx);
  }
  if (idx == 0) {
    rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
    RCRET(rc);
    uint8_t lkl = MIN(SBLK_LKLEN, key->size);
    _mm_set_u1(mm, sblk->addr + SOFF_LKL_U1, lkl);
    memcpy(mm + sblk->addr + SOFF_LK_U61, key->data, lkl);
    if (key->size <= SBLK_LKLEN) {
      sblk->flags |= SBLK_FULL_LKEY;
    } else {
      sblk->flags &= ~SBLK_FULL_LKEY;
    }
    fsm->release_mmap(fsm);
  }
  sblk->pi[idx] = kvidx;
  sblk->pnum++;
  sblk->flags |= SBLK_DURTY;
  return 0;
}

IW_INLINE iwrc _sblk_addkv(SBLK *sblk,
                           const IWKV_val *key,
                           const IWKV_val *val) {
  assert(key && key->size && key->data && val && sblk->kvblk);
  int8_t kvidx;
  uint8_t *mm;
  KVBLK *kvblk = sblk->kvblk;
  IWFS_FSM *fsm = &sblk->db->iwkv->fsm;
  if (sblk->pnum >= KVBLK_IDXNUM) {
    return _IWKV_ERROR_KVBLOCK_FULL;
  }
  iwrc rc = _kvblk_addkv(kvblk, key, val, &kvidx);
  RCRET(rc);
  rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
  RCRET(rc);
  if (_sblk_insert_pi(sblk, kvidx, key, mm) == 0) { // the lowest key inserted
    uint8_t lkl = MIN(SBLK_LKLEN, key->size);
    _mm_set_u1(mm, sblk->addr + SOFF_LKL_U1, lkl);
    memcpy(mm + sblk->addr + SOFF_LK_U61, key->data, lkl);
    if (key->size <= SBLK_LKLEN) {
      sblk->flags |= SBLK_FULL_LKEY;
    } else {
      sblk->flags &= ~SBLK_FULL_LKEY;
    }
  }
  fsm->release_mmap(fsm);
  sblk->flags |= SBLK_DURTY;
  return 0;
}

IW_INLINE iwrc _sblk_updatekv(SBLK *sblk,
                              int8_t idx,
                              const IWKV_val *key,
                              const IWKV_val *val) {
  assert(idx >= 0 && idx < sblk->pnum);
  KVBLK *kvblk = sblk->kvblk;
  int8_t kvidx = sblk->pi[idx];
  iwrc rc = _kvblk_updatev(kvblk, &kvidx, key, val);
  RCRET(rc);
  sblk->pi[idx] = kvidx;
  sblk->flags |= SBLK_DURTY;
  return 0;
}

IW_INLINE iwrc _sblk_rmkv(SBLK *sblk,
                          uint8_t idx) {
  KVBLK *kvblk = sblk->kvblk;
  IWFS_FSM *fsm = &sblk->db->iwkv->fsm;
  assert(kvblk && idx < sblk->pnum && sblk->pi[idx] < KVBLK_IDXNUM);
  iwrc rc = _kvblk_rmkv(kvblk, sblk->pi[idx], 0);
  RCRET(rc);
  sblk->pnum--;
  sblk->flags |= SBLK_DURTY;
  if (idx < sblk->pnum && sblk->pnum > 0) {
    memmove(sblk->pi + idx, sblk->pi + idx + 1, sblk->pnum - idx);
  }
  if (idx == 0) {
    uint8_t *mm;
    rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
    RCRET(rc);
    // Lowest key removed, replace it with the next key or reset
    if (sblk->pnum > 0) {
      uint32_t klen;
      uint8_t *kbuf, *mm, lkl;
      _kvblk_peek_key(sblk->kvblk, sblk->pi[idx], mm, &kbuf, &klen);
      lkl = MIN(SBLK_LKLEN, klen);
      _mm_set_u1(mm, sblk->addr + SOFF_LKL_U1, lkl);
      memcpy(mm + sblk->addr + SOFF_LK_U61, kbuf, lkl);
      if (lkl <= SBLK_LKLEN) {
        sblk->flags |= SBLK_FULL_LKEY;
      } else {
        sblk->flags &= ~SBLK_FULL_LKEY;
      }
    } else {
      _mm_set_u1(mm, sblk->addr + SOFF_LKL_U1, 0);
    }
    fsm->release_mmap(fsm);
  }
  return rc;
}

IW_INLINE iwrc _sblk_create2(IWLCTX *lx,
                             int8_t nlevel,
                             int8_t kvbpow,
                             const IWKV_val *key,
                             IWKV_val *val,
                             SBLK **oblk) {
  SBLK *sblk;
  *oblk = 0;
  iwrc rc = _sblk_create(lx, nlevel, kvbpow, &sblk);
  RCRET(rc);
  rc = _sblk_addkv(sblk, key, val);
  if (rc) {
    _sblk_destroy(lx, &sblk);
  } else {
    *oblk = sblk;
  }
  return rc;
}

//--------------------------  IWLCTX

IW_INLINE int _lx_sblk_cmp_key(IWLCTX *lx, SBLK *sblk, uint8_t *mm) {
  int res;
  uint8_t *k;
  uint32_t kl;
  const IWKV_val *key = lx->key;
  if (sblk->pnum < 1 && (sblk->flags & SBLK_DB)) { // empty block
    return -1;
  }
  kl = _mm_u1(mm, sblk->addr + SOFF_LKL_U1);
  if (key->size < kl) {
    k = mm + sblk->addr + SOFF_LK_U61;
    IW_CMP(res, k, kl, key->data, key->size);
    return res;
  }
  if (sblk->flags & SBLK_FULL_LKEY) {
    k = mm + sblk->addr + SOFF_LK_U61;
  } else {
    _kvblk_peek_key(sblk->kvblk, sblk->pi[0] /* lower key index */, mm, &k, &kl);
    if (!kl) {
      return -1;
    }
  }
  IW_CMP(res, k, kl, key->data, key->size);
  return res;
}

static iwrc _lx_roll_forward(IWLCTX *lx, uint8_t *mm, bool key2upper) {
  iwrc rc = 0;
  SBLK *sblk;
  blkn_t blkn;
  uint8_t lvl = lx->lvl;
  assert(lx->lower);
  while ((blkn = lx->lower->n[lvl])) {
    off_t blkaddr = BLK2ADDR(blkn);
    if (lx->nlvl > -1 && lvl < lx->nlvl) {
      int8_t ulvl = lvl + 1;
      if (lx->pupper[ulvl] && lx->pupper[ulvl]->addr == blkaddr) {
        sblk = lx->pupper[ulvl];
      } else if (lx->plower[ulvl] && lx->plower[ulvl]->addr == blkaddr) {
        sblk = lx->plower[ulvl];
      } else {
        rc = _sblk_at_mm(lx, blkaddr, lx->sblk_flags, mm, &sblk);
      }
    } else {
      if (lx->upper && lx->upper->addr == blkaddr) {
        break;
      } else {
        rc = _sblk_at_mm(lx, blkaddr, lx->sblk_flags, mm, &sblk);
      }
    }
    RCRET(rc);
    int cret = _lx_sblk_cmp_key(lx, sblk, mm);
    if (key2upper ? cret >= 0 : cret > 0) { // upper >|>= key
      if (lx->upper && !(lx->upper->flags & SBLK_PINNED)) {
        _sblk_release(lx, &lx->upper);
      }
      lx->upper = sblk;
      break;
    } else {
      if (!(lx->lower->flags & SBLK_PINNED)) {
        _sblk_release(lx, &lx->lower);
      }
      lx->lower = sblk;
    }
  }
  return 0;
}

static iwrc _lx_find_bounds(IWLCTX *lx, bool key2upper) {
  SBLK *sblk;
  uint8_t *mm;
  IWFS_FSM *fsm  = &lx->db->iwkv->fsm;
  iwrc rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
  RCRET(rc);
  assert(!lx->lower);
  rc = _sblk_at_mm(lx, lx->db->addr, lx->sblk_flags, mm, &lx->lower);
  RCGO(rc, finish);
  for (int lvl = lx->lower->lvl; lvl >= 0;) {
    lx->lvl = lvl;
    rc = _lx_roll_forward(lx, mm, key2upper);
    RCGO(rc, finish);
    blkn_t ub = lx->upper ? ADDR2BLK(lx->upper->addr) : 0;
    do {
      lx->lvl = lvl;
      if (lx->nlvl >= lvl) {
        lx->lower->flags |= SBLK_PINNED;
        lx->plower[lvl] = lx->lower;
        if (lx->upper) {
          lx->upper->flags |= SBLK_PINNED;
          lx->pupper[lvl] = lx->upper;
        }
      }
    } while (lvl-- > 0 && lx->lower->n[lvl] == ub);
  }
finish:
  fsm->release_mmap(fsm);
  return rc;
}

static void _lx_release_mm(IWLCTX *lx, uint8_t *mm) {
  if (lx->nlvl) {
    SBLK *lsb = 0, *usb = 0, *db = 0;
    if (lx->nb) {
      _sblk_sync_and_release_mm(lx, &lx->nb, mm);
    }
    if (lx->pupper[lx->nlvl] && (lx->pupper[lx->nlvl]->flags & SBLK_DB)) {
      db = lx->pupper[lx->nlvl];
      _sblk_sync_and_release_mm(lx, &lx->pupper[lx->nlvl], mm);
    }
    if (!db && lx->plower[lx->nlvl] && (lx->plower[lx->nlvl]->flags & SBLK_DB)) {
      db = lx->plower[lx->nlvl];
      _sblk_sync_and_release_mm(lx, &lx->plower[lx->nlvl], mm);
    }
    for (int i = lx->nlvl; i >= 0; --i) {
      if (lx->pupper[i]) {
        if (lx->pupper[i] != usb && lx->pupper[i] != db) {
          usb = lx->pupper[i];
          _sblk_sync_and_release_mm(lx, &lx->pupper[i], mm);
        }
        lx->pupper[i] = 0;
      }
      if (lx->plower[i]) {
        if (lx->plower[i] != lsb && lx->plower[i] != db) {
          lsb = lx->plower[i];
          _sblk_sync_and_release_mm(lx, &lx->plower[i], mm);
        }
        lx->plower[i] = 0;
      }
    }
  } else {
    if (lx->upper) {
      _sblk_sync_and_release_mm(lx, &lx->upper, mm);
    }
    if (lx->lower) {
      _sblk_sync_and_release_mm(lx, &lx->lower, mm);
    }
  }
}

IW_INLINE iwrc _lx_release(IWLCTX *lx) {
  uint8_t *mm;
  IWFS_FSM *fsm = &lx->db->iwkv->fsm;
  iwrc rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
  RCRET(rc);
  _lx_release_mm(lx, mm);
  return fsm->release_mmap(fsm);
}

static iwrc _lx_split_addkv(IWLCTX *lx, int idx, SBLK *sblk) {
  iwrc rc;
  SBLK *nb;
  blkn_t nblk;
  uint8_t *mm;
  uint8_t kvbpow = 0;
  IWDB db = lx->db;
  IWFS_FSM *fsm = &lx->db->iwkv->fsm;
  int pivot = (KVBLK_IDXNUM / 2) + 1; // 32
  assert(sblk->flags & SBLK_WLOCKED);
  
  if (idx == sblk->pnum && lx->upper && lx->upper->pnum < KVBLK_IDXNUM) {
    // Good to place lv into the right(upper) block
    return _sblk_addkv(lx->upper, lx->key, lx->val);
  }
  if (idx > 0 && idx < sblk->pnum) {
    // Partial split required
    // Compute space required for the new sblk which stores kv pairs after pivot `idx`
    size_t sz = 0;
    for (int i = pivot; i < sblk->pnum; ++i) {
      sz += sblk->kvblk->pidx[sblk->pi[i]].len;
    }
    if (idx > pivot) {
      sz += IW_VNUMSIZE(lx->key->size) + lx->key->size + lx->val->size;
    }
    kvbpow = iwlog2_64(KVBLK_MAX_NKV_SZ + sz);
  }
  rc = _sblk_create(lx, lx->nlvl, kvbpow, &nb);
  RCRET(rc);
  nblk = ADDR2BLK(nb->addr);
  
  if (idx == sblk->pnum) {
    // Upper side
    rc = _sblk_addkv(nb, lx->key, lx->val);
    RCGO(rc, finish);
    
  } else if (idx == 0) {
    // Lowest side
    // [u1:flags,lkl:u1,lk:u61,lvl:u1,p0:u4,n0-n29:u4,pnum:u1,kblk:u4,[pi0:u1,...pi62]]:u256
    SBLK nbk, sbk;  // SBLK backup
    uint8_t lkl;    // Lower sblk key len backup
    memcpy(&nbk, nb, sizeof(*nb));
    memcpy(&sbk, sblk, sizeof(*sblk));
    memcpy(&lkl, mm + sblk->addr + SOFF_LKL_U1, 1);
    rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
    RCGO(rc, finish);
    // Move sblk keys to nb
    nb->flags = sblk->flags | SBLK_DURTY;
    nb->pnum = sblk->pnum;
    nb->kvblk = sblk->kvblk;
    nb->kvblkn = sblk->kvblkn;
    memcpy(nb->pi, sblk->pi, sizeof(sblk->pi));
    memcpy(mm + nbk.addr + SOFF_LKL_U1,
           mm + sblk->addr + SOFF_LKL_U1,
           1 + SBLK_LKLEN);
    sblk->flags = nbk.flags;
    sblk->pnum = nbk.pnum;
    sblk->kvblk = nbk.kvblk;
    sblk->kvblkn = nbk.kvblkn;
    memset(sblk->pi, 0, sizeof(sblk->pi));
    memset(mm + sblk->addr + SOFF_LKL_U1, 0, 1); // reset lkl
    fsm->release_mmap(fsm);
    rc = _sblk_addkv(sblk, lx->key, lx->val);
    if (rc) {
      // restore previous state
      iwrc rc1 = fsm->acquire_mmap(fsm, 0, &mm, 0);
      RCGO(rc1, finish);
      memcpy(sblk, &sbk, sizeof(*sblk));
      memcpy(nb, &nbk, sizeof(*nb));
      // restore LK data
      memcpy(mm + sblk->addr + SOFF_LKL_U1, &lkl, 1);
      fsm->release_mmap(fsm);
      goto finish;
    }
  } else {
    // We are in the middle
    // Do the partial split
    // Move kv pairs into new `nb`
    IWKV_val key, val;
    for (int i = pivot, end = sblk->pnum; i < end; ++i) {
      rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
      RCBREAK(rc);
      rc = _kvblk_getkv(mm, sblk->kvblk, sblk->pi[i], &key, &val);
      assert(key.size);
      fsm->release_mmap(fsm);
      RCBREAK(rc);
      rc = _sblk_addkv(nb, &key, &val);
      _kv_dispose(&key, &val);
      RCBREAK(rc);
      sblk->kvblk->pidx[sblk->pi[i]].len = 0;
      sblk->kvblk->pidx[sblk->pi[i]].off = 0;
      sblk->pnum--;
      if (i == pivot) {
        sblk->kvblk->zidx = sblk->pi[i];
      }
    }
    // sync maxoff
    sblk->kvblk->maxoff = 0;
    for (int i = 0; i < KVBLK_IDXNUM; ++i) {
      if (sblk->kvblk->pidx[i].off > sblk->kvblk->maxoff) {
        sblk->kvblk->maxoff = sblk->kvblk->pidx[i].off;
      }
    }
    if (idx > pivot) {
      rc = _sblk_addkv(nb, lx->key, lx->val);
    } else {
      rc = _sblk_addkv(sblk, lx->key, lx->val);
    }
    RCGO(rc, finish);
  }
  // fix levels
  //  [ lb -> sblk -> ub ]
  //  [ lb -> sblk -> nb -> ub ]
  lx->pupper[0]->p0 = nblk;
  lx->pupper[0]->flags |= SBLK_DURTY;
  for (int i = 0; i <= nb->lvl; ++i) {
    lx->plower[i]->n[i] = nblk;
    lx->plower[i]->flags |= SBLK_DURTY;
    nb->n[i] = (lx->pupper[i]->addr != lx->db->addr) ? ADDR2BLK(lx->pupper[i]->addr) : 0;
  }
finish:
  if (rc) {
    lx->nb = 0;
    IWRC(_sblk_destroy(lx, &nb), rc);
  } else {
    lx->nb = nb;
  }
  return rc;
}

iwrc _lx_lock_chute_mm(IWLCTX *lx, uint8_t *mm) {
  assert(lx->nlvl >= 0);
  iwrc rc = 0;
  SBLK *db = 0;
  blkn_t dblk = ADDR2BLK(lx->db->addr);
  if (lx->plower[lx->nlvl] && (lx->plower[lx->nlvl]->flags & SBLK_DB)) {
    assert(lx->plower[lx->nlvl]->addr == BLK2ADDR(dblk));
    db = lx->plower[lx->nlvl];
  }
  if (!lx->pupper[lx->nlvl]) {
    if (!db) {
      rc = _sblk_at_mm(lx, lx->db->addr, 0, mm, &db);
      RCRET(rc);
    }
    lx->pupper[lx->nlvl] = db;
    for (int i = lx->nlvl - 1; i >= 0 && !lx->pupper[i]; --i) {
      lx->pupper[i] = db;
    }
  }
  for (int i = 0; i <= lx->nlvl; ++i) {
    rc = _sblk_write_upgrade_mm(lx, lx->plower[i], mm);
    RCRET(rc);
    blkn_t bn = lx->plower[i]->n[i] ? lx->plower[i]->n[i] : dblk;
    if (bn != ADDR2BLK(lx->pupper[i]->addr)) {
      // address was changed
      return _IWKV_ERROR_AGAIN;
    }
    if (i == 0) {
      rc = _sblk_write_upgrade_mm(lx, lx->pupper[0], mm);
      RCRET(rc);
      bn = lx->pupper[0]->p0 ? lx->pupper[0]->p0 : dblk;
      if (bn != ADDR2BLK(lx->plower[0]->addr)) {
        // prev ptr was changed
        return _IWKV_ERROR_AGAIN;
      }
    }
  }
  return 0;
}

IW_INLINE WUR iwrc _lx_addkv(IWLCTX *lx, SBLK *sblk) {
  int idx;
  bool found;
  uint8_t *mm;
  IWFS_FSM *fsm = &lx->db->iwkv->fsm;
  iwrc rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
  RCRET(rc);
  rc = _sblk_write_upgrade_mm(lx, sblk, mm);
  if (rc) {
    fsm->release_mmap(fsm);
    return rc;
  }
  idx = _sblk_find_pi(sblk, lx->key, mm, &found);
  if (found && (lx->op_flags & IWKV_NO_OVERWRITE)) {
    fsm->release_mmap(fsm);
    return IWKV_ERROR_KEY_EXISTS;
  }
  if (!found && sblk->pnum >= KVBLK_IDXNUM) {
    if (lx->nlvl < 0) {
      fsm->release_mmap(fsm);
      return _IWKV_ERROR_REQUIRE_NLEVEL;
    }
    // lock sblk body chute
    rc = _lx_lock_chute_mm(lx, mm);
    if (rc) {
      fsm->release_mmap(fsm);
      return rc;
    }
  }
  fsm->release_mmap(fsm);
  if (!found && sblk->pnum >= KVBLK_IDXNUM) {
    rc = _lx_split_addkv(lx, idx, sblk);
  } else {
    if (!found) {
      rc = _sblk_addkv2(lx, sblk, idx, lx->key, lx->val);
    } else {
      rc = _sblk_updatekv(sblk, idx, lx->key, lx->val);
    }
  }
  return rc;
}

iwrc _lx_put_lr(IWLCTX *lx) {
  iwrc rc;
start:
  rc = _lx_find_bounds(lx, false);
  if (rc) {
    _lx_release_mm(lx, 0);
    return rc;
  }
  rc = _lx_addkv(lx, lx->lower);
  if (rc == _IWKV_ERROR_AGAIN || rc == _IWKV_ERROR_REQUIRE_NLEVEL) {
    _lx_release_mm(lx, 0);
    if (rc == _IWKV_ERROR_REQUIRE_NLEVEL) {
      lx->nlvl = _sblk_genlevel();
    }
    goto start;
  }
  return _lx_release(lx);
}

iwrc _lx_get_lr(IWLCTX *lx) {
  iwrc rc = _lx_find_bounds(lx, false);
  RCRET(rc);
  lx->val->size = 0;
  if (!(lx->lower->flags & SBLK_DB)) {
    bool found;
    uint8_t *mm;
    IWFS_FSM *fsm = &lx->db->iwkv->fsm;
    rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
    RCRET(rc);
    int idx = _sblk_find_pi(lx->lower, lx->key, mm, &found);
    if (found) {
      idx = lx->lower->pi[idx];
      rc = _kvblk_getvalue(lx->lower->kvblk, mm, idx, lx->val);
    } else {
      rc = IWKV_ERROR_NOTFOUND;
    }
    rc = fsm->release_mmap(fsm);
  }
  _lx_release_mm(lx, 0);
  return rc;
}

iwrc _lx_del_lr(IWLCTX *lx, bool dbwlocked) {
  iwrc rc;
  int idx;
  bool found;
  uint8_t *mm;
  IWFS_FSM *fsm = &lx->db->iwkv->fsm;
  rc = _lx_find_bounds(lx, true);
  if (!lx->upper) {
    _lx_release_mm(lx, 0);
    return IWKV_ERROR_NOTFOUND;
  }
  rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
  RCRET(rc);
  idx = _sblk_find_pi(lx->upper, lx->key, mm, &found);
  if (!found) {
    fsm->release_mmap(fsm);
    _lx_release_mm(lx, 0);
    return IWKV_ERROR_NOTFOUND;;
  }
  
  if (lx->upper->pnum <= 1 && !dbwlocked) {
    // ask DB write lock
    return _IWKV_ERROR_REQUIRE_WLOCK;
  }
  rc = _sblk_rmkv(lx->upper, idx);
  RCRET(rc);
  if (lx->upper->pnum < 1) {
    //for (int i = 0; i <= lx->upper->lvl && i <= ;)
  }
  
  // fix levels
  //  [ lb -> sblk -> ub ]
  //  [ lb -> sblk -> nb -> ub ]
  //  lx->pupper[0]->p0 = nblk;
  //  lx->pupper[0]->flags |= SBLK_DURTY;
  //  for (int i = 0; i <= nb->lvl; ++i) {
  //    lx->plower[i]->n[i] = nblk;
  //    lx->plower[i]->flags |= SBLK_DURTY;
  //    nb->n[i] = (lx->pupper[i]->addr != lx->db->addr) ? ADDR2BLK(lx->pupper[i]->addr) : 0;
  //  }  
finish:
  
  return rc;
}

//--------------------------  PUBLIC API

static const char *_kv_ecodefn(locale_t locale, uint32_t ecode) {
  if (!(ecode > _IWKV_ERROR_START && ecode < _IWKV_ERROR_END)) {
    return 0;
  }
  switch (ecode) {
    case IWKV_ERROR_NOTFOUND:
      return "Key not found. (IWKV_ERROR_NOTFOUND)";
    case IWKV_ERROR_KEY_EXISTS:
      return "Key exists. (IWKV_ERROR_KEY_EXISTS)";
    case IWKV_ERROR_MAXKVSZ:
      return "Size of Key+value must be lesser than 0xfffffff bytes (IWKV_ERROR_MAXKVSZ)";
    case IWKV_ERROR_MAXDBSZ:
      return "Database file size reached its maximal limit: 0x3fffffffc0 bytes (IWKV_ERROR_MAXDBSZ)";
    case IWKV_ERROR_CORRUPTED:
      return "Database file invalid or corrupted (IWKV_ERROR_CORRUPTED)";
  }
  return 0;
}

iwrc iwkv_init(void) {
  static int _kv_initialized = 0;
  if (!__sync_bool_compare_and_swap(&_kv_initialized, 0, 1)) {
    return 0;  // initialized already
  }
  int64_t t;
  iwrc rc = iw_init();
  RCRET(rc);
  rc = iwp_current_time_ms(&t);
  RCRET(rc);
  iwu_rand_seed(t / 1000);
  return iwlog_register_ecodefn(_kv_ecodefn);
}

iwrc iwkv_open(const IWKV_OPTS *opts, IWKV *iwkvp) {
  assert(iwkvp && opts);
  iwrc rc = 0;
  uint32_t lv;
  uint64_t llv;
  uint8_t *rp, *mm;
  rc = iwkv_init();
  RCRET(rc);
  *iwkvp = calloc(1, sizeof(struct IWKV));
  if (!*iwkvp) {
    return iwrc_set_errno(IW_ERROR_ALLOC, errno);
  }
  IWKV iwkv = *iwkvp;
  pthread_rwlock_init(&iwkv->rwl_api, 0);
  iwkv_openflags oflags = opts->oflags;
  iwfs_omode omode = IWFS_OREAD;
  if (oflags & IWKV_TRUNC) {
    oflags &= ~IWKV_RDONLY;
    omode |= IWFS_OTRUNC;
  }
  if (!(oflags & IWKV_RDONLY)) {
    omode |= IWFS_OWRITE;
  }
  iwkv->oflags = oflags;
  IWFS_FSM_STATE fsmstate;
  IWFS_FSM_OPTS fsmopts = {
    .exfile = {
      .file = {
        .path       = opts->path,
        .omode      = omode,
        .lock_mode  = (oflags & IWKV_RDONLY) ? IWP_RLOCK : IWP_WLOCK
      },
      .rspolicy     = iw_exfile_szpolicy_fibo
    },
    .bpow = IWKV_FSM_BPOW,      // 64 bytes block size
    .hdrlen = KVHDRSZ,          // Size of custom file header
    .oflags = ((oflags & (IWKV_NOLOCKS | IWKV_RDONLY)) ? IWFSM_NOLOCKS : 0),
    .mmap_all = 1
    //!!!! todo implement: .maxoff = IWKV_MAX_DBSZ
  };
  rc = iwfs_fsmfile_open(&iwkv->fsm, &fsmopts);
  RCGO(rc, finish);
  IWFS_FSM *fsm  = &iwkv->fsm;
  iwkv->dbs = kh_init(DBS);
  rc = fsm->state(fsm, &fsmstate);
  RCGO(rc, finish);
  
  if (fsmstate.exfile.file.ostatus & IWFS_OPEN_NEW) {
    // Write magic number
    lv = IWKV_MAGIC;
    lv = IW_HTOIL(lv);
    rc = fsm->writehdr(fsm, 0, &lv, sizeof(lv));
    RCGO(rc, finish);
    fsm->sync(fsm, 0);
  } else {
    uint8_t hdr[KVHDRSZ];
    rc = fsm->readhdr(fsm, 0, hdr, KVHDRSZ);
    RCGO(rc, finish);
    rp = hdr;
    memcpy(&lv, rp, sizeof(lv));
    rp += sizeof(lv);
    lv = IW_ITOHL(lv);
    if (lv != IWKV_MAGIC) {
      rc = IWKV_ERROR_CORRUPTED;
      goto finish;
    }
    memcpy(&llv, rp, sizeof(llv));
    llv = IW_ITOHLL(llv);
    rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
    RCGO(rc, finish);
    rc = _db_load_chain(iwkv, llv, mm);
    fsm->release_mmap(fsm);
  }
  (*iwkvp)->open = true;
finish:
  if (rc) {
    (*iwkvp)->open = true;
    IWRC(iwkv_close(iwkvp), rc);
  }
  return rc;
}

iwrc iwkv_close(IWKV *iwkvp) {
  ENSURE_OPEN((*iwkvp));
  int rci;
  iwrc rc = 0;
  IWKV iwkv = *iwkvp;
  API_WLOCK(iwkv, rci);
  iwkv->open = false;
  IWDB db = iwkv->first_db;
  while (db) {
    IWDB ndb = db->next;
    _db_release_lw(&db);
    db = ndb;
  }
  IWRC(iwkv->fsm.close(&iwkv->fsm), rc);
  if (iwkv->dbs) {
    kh_destroy(DBS, iwkv->dbs);
    iwkv->dbs = 0;
  }
  API_UNLOCK(iwkv, rci, rc);
  pthread_rwlock_destroy(&iwkv->rwl_api);
  free(iwkv);
  *iwkvp = 0;
  return rc;
}

iwrc iwkv_sync(IWKV iwkv) {
  ENSURE_OPEN(iwkv);
  if (iwkv->oflags & IWKV_RDONLY) {
    return IW_ERROR_READONLY;
  }
  iwrc rc = 0;
  IWFS_FSM *fsm  = &iwkv->fsm;
  int rci = pthread_rwlock_rdlock(&iwkv->rwl_api);
  if (rci) rc = iwrc_set_errno(IW_ERROR_THREADING_ERRNO, rci);
  IWRC(fsm->sync(fsm, IWFS_FDATASYNC), rc);
  pthread_rwlock_unlock(&iwkv->rwl_api);
  return rc;
}

iwrc iwkv_db(IWKV iwkv, uint32_t dbid, iwdb_flags_t flags, IWDB *dbp) {
  ENSURE_OPEN(iwkv);
  int rci;
  iwrc rc = 0;
  IWDB db = 0;
  *dbp = 0;
  API_RLOCK(iwkv, rci);
  khiter_t ki = kh_get(DBS, iwkv->dbs, dbid);
  if (ki != kh_end(iwkv->dbs)) {
    db = kh_value(iwkv->dbs, ki);
  }
  API_UNLOCK(iwkv, rci, rc);
  RCRET(rc);
  if (db) {
    *dbp = db;
    return 0;
  }
  if (iwkv->oflags & IWKV_RDONLY) {
    return IW_ERROR_READONLY;
  }
  API_WLOCK(iwkv, rci);
  ki = kh_get(DBS, iwkv->dbs, dbid);
  if (ki != kh_end(iwkv->dbs)) {
    db = kh_value(iwkv->dbs, ki);
  }
  if (db) {
    *dbp = db;
  } else {
    rc = _db_create_lw(iwkv, dbid, flags, dbp);
  }
  API_UNLOCK(iwkv, rci, rc);
  return rc;
}

iwrc iwkv_db_destroy(IWDB *dbp) {
  assert(dbp && *dbp);
  int rci;
  iwrc rc = 0;
  IWKV iwkv = (*dbp)->iwkv;
  if (iwkv->oflags & IWKV_RDONLY) {
    return IW_ERROR_READONLY;
  }
  ENSURE_OPEN(iwkv);
  API_WLOCK(iwkv, rci);
  rc = _db_destroy_lw(dbp);
  API_UNLOCK(iwkv, rci, rc);
  return rc;
}

iwrc iwkv_put(IWDB db, const IWKV_val *key, const IWKV_val *val, iwkv_opflags op_flags) {
  if (!db || !key || !key->size || !val) {
    return IW_ERROR_INVALID_ARGS;
  }
  if (db->iwkv->oflags & IWKV_RDONLY) {
    return IW_ERROR_READONLY;
  }
  int rci;
  iwrc rc = 0;
  IWLCTX lx = {
    .db = db,
    .key = key,
    .val = (IWKV_val *) val,
    .nlvl = -1,
    .op = IWLCTX_PUT,
    .op_flags = op_flags
  };
  API_RLOCK(db->iwkv, rci);
  rc = _lx_put_lr(&lx);
  API_UNLOCK(db->iwkv, rci, rc);
  return rc;
}
