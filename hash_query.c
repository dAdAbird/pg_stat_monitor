/*-------------------------------------------------------------------------
 *
 * hash_query.c
 *		Track statement execution times across a whole database cluster.
 *
 * Portions Copyright © 2018-2020, Percona LLC and/or its affiliates
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 *
 * Portions Copyright (c) 1994, The Regents of the University of California
 *
 * IDENTIFICATION
 *	  contrib/pg_stat_monitor/hash_query.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "pg_stat_monitor.h"

static pgssSharedState *pgss;
static HTAB *pgss_hash;
static HTAB *pgss_query_hash;

static HTAB* hash_init(const char *hash_name, int key_size, int entry_size, int hash_size);

static HTAB*
hash_init(const char *hash_name, int key_size, int entry_size, int hash_size)
{
	HASHCTL info;
	memset(&info, 0, sizeof(info));
	info.keysize = key_size;
	info.entrysize = entry_size;
	return ShmemInitHash(hash_name, hash_size, hash_size, &info, HASH_ELEM | HASH_BLOBS);
}

void
pgss_startup(void)
{
	bool		found = false;
	int32		i;

	/* reset in case this is a restart within the postmaster */

	pgss = NULL;
	pgss_hash = NULL;
	pgss_query_hash = NULL;

	/*
	* Create or attach to the shared memory state, including hash table
	*/
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

	pgss = ShmemInitStruct("pg_stat_monitor", sizeof(pgssSharedState), &found);
	if (!found)
	{
		/* First time through ... */
		pgss->lock = &(GetNamedLWLockTranche("pg_stat_monitor"))->lock;
		SpinLockInit(&pgss->mutex);
		ResetSharedState(pgss);
	}

#ifdef BENCHMARK
	init_hook_stats();
#endif

	pgss->query_buf_size_bucket = MAX_QUERY_BUF / PGSM_MAX_BUCKETS;

	for (i = 0; i < PGSM_MAX_BUCKETS; i++)
	{
		unsigned char *buf = (unsigned char *)ShmemAlloc(pgss->query_buf_size_bucket);
		set_qbuf(i, buf);
		memset(buf, 0, sizeof (uint64));
	}

	pgss_hash = hash_init("pg_stat_monitor: bucket hashtable", sizeof(pgssHashKey), sizeof(pgssEntry), MAX_BUCKET_ENTRIES);
	pgss_query_hash = hash_init("pg_stat_monitor: query hashtable", sizeof(pgssQueryHashKey), sizeof(pgssQueryEntry),MAX_BUCKET_ENTRIES);

	LWLockRelease(AddinShmemInitLock);

	/*
	 * If we're in the postmaster (or a standalone backend...), set up a shmem
	 * exit hook to dump the statistics to disk.
	 */
	on_shmem_exit(pgss_shmem_shutdown, (Datum) 0);
}

pgssSharedState*
pgsm_get_ss(void)
{
	return pgss;
}

HTAB*
pgsm_get_hash(void)
{
	return pgss_hash;
}

HTAB*
pgsm_get_query_hash(void)
{
	return pgss_query_hash;
}

/*
 * shmem_shutdown hook: Dump statistics into file.
 *
 * Note: we don't bother with acquiring lock, because there should be no
 * other processes running when this is called.
 */
void
pgss_shmem_shutdown(int code, Datum arg)
{
	/* Don't try to dump during a crash. */
	if (code)
		return;

	pgss = NULL;
	/* Safety check ... shouldn't get here unless shmem is set up. */
	if (!IsHashInitialize())
		return;
}

Size
hash_memsize(void)
{
	Size	size;

	size = MAXALIGN(sizeof(pgssSharedState));
	size += MAXALIGN(MAX_QUERY_BUF);
	size = add_size(size, hash_estimate_size(MAX_BUCKET_ENTRIES, sizeof(pgssEntry)));
	size = add_size(size, hash_estimate_size(MAX_BUCKET_ENTRIES, sizeof(pgssQueryEntry)));

	return size;
}

pgssEntry *
hash_entry_alloc(pgssSharedState *pgss, pgssHashKey *key,int encoding)
{
	pgssEntry	*entry = NULL;
	bool		found = false;

	if (hash_get_num_entries(pgss_hash) >= MAX_BUCKET_ENTRIES)
	{
		elog(DEBUG1, "%s", "pg_stat_monitor: out of memory");
		return NULL;
	}
	/* Find or create an entry with desired hash code */
	entry = (pgssEntry *) hash_search(pgss_hash, key, HASH_ENTER_NULL, &found);
	if (!found)
	{
		pgss->bucket_entry[pgss->current_wbucket]++;
		/* New entry, initialize it */
		/* reset the statistics */
		memset(&entry->counters, 0, sizeof(Counters));
		/* set the appropriate initial usage count */
		/* re-initialize the mutex each time ... we assume no one using it */
		SpinLockInit(&entry->mutex);
		/* ... and don't forget the query text metadata */
		entry->encoding = encoding;
	}
	if (entry == NULL)
		elog(DEBUG1, "%s", "pg_stat_monitor: out of memory");
	return entry;
}
/*
 * Reset all the entries.
 *
 * Caller must hold an exclusive lock on pgss->lock.
 */
