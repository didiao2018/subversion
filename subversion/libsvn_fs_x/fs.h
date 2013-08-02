/* fs.h : interface to Subversion filesystem, private to libsvn_fs
 *
 * ====================================================================
 *    Licensed to the Apache Software Foundation (ASF) under one
 *    or more contributor license agreements.  See the NOTICE file
 *    distributed with this work for additional information
 *    regarding copyright ownership.  The ASF licenses this file
 *    to you under the Apache License, Version 2.0 (the
 *    "License"); you may not use this file except in compliance
 *    with the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing,
 *    software distributed under the License is distributed on an
 *    "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 *    KIND, either express or implied.  See the License for the
 *    specific language governing permissions and limitations
 *    under the License.
 * ====================================================================
 */

#ifndef SVN_LIBSVN_FS_X_H
#define SVN_LIBSVN_FS_X_H

#include <apr_pools.h>
#include <apr_hash.h>
#include <apr_network_io.h>
#include <apr_md5.h>
#include <apr_sha1.h>

#include "svn_fs.h"
#include "svn_config.h"
#include "private/svn_atomic.h"
#include "private/svn_cache.h"
#include "private/svn_fs_private.h"
#include "private/svn_sqlite.h"
#include "private/svn_mutex.h"
#include "private/svn_named_atomic.h"

#include "id.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */


/*** The filesystem structure.  ***/

/* Following are defines that specify the textual elements of the
   native filesystem directories and revision files. */

/* Names of special files in the fs_x filesystem. */
#define PATH_FORMAT           "format"           /* Contains format number */
#define PATH_UUID             "uuid"             /* Contains UUID */
#define PATH_CURRENT          "current"          /* Youngest revision */
#define PATH_LOCK_FILE        "write-lock"       /* Revision lock file */
#define PATH_REVS_DIR         "revs"             /* Directory of revisions */
#define PATH_REVPROPS_DIR     "revprops"         /* Directory of revprops */
#define PATH_TXNS_DIR         "transactions"     /* Directory of transactions */
#define PATH_NODE_ORIGINS_DIR "node-origins"     /* Lazy node-origin cache */
#define PATH_TXN_PROTOS_DIR   "txn-protorevs"    /* Directory of proto-revs */
#define PATH_TXN_CURRENT      "txn-current"      /* File with next txn key */
#define PATH_TXN_CURRENT_LOCK "txn-current-lock" /* Lock for txn-current */
#define PATH_LOCKS_DIR        "locks"            /* Directory of locks */
#define PATH_MIN_UNPACKED_REV "min-unpacked-rev" /* Oldest revision which
                                                    has not been packed. */
#define PATH_REVPROP_GENERATION "revprop-generation"
                                                 /* Current revprop generation*/
#define PATH_MANIFEST         "manifest"         /* Manifest file name */
#define PATH_PACKED           "pack"             /* Packed revision data file */
#define PATH_EXT_PACKED_SHARD ".pack"            /* Extension for packed
                                                    shards */
#define PATH_EXT_L2P_INDEX    ".l2p"             /* extension of the log-
                                                    to-phys index */
#define PATH_EXT_P2L_INDEX    ".p2l"             /* extension of the phys-
                                                    to-log index */
/* If you change this, look at tests/svn_test_fs.c(maybe_install_fsx_conf) */
#define PATH_CONFIG           "fsx.conf"         /* Configuration */

/* Names of special files and file extensions for transactions */
#define PATH_CHANGES       "changes"       /* Records changes made so far */
#define PATH_TXN_PROPS     "props"         /* Transaction properties */
#define PATH_NEXT_IDS      "next-ids"      /* Next temporary ID assignments */
#define PATH_PREFIX_NODE   "node."         /* Prefix for node filename */
#define PATH_EXT_TXN       ".txn"          /* Extension of txn dir */
#define PATH_EXT_CHILDREN  ".children"     /* Extension for dir contents */
#define PATH_EXT_PROPS     ".props"        /* Extension for node props */
#define PATH_EXT_REV       ".rev"          /* Extension of protorev file */
#define PATH_EXT_REV_LOCK  ".rev-lock"     /* Extension of protorev lock file */
#define PATH_TXN_ITEM_INDEX "itemidx"      /* File containing the current item
                                             index number */