void
hash_query_entryies_reset()
{
	HASH_SEQ_STATUS 	hash_seq;
	pgssQueryEntry      *entry;

	hash_seq_init(&hash_seq, pgss_query_hash);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
		entry = hash_search(pgss_query_hash, &entry->key, HASH_REMOVE, NULL);
}


/*
 * Deallocate finished entries.
 *
 * Caller must hold an exclusive lock on pgss->lock.
 */
void
hash_query_entry_dealloc(int bucket, unsigned char *buf)
{
	HASH_SEQ_STATUS 	hash_seq;
	pgssQueryEntry      *entry;
	unsigned char       *old_buf;
	pgssSharedState     *pgss = pgsm_get_ss();

	old_buf = palloc0(pgss->query_buf_size_bucket);
	memcpy(old_buf, buf, pgss->query_buf_size_bucket);

	memset(buf, 0, pgss->query_buf_size_bucket);

	hash_seq_init(&hash_seq, pgss_query_hash);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		if (entry->key.bucket_id == bucket)
		{
			if (entry->state == PGSS_FINISHED || entry->state == PGSS_ERROR)
			{
				entry = hash_search(pgss_query_hash, &entry->key, HASH_REMOVE, NULL);
			}
			else
			{
				int len;
				char query_txt[1024];
				if (read_query(old_buf, entry->key.bucket_id, entry->key.queryid, query_txt) == 0)
				{
					len = read_query_buffer(entry->key.bucket_id, entry->key.queryid, query_txt);
					if (len != MAX_QUERY_BUFFER_BUCKET)
						snprintf(query_txt, 32, "%s", "<insufficient disk/shared space>");
				}
				SaveQueryText(entry->key.bucket_id, entry->key.queryid, buf, query_txt, strlen(query_txt));
			}
		}
	}
	pfree(old_buf);
}

/*
 * Deallocate least-used entries.
 *
 * Caller must hold an exclusive lock on pgss->lock.
 */
bool
hash_entry_dealloc(int bucket)
{
	HASH_SEQ_STATUS hash_seq;
	pgssEntry		*entry = NULL;

	hash_seq_init(&hash_seq, pgss_hash);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		if (bucket < 0 ||
			(entry->key.bucket_id == bucket &&
				 (entry->counters.state == PGSS_FINISHED || entry->counters.state == PGSS_ERROR)))
		{
			entry = hash_search(pgss_hash, &entry->key, HASH_REMOVE, NULL);
		}
	}
	return true;
}

/*
 * Release all entries.
 */
void
hash_entry_reset()
{
	pgssSharedState *pgss   = pgsm_get_ss();
	HASH_SEQ_STATUS		hash_seq;
	pgssEntry			*entry;

	LWLockAcquire(pgss->lock, LW_EXCLUSIVE);

	hash_seq_init(&hash_seq, pgss_hash);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		hash_search(pgss_hash, &entry->key, HASH_REMOVE, NULL);
	}
	pgss->current_wbucket = 0;
	LWLockRelease(pgss->lock);
}

/* Caller must accuire lock */
pgssQueryEntry*
hash_create_query_entry(uint64 bucket_id, uint64 queryid, uint64 dbid, uint64 userid, uint64 ip, uint64 appid)
{
    pgssQueryHashKey    key;
	pgssQueryEntry      *entry;
	bool                found;

    key.queryid = queryid;
	key.bucket_id = bucket_id;
	key.dbid = dbid;
	key.userid = userid;
	key.ip = ip;
	key.appid = appid;

	entry = (pgssQueryEntry *) hash_search(pgss_query_hash, &key, HASH_ENTER_NULL, &found);
	return entry;
}

/* Caller must accuire lock */
pgssQueryEntry*
hash_find_query_entry(uint64 bucket_id, uint64 queryid, uint64 dbid, uint64 userid, uint64 ip, uint64 appid)
{
    pgssQueryHashKey    key;
	pgssQueryEntry      *entry;
	bool                found;

    key.queryid = queryid;
	key.bucket_id = bucket_id;
	key.dbid = dbid;
	key.userid = userid;
	key.ip = ip;
	key.appid = appid;

    /* Lookup the hash table entry with shared lock. */
	entry = (pgssQueryEntry *) hash_search(pgss_query_hash, &key, HASH_FIND, &found);
    return entry;
}

bool
IsHashInitialize(void)
{
	return (pgss != NULL &&
			pgss_hash != NULL);
}