#define PATH_INDEX          "index"        /* name of index files w/o ext */

/* Names of files in legacy FS formats */
#define PATH_REV           "rev"           /* Proto rev file */
#define PATH_REV_LOCK      "rev-lock"      /* Proto rev (write) lock file */

/* Names of sections and options in fsx.conf. */
#define CONFIG_SECTION_CACHES            "caches"
#define CONFIG_OPTION_FAIL_STOP          "fail-stop"
#define CONFIG_SECTION_REP_SHARING       "rep-sharing"
#define CONFIG_OPTION_ENABLE_REP_SHARING "enable-rep-sharing"
#define CONFIG_SECTION_DELTIFICATION     "deltification"
#define CONFIG_OPTION_MAX_DELTIFICATION_WALK     "max-deltification-walk"
#define CONFIG_OPTION_MAX_LINEAR_DELTIFICATION   "max-linear-deltification"
#define CONFIG_SECTION_PACKED_REVPROPS   "packed-revprops"
#define CONFIG_OPTION_REVPROP_PACK_SIZE  "revprop-pack-size"
#define CONFIG_OPTION_COMPRESS_PACKED_REVPROPS  "compress-packed-revprops"
#define CONFIG_SECTION_IO                "io"
#define CONFIG_OPTION_BLOCK_SIZE         "block-size"
#define CONFIG_OPTION_L2P_PAGE_SIZE      "l2p-page-size"
#define CONFIG_OPTION_P2L_PAGE_SIZE      "p2l-page-size"

/* The format number of this filesystem.
   This is independent of the repository format number, and
   independent of any other FS back ends. */
#define SVN_FS_X__FORMAT_NUMBER   1

/* Private FSX-specific data shared between all svn_txn_t objects that
   relate to a particular transaction in a filesystem (as identified
   by transaction id and filesystem UUID).  Objects of this type are
   allocated in their own subpool of the common pool. */
typedef struct fs_x_shared_txn_data_t
{
  /* The next transaction in the list, or NULL if there is no following
     transaction. */
  struct fs_x_shared_txn_data_t *next;

  /* ID of this transaction. */
  svn_fs_x__id_part_t txn_id;

  /* Whether the transaction's prototype revision file is locked for
     writing by any thread in this process (including the current
     thread; recursive locks are not permitted).  This is effectively
     a non-recursive mutex. */
  svn_boolean_t being_written;

  /* The pool in which this object has been allocated; a subpool of the
     common pool. */
  apr_pool_t *pool;
} fs_x_shared_txn_data_t;

/* On most operating systems apr implements file locks per process, not
   per file.  On Windows apr implements the locking as per file handle
   locks, so we don't have to add our own mutex for just in-process
   synchronization. */
/* Compare ../libsvn_subr/named_atomic.c:USE_THREAD_MUTEX */
#if APR_HAS_THREADS && !defined(WIN32)
#define SVN_FS_X__USE_LOCK_MUTEX 1
#else
#define SVN_FS_X__USE_LOCK_MUTEX 0
#endif

/* Private FSX-specific data shared between all svn_fs_t objects that
   relate to a particular filesystem, as identified by filesystem UUID.
   Objects of this type are allocated in the common pool. */
typedef struct fs_x_shared_data_t
{
  /* A list of shared transaction objects for each transaction that is
     currently active, or NULL if none are.  All access to this list,
     including the contents of the objects stored in it, is synchronised
     under TXN_LIST_LOCK. */
  fs_x_shared_txn_data_t *txns;

  /* A free transaction object, or NULL if there is no free object.
     Access to this object is synchronised under TXN_LIST_LOCK. */
  fs_x_shared_txn_data_t *free_txn;

  /* A lock for intra-process synchronization when accessing the TXNS list. */
  svn_mutex__t *txn_list_lock;

  /* A lock for intra-process synchronization when grabbing the
     repository write lock. */
  svn_mutex__t *fs_write_lock;

  /* A lock for intra-process synchronization when locking the
     txn-current file. */
  svn_mutex__t *txn_current_lock;

  /* The common pool, under which this object is allocated, subpools
     of which are used to allocate the transaction objects. */
  apr_pool_t *common_pool;
} fs_x_shared_data_t;

/* Data structure for the 1st level DAG node cache. */
typedef struct fs_x_dag_cache_t fs_x_dag_cache_t;

/* Key type for all caches that use revision + offset / counter as key.

   NOTE: always initialize this using calloc() or '= {0};'!  This is used
   as a cahe key and the padding bytes on 32 bit archs should be zero for
   cache effectiveness. */
typedef struct pair_cache_key_t
{
  svn_revnum_t revision;

  apr_uint64_t second;
} pair_cache_key_t;

/* Key type that identifies a represenation / rep header. */
typedef struct representation_cache_key_t
{
  /* Revision that contains the representation */
  apr_uint32_t revision;

  /* Packed or non-packed representation? */
  svn_boolean_t is_packed;

  /* Item index of the representation */
  apr_uint64_t item_index;
} representation_cache_key_t;

/* Key type that identifies a txdelta window. */
typedef struct window_cache_key_t
{
  /* Revision that contains the representation */
  apr_uint32_t revision;

  /* Window number within that representation */
  int chunk_index;

  /* Item index of the representation */
  apr_uint64_t item_index;
} window_cache_key_t;

/* Private (non-shared) FSX-specific data for each svn_fs_t object.
   Any caches in here may be NULL. */
typedef struct fs_x_data_t
{
  /* The format number of this FS. */
  int format;

  /* The maximum number of files to store per directory. */
  int max_files_per_dir;

  /* Rev / pack file read granularity. */
  apr_int64_t block_size;

  /* Capacity in entries of log-to-phys index pages */
  apr_int64_t l2p_page_size;

  /* Rev / pack file granularity covered by phys-to-log index pages */
  apr_int64_t p2l_page_size;
  
  /* The revision that was youngest, last time we checked. */
  svn_revnum_t youngest_rev_cache;

  /* The fsx.conf file, parsed.  Allocated in FS->pool. */
  svn_config_t *config;

  /* Caches of immutable data.  (Note that if these are created with
     svn_cache__create_memcache, the data can be shared between
     multiple svn_fs_t's for the same filesystem.) */

  /* A cache of revision root IDs, mapping from (svn_revnum_t *) to
     (svn_fs_id_t *).  (Not threadsafe.) */
  svn_cache__t *rev_root_id_cache;

  /* Caches native dag_node_t* instances and acts as a 1st level cache */
  fs_x_dag_cache_t *dag_node_cache;

  /* DAG node cache for immutable nodes.  Maps (revision, fspath)
     to (dag_node_t *). This is the 2nd level cache for DAG nodes. */
  svn_cache__t *rev_node_cache;

  /* A cache of the contents of immutable directories; maps from
     unparsed FS ID to a apr_hash_t * mapping (const char *) dirent
     names to (svn_fs_dirent_t *). */
  svn_cache__t *dir_cache;

  /* Fulltext cache; currently only used with memcached.  Maps from
     rep key (revision/offset) to svn_string_t. */
  svn_cache__t *fulltext_cache;

  /* Access object to the atomics namespace used by revprop caching.
     Will be NULL until the first access. */
  svn_atomic_namespace__t *revprop_namespace;

  /* Access object to the revprop "generation". Will be NULL until
     the first access. */
  svn_named_atomic__t *revprop_generation;

  /* Access object to the revprop update timeout. Will be NULL until
     the first access. */
  svn_named_atomic__t *revprop_timeout;

  /* Revision property cache.  Maps from (rev,generation) to apr_hash_t. */
  svn_cache__t *revprop_cache;

  /* Node properties cache.  Maps from rep key to apr_hash_t. */
  svn_cache__t *properties_cache;

  /* Pack manifest cache; a cache mapping (svn_revnum_t) shard number to
     a manifest; and a manifest is a mapping from (svn_revnum_t) revision
     number offset within a shard to (apr_off_t) byte-offset in the
     respective pack file. */
  svn_cache__t *packed_offset_cache;

  /* Cache for txdelta_window_t objects; the key is window_cache_key_t */
  svn_cache__t *txdelta_window_cache;

  /* Cache for combined windows as svn_stringbuf_t objects;
     the key is window_cache_key_t */
  svn_cache__t *combined_window_cache;

  /* Cache for svn_fs_x__rep_header_t objects;
   * the key is (revision, item index) */
  svn_cache__t *node_revision_cache;

  /* Cache for noderevs_t containers;
     the key is a (pack file revision, file offset) pair */
  svn_cache__t *noderevs_container_cache;

  /* Cache for change lists as APR arrays of change_t * objects; the key
     is the revision */
  svn_cache__t *changes_cache;

  /* Cache for change_list_t containers;
     the key is a (pack file revision, file offset) pair */
  svn_cache__t *changes_container_cache;

  /* Cache for star-delta / representation containers;
     the key is a (pack file revision, file offset) pair */
  svn_cache__t *reps_container_cache;

  /* Cache for svn_fs_x__rep_header_t objects; the key is a
     (revision, item index) pair */
  svn_cache__t *rep_header_cache;

  /* Cache for svn_mergeinfo_t objects; the key is a combination of
     revision, inheritance flags and path. */
  svn_cache__t *mergeinfo_cache;

  /* Cache for presence of svn_mergeinfo_t on a noderev; the key is a
     combination of revision, inheritance flags and path; value is "1"
     if the node has mergeinfo, "0" if it doesn't. */
  svn_cache__t *mergeinfo_existence_cache;

  /* Cache for l2p_header_t objects; the key is (revision, is-packed).
     Will be NULL for pre-format7 repos */
  svn_cache__t *l2p_header_cache;

  /* Cache for l2p_page_t objects; the key is svn_fs_x__page_cache_key_t.
     Will be NULL for pre-format7 repos */
  svn_cache__t *l2p_page_cache;

  /* Cache for p2l_header_t objects; the key is (revision, is-packed).
     Will be NULL for pre-format7 repos */
  svn_cache__t *p2l_header_cache;

  /* Cache for apr_array_header_t objects containing svn_fs_x__p2l_entry_t
     elements; the key is svn_fs_x__page_cache_key_t.
     Will be NULL for pre-format7 repos */
  svn_cache__t *p2l_page_cache;

  /* TRUE while the we hold a lock on the write lock file. */
  svn_boolean_t has_write_lock;

  /* If set, there are or have been more than one concurrent transaction */
  svn_boolean_t concurrent_transactions;

  /* Temporary cache for changed directories yet to be committed; maps from
     unparsed FS ID to ###x.  NULL outside transactions. */
  svn_cache__t *txn_dir_cache;

  /* Data shared between all svn_fs_t objects for a given filesystem. */
  fs_x_shared_data_t *shared;

  /* The sqlite database used for rep caching. */
  svn_sqlite__db_t *rep_cache_db;

  /* Thread-safe boolean */
  svn_atomic_t rep_cache_db_opened;

  /* The oldest revision not in a pack file.  It also applies to revprops
   * if revprop packing has been enabled by the FSX format version. */
  svn_revnum_t min_unpacked_rev;

  /* Whether rep-sharing is supported by the filesystem
   * and allowed by the configuration. */
  svn_boolean_t rep_sharing_allowed;

  /* File size limit in bytes up to which multiple revprops shall be packed
   * into a single file. */
  apr_int64_t revprop_pack_size;

  /* Whether packed revprop files shall be compressed. */
  svn_boolean_t compress_packed_revprops;

  /* Restart deltification histories after each multiple of this value */
  apr_int64_t max_deltification_walk;

  /* Maximum number of length of the linear part at the top of the
   * deltification history after which skip deltas will be used. */
  apr_int64_t max_linear_deltification;

  /* Pointer to svn_fs_open. */
  svn_error_t *(*svn_fs_open_)(svn_fs_t **, const char *, apr_hash_t *,
                               apr_pool_t *);
} fs_x_data_t;


/*** Filesystem Transaction ***/
typedef struct transaction_t
{
  /* property list (const char * name, svn_string_t * value).
     may be NULL if there are no properties.  */
  apr_hash_t *proplist;

  /* node revision id of the root node.  */
  const svn_fs_id_t *root_id;

  /* node revision id of the node which is the root of the revision
     upon which this txn is base.  (unfinished only) */
  const svn_fs_id_t *base_id;

  /* copies list (const char * copy_ids), or NULL if there have been
     no copies in this transaction.  */
  apr_array_header_t *copies;

} transaction_t;


/*** Representation ***/
/* If you add fields to this, check to see if you need to change
 * svn_fs_x__rep_copy. */
typedef struct representation_t
{
  /* Checksums digests for the contents produced by this representation.
     This checksum is for the contents the rep shows to consumers,
     regardless of how the rep stores the data under the hood.  It is
     independent of the storage (fulltext, delta, whatever).

     If has_sha1 is FALSE, then for compatibility behave as though this
     checksum matches the expected checksum.

     The md5 checksum is always filled, unless this is rep which was
     retrieved from the rep-cache.  The sha1 checksum is only computed on
     a write, for use with rep-sharing. */
  svn_boolean_t has_sha1;
  unsigned char sha1_digest[APR_SHA1_DIGESTSIZE];
  unsigned char md5_digest[APR_MD5_DIGESTSIZE];

  /* Revision where this representation is located. */
  svn_revnum_t revision;

  /* Item index with the the revision. */
  apr_uint64_t item_index;

  /* The size of the representation in bytes as seen in the revision
     file. */
  svn_filesize_t size;

  /* The size of the fulltext of the representation. */
  svn_filesize_t expanded_size;

  /* Is this representation a transaction? */
  svn_fs_x__id_part_t txn_id;

  /* For rep-sharing, we need a way of uniquifying node-revs which share the
     same representation (see svn_fs_x__noderev_same_rep_key() ).  So, we
     store the original txn of the node rev (not the rep!), along with some
     intra-node uniqification content. */
  struct
  {
    svn_fs_x__id_part_t txn_id;
    apr_uint64_t number;
  } uniquifier;
} representation_t;


/*** Node-Revision ***/
/* If you add fields to this, check to see if you need to change
 * copy_node_revision in dag.c. */
typedef struct node_revision_t
{
  /* node kind */
  svn_node_kind_t kind;

  /* The node-id for this node-rev. */
  const svn_fs_id_t *id;

  /* predecessor node revision id, or NULL if there is no predecessor
     for this node revision */
  const svn_fs_id_t *predecessor_id;

  /* If this node-rev is a copy, where was it copied from? */
  const char *copyfrom_path;
  svn_revnum_t copyfrom_rev;

  /* Helper for history tracing, root of the parent tree from whence
     this node-rev was copied. */
  svn_revnum_t copyroot_rev;
  const char *copyroot_path;

  /* number of predecessors this node revision has (recursively), or
     -1 if not known (for backward compatibility). */
  int predecessor_count;

  /* representation key for this node's properties.  may be NULL if
     there are no properties.  */
  representation_t *prop_rep;

  /* representation for this node's data.  may be NULL if there is
     no data. */
  representation_t *data_rep;

  /* path at which this node first came into existence.  */
  const char *created_path;

  /* is this the unmodified root of a transaction? */
  svn_boolean_t is_fresh_txn_root;

  /* Number of nodes with svn:mergeinfo properties that are
     descendants of this node (including it itself) */
  apr_int64_t mergeinfo_count;

  /* Does this node itself have svn:mergeinfo? */
  svn_boolean_t has_mergeinfo;

} node_revision_t;


/*** Change ***/
typedef struct change_t
{
  /* Path of the change. */
  svn_string_t path;

  /* API compatible change description */
  svn_fs_path_change2_t info;
} change_t;


#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* SVN_LIBSVN_FS_X_H */
