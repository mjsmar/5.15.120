/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PAGEMAP_H
#define _LINUX_PAGEMAP_H

/*
 * Copyright 1995 Linus Torvalds
 */
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/list.h>
#include <linux/highmem.h>
#include <linux/compiler.h>
#include <linux/uaccess.h>
#include <linux/gfp.h>
#include <linux/bitops.h>
#include <linux/hardirq.h> /* for in_interrupt() */
#include <linux/hugetlb_inline.h>

struct pagevec;

static inline bool mapping_empty(struct address_space *mapping)
{
	return xa_empty(&mapping->i_pages);
}

/*
 * Bits in mapping->flags.
 */
enum mapping_flags {
	AS_EIO		= 0,	/* IO error on async write */
	AS_ENOSPC	= 1,	/* ENOSPC on async write */
	AS_MM_ALL_LOCKS	= 2,	/* under mm_take_all_locks() */
	AS_UNEVICTABLE	= 3,	/* e.g., ramdisk, SHM_LOCK */
	AS_EXITING	= 4, 	/* final truncate in progress */
	/* writeback related tags are not used */
	AS_NO_WRITEBACK_TAGS = 5,
	AS_THP_SUPPORT = 6,	/* THPs supported */
};

/**
 * mapping_set_error - record a writeback error in the address_space
 * @mapping: the mapping in which an error should be set
 * @error: the error to set in the mapping
 *
 * When writeback fails in some way, we must record that error so that
 * userspace can be informed when fsync and the like are called.  We endeavor
 * to report errors on any file that was open at the time of the error.  Some
 * internal callers also need to know when writeback errors have occurred.
 *
 * When a writeback error occurs, most filesystems will want to call
 * mapping_set_error to record the error in the mapping so that it can be
 * reported when the application calls fsync(2).
 */
static inline void mapping_set_error(struct address_space *mapping, int error)
{
	if (likely(!error))
		return;

	/* Record in wb_err for checkers using errseq_t based tracking */
	__filemap_set_wb_err(mapping, error);

	/* Record it in superblock */
	if (mapping->host)
		errseq_set(&mapping->host->i_sb->s_wb_err, error);

	/* Record it in flags for now, for legacy callers */
	if (error == -ENOSPC)
		set_bit(AS_ENOSPC, &mapping->flags);
	else
		set_bit(AS_EIO, &mapping->flags);
}

static inline void mapping_set_unevictable(struct address_space *mapping)
{
	set_bit(AS_UNEVICTABLE, &mapping->flags);
}

static inline void mapping_clear_unevictable(struct address_space *mapping)
{
	clear_bit(AS_UNEVICTABLE, &mapping->flags);
}

static inline bool mapping_unevictable(struct address_space *mapping)
{
	return mapping && test_bit(AS_UNEVICTABLE, &mapping->flags);
}

static inline void mapping_set_exiting(struct address_space *mapping)
{
	set_bit(AS_EXITING, &mapping->flags);
}

static inline int mapping_exiting(struct address_space *mapping)
{
	return test_bit(AS_EXITING, &mapping->flags);
}

static inline void mapping_set_no_writeback_tags(struct address_space *mapping)
{
	set_bit(AS_NO_WRITEBACK_TAGS, &mapping->flags);
}

static inline int mapping_use_writeback_tags(struct address_space *mapping)
{
	return !test_bit(AS_NO_WRITEBACK_TAGS, &mapping->flags);
}

static inline gfp_t mapping_gfp_mask(struct address_space * mapping)
{
	return mapping->gfp_mask;
}

/* Restricts the given gfp_mask to what the mapping allows. */
static inline gfp_t mapping_gfp_constraint(struct address_space *mapping,
		gfp_t gfp_mask)
{
	return mapping_gfp_mask(mapping) & gfp_mask;
}

/*
 * This is non-atomic.  Only to be used before the mapping is activated.
 * Probably needs a barrier...
 */
static inline void mapping_set_gfp_mask(struct address_space *m, gfp_t mask)
{
	m->gfp_mask = mask;
}

static inline bool mapping_thp_support(struct address_space *mapping)
{
	return test_bit(AS_THP_SUPPORT, &mapping->flags);
}

static inline int filemap_nr_thps(struct address_space *mapping)
{
#ifdef CONFIG_READ_ONLY_THP_FOR_FS
	return atomic_read(&mapping->nr_thps);
#else
	return 0;
#endif
}

static inline void filemap_nr_thps_inc(struct address_space *mapping)
{
#ifdef CONFIG_READ_ONLY_THP_FOR_FS
	if (!mapping_thp_support(mapping))
		atomic_inc(&mapping->nr_thps);
#else
	WARN_ON_ONCE(1);
#endif
}

static inline void filemap_nr_thps_dec(struct address_space *mapping)
{
#ifdef CONFIG_READ_ONLY_THP_FOR_FS
	if (!mapping_thp_support(mapping))
		atomic_dec(&mapping->nr_thps);
#else
	WARN_ON_ONCE(1);
#endif
}

void release_pages(struct page **pages, int nr);

struct address_space *page_mapping(struct page *);
struct address_space *folio_mapping(struct folio *);
struct address_space *swapcache_mapping(struct folio *);

/**
 * folio_file_mapping - Find the mapping this folio belongs to.
 * @folio: The folio.
 *
 * For folios which are in the page cache, return the mapping that this
 * page belongs to.  Folios in the swap cache return the mapping of the
 * swap file or swap device where the data is stored.  This is different
 * from the mapping returned by folio_mapping().  The only reason to
 * use it is if, like NFS, you return 0 from ->activate_swapfile.
 *
 * Do not call this for folios which aren't in the page cache or swap cache.
 */
static inline struct address_space *folio_file_mapping(struct folio *folio)
{
	if (unlikely(folio_test_swapcache(folio)))
		return swapcache_mapping(folio);

	return folio->mapping;
}

static inline struct address_space *page_file_mapping(struct page *page)
{
	return folio_file_mapping(page_folio(page));
}

/*
 * For file cache pages, return the address_space, otherwise return NULL
 */
static inline struct address_space *page_mapping_file(struct page *page)
{
	struct folio *folio = page_folio(page);

	if (unlikely(folio_test_swapcache(folio)))
		return NULL;
	return folio_mapping(folio);
}

static inline bool page_cache_add_speculative(struct page *page, int count)
{
	VM_BUG_ON_PAGE(PageTail(page), page);
	return folio_ref_try_add_rcu((struct folio *)page, count);
}

static inline bool page_cache_get_speculative(struct page *page)
{
	return page_cache_add_speculative(page, 1);
}

/**
 * folio_attach_private - Attach private data to a folio.
 * @folio: Folio to attach data to.
 * @data: Data to attach to folio.
 *
 * Attaching private data to a folio increments the page's reference count.
 * The data must be detached before the folio will be freed.
 */
static inline void folio_attach_private(struct folio *folio, void *data)
{
	folio_get(folio);
	folio->private = data;
	folio_set_private(folio);
}

/**
 * folio_detach_private - Detach private data from a folio.
 * @folio: Folio to detach data from.
 *
 * Removes the data that was previously attached to the folio and decrements
 * the refcount on the page.
 *
 * Return: Data that was attached to the folio.
 */
static inline void *folio_detach_private(struct folio *folio)
{
	void *data = folio_get_private(folio);

	if (!folio_test_private(folio))
		return NULL;
	folio_clear_private(folio);
	folio->private = NULL;
	folio_put(folio);

	return data;
}

static inline void attach_page_private(struct page *page, void *data)
{
	folio_attach_private(page_folio(page), data);
}

static inline void *detach_page_private(struct page *page)
{
	return folio_detach_private(page_folio(page));
}

#ifdef CONFIG_NUMA
extern struct page *__page_cache_alloc(gfp_t gfp);
#else
static inline struct page *__page_cache_alloc(gfp_t gfp)
{
	return alloc_pages(gfp, 0);
}
#endif

static inline struct page *page_cache_alloc(struct address_space *x)
{
	return __page_cache_alloc(mapping_gfp_mask(x));
}

static inline gfp_t readahead_gfp_mask(struct address_space *x)
{
	return mapping_gfp_mask(x) | __GFP_NORETRY | __GFP_NOWARN;
}

typedef int filler_t(void *, struct page *);

pgoff_t page_cache_next_miss(struct address_space *mapping,
			     pgoff_t index, unsigned long max_scan);
pgoff_t page_cache_prev_miss(struct address_space *mapping,
			     pgoff_t index, unsigned long max_scan);

#define FGP_ACCESSED		0x00000001
#define FGP_LOCK		0x00000002
#define FGP_CREAT		0x00000004
#define FGP_WRITE		0x00000008
#define FGP_NOFS		0x00000010
#define FGP_NOWAIT		0x00000020
#define FGP_FOR_MMAP		0x00000040
#define FGP_HEAD		0x00000080
#define FGP_ENTRY		0x00000100

struct page *pagecache_get_page(struct address_space *mapping, pgoff_t offset,
		int fgp_flags, gfp_t cache_gfp_mask);

/**
 * find_get_page - find and get a page reference
 * @mapping: the address_space to search
 * @offset: the page index
 *
 * Looks up the page cache slot at @mapping & @offset.  If there is a
 * page cache page, it is returned with an increased refcount.
 *
 * Otherwise, %NULL is returned.
 */
static inline struct page *find_get_page(struct address_space *mapping,
					pgoff_t offset)
{
	return pagecache_get_page(mapping, offset, 0, 0);
}

static inline struct page *find_get_page_flags(struct address_space *mapping,
					pgoff_t offset, int fgp_flags)
{
	return pagecache_get_page(mapping, offset, fgp_flags, 0);
}

/**
 * find_lock_page - locate, pin and lock a pagecache page
 * @mapping: the address_space to search
 * @index: the page index
 *
 * Looks up the page cache entry at @mapping & @index.  If there is a
 * page cache page, it is returned locked and with an increased
 * refcount.
 *
 * Context: May sleep.
 * Return: A struct page or %NULL if there is no page in the cache for this
 * index.
 */
static inline struct page *find_lock_page(struct address_space *mapping,
					pgoff_t index)
{
	return pagecache_get_page(mapping, index, FGP_LOCK, 0);
}

/**
 * find_lock_head - Locate, pin and lock a pagecache page.
 * @mapping: The address_space to search.
 * @index: The page index.
 *
 * Looks up the page cache entry at @mapping & @index.  If there is a
 * page cache page, its head page is returned locked and with an increased
 * refcount.
 *
 * Context: May sleep.
 * Return: A struct page which is !PageTail, or %NULL if there is no page
 * in the cache for this index.
 */
static inline struct page *find_lock_head(struct address_space *mapping,
					pgoff_t index)
{
	return pagecache_get_page(mapping, index, FGP_LOCK | FGP_HEAD, 0);
}

/**
 * find_or_create_page - locate or add a pagecache page
 * @mapping: the page's address_space
 * @index: the page's index into the mapping
 * @gfp_mask: page allocation mode
 *
 * Looks up the page cache slot at @mapping & @offset.  If there is a
 * page cache page, it is returned locked and with an increased
 * refcount.
 *
 * If the page is not present, a new page is allocated using @gfp_mask
 * and added to the page cache and the VM's LRU list.  The page is
 * returned locked and with an increased refcount.
 *
 * On memory exhaustion, %NULL is returned.
 *
 * find_or_create_page() may sleep, even if @gfp_flags specifies an
 * atomic allocation!
 */
static inline struct page *find_or_create_page(struct address_space *mapping,
					pgoff_t index, gfp_t gfp_mask)
{
	return pagecache_get_page(mapping, index,
					FGP_LOCK|FGP_ACCESSED|FGP_CREAT,
					gfp_mask);
}

/**
 * grab_cache_page_nowait - returns locked page at given index in given cache
 * @mapping: target address_space
 * @index: the page index
 *
 * Same as grab_cache_page(), but do not wait if the page is unavailable.
 * This is intended for speculative data generators, where the data can
 * be regenerated if the page couldn't be grabbed.  This routine should
 * be safe to call while holding the lock for another page.
 *
 * Clear __GFP_FS when allocating the page to avoid recursion into the fs
 * and deadlock against the caller's locked page.
 */
static inline struct page *grab_cache_page_nowait(struct address_space *mapping,
				pgoff_t index)
{
	return pagecache_get_page(mapping, index,
			FGP_LOCK|FGP_CREAT|FGP_NOFS|FGP_NOWAIT,
			mapping_gfp_mask(mapping));
}

/* Does this page contain this index? */
static inline bool thp_contains(struct page *head, pgoff_t index)
{
	/* HugeTLBfs indexes the page cache in units of hpage_size */
	if (PageHuge(head))
		return head->index == index;
	return page_index(head) == (index & ~(thp_nr_pages(head) - 1UL));
}

#define swapcache_index(folio)	__page_file_index(&(folio)->page)

/**
 * folio_index - File index of a folio.
 * @folio: The folio.
 *
 * For a folio which is either in the page cache or the swap cache,
 * return its index within the address_space it belongs to.  If you know
 * the page is definitely in the page cache, you can look at the folio's
 * index directly.
 *
 * Return: The index (offset in units of pages) of a folio in its file.
 */
static inline pgoff_t folio_index(struct folio *folio)
{
        if (unlikely(folio_test_swapcache(folio)))
                return swapcache_index(folio);
        return folio->index;
}

/**
 * folio_next_index - Get the index of the next folio.
 * @folio: The current folio.
 *
 * Return: The index of the folio which follows this folio in the file.
 */
static inline pgoff_t folio_next_index(struct folio *folio)
{
	return folio->index + folio_nr_pages(folio);
}

/**
 * folio_file_page - The page for a particular index.
 * @folio: The folio which contains this index.
 * @index: The index we want to look up.
 *
 * Sometimes after looking up a folio in the page cache, we need to
 * obtain the specific page for an index (eg a page fault).
 *
 * Return: The page containing the file data for this index.
 */
static inline struct page *folio_file_page(struct folio *folio, pgoff_t index)
{
	/* HugeTLBfs indexes the page cache in units of hpage_size */
	if (folio_test_hugetlb(folio))
		return &folio->page;
	return folio_page(folio, index & (folio_nr_pages(folio) - 1));
}

/**
 * folio_contains - Does this folio contain this index?
 * @folio: The folio.
 * @index: The page index within the file.
 *
 * Context: The caller should have the page locked in order to prevent
 * (eg) shmem from moving the page between the page cache and swap cache
 * and changing its index in the middle of the operation.
 * Return: true or false.
 */
static inline bool folio_contains(struct folio *folio, pgoff_t index)
{
	/* HugeTLBfs indexes the page cache in units of hpage_size */
	if (folio_test_hugetlb(folio))
		return folio->index == index;
	return index - folio_index(folio) < folio_nr_pages(folio);
}

/*
 * Given the page we found in the page cache, return the page corresponding
 * to this index in the file
 */
static inline struct page *find_subpage(struct page *head, pgoff_t index)
{
	/* HugeTLBfs wants the head page regardless */
	if (PageHuge(head))
		return head;

	return head + (index & (thp_nr_pages(head) - 1));
}

unsigned find_get_entries(struct address_space *mapping, pgoff_t start,
		pgoff_t end, struct pagevec *pvec, pgoff_t *indices);
unsigned find_get_pages_range(struct address_space *mapping, pgoff_t *start,
			pgoff_t end, unsigned int nr_pages,
			struct page **pages);
static inline unsigned find_get_pages(struct address_space *mapping,
			pgoff_t *start, unsigned int nr_pages,
			struct page **pages)
{
	return find_get_pages_range(mapping, start, (pgoff_t)-1, nr_pages,
				    pages);
}
unsigned find_get_pages_contig(struct address_space *mapping, pgoff_t start,
			       unsigned int nr_pages, struct page **pages);
unsigned find_get_pages_range_tag(struct address_space *mapping, pgoff_t *index,
			pgoff_t end, xa_mark_t tag, unsigned int nr_pages,
			struct page **pages);
static inline unsigned find_get_pages_tag(struct address_space *mapping,
			pgoff_t *index, xa_mark_t tag, unsigned int nr_pages,
			struct page **pages)
{
	return find_get_pages_range_tag(mapping, index, (pgoff_t)-1, tag,
					nr_pages, pages);
}

struct page *grab_cache_page_write_begin(struct address_space *mapping,
			pgoff_t index, unsigned flags);

/*
 * Returns locked page at given index in given cache, creating it if needed.
 */
static inline struct page *grab_cache_page(struct address_space *mapping,
								pgoff_t index)
{
	return find_or_create_page(mapping, index, mapping_gfp_mask(mapping));
}

extern struct page * read_cache_page(struct address_space *mapping,
				pgoff_t index, filler_t *filler, void *data);
extern struct page * read_cache_page_gfp(struct address_space *mapping,
				pgoff_t index, gfp_t gfp_mask);
extern int read_cache_pages(struct address_space *mapping,
		struct list_head *pages, filler_t *filler, void *data);

static inline struct page *read_mapping_page(struct address_space *mapping,
				pgoff_t index, void *data)
{
	return read_cache_page(mapping, index, NULL, data);
}

/*
 * Get index of the page within radix-tree (but not for hugetlb pages).
 * (TODO: remove once hugetlb pages will have ->index in PAGE_SIZE)
 */
static inline pgoff_t page_to_index(struct page *page)
{
	struct page *head;

	if (likely(!PageTransTail(page)))
		return page->index;

	head = compound_head(page);
	/*
	 *  We don't initialize ->index for tail pages: calculate based on
	 *  head page
	 */
	return head->index + page - head;
}

extern pgoff_t hugetlb_basepage_index(struct page *page);

/*
 * Get the offset in PAGE_SIZE (even for hugetlb pages).
 * (TODO: hugetlb pages should have ->index in PAGE_SIZE)
 */
static inline pgoff_t page_to_pgoff(struct page *page)
{
	if (unlikely(PageHuge(page)))
		return hugetlb_basepage_index(page);
	return page_to_index(page);
}

/*
 * Return byte-offset into filesystem object for page.
 */
static inline loff_t page_offset(struct page *page)
{
	return ((loff_t)page->index) << PAGE_SHIFT;
}

static inline loff_t page_file_offset(struct page *page)
{
	return ((loff_t)page_index(page)) << PAGE_SHIFT;
}

/**
 * folio_pos - Returns the byte position of this folio in its file.
 * @folio: The folio.
 */
static inline loff_t folio_pos(struct folio *folio)
{
	return page_offset(&folio->page);
}

/**
 * folio_file_pos - Returns the byte position of this folio in its file.
 * @folio: The folio.
 *
 * This differs from folio_pos() for folios which belong to a swap file.
 * NFS is the only filesystem today which needs to use folio_file_pos().
 */
static inline loff_t folio_file_pos(struct folio *folio)
{
	return page_file_offset(&folio->page);
}

extern pgoff_t linear_hugepage_index(struct vm_area_struct *vma,
				     unsigned long address);

static inline pgoff_t linear_page_index(struct vm_area_struct *vma,
					unsigned long address)
{
	pgoff_t pgoff;
	if (unlikely(is_vm_hugetlb_page(vma)))
		return linear_hugepage_index(vma, address);
	pgoff = (address - vma->vm_start) >> PAGE_SHIFT;
	pgoff += vma->vm_pgoff;
	return pgoff;
}

struct wait_page_key {
	struct page *page;
	int bit_nr;
	int page_match;
};

struct wait_page_queue {
	struct page *page;
	int bit_nr;
	wait_queue_entry_t wait;
};

static inline bool wake_page_match(struct wait_page_queue *wait_page,
				  struct wait_page_key *key)
{
	if (wait_page->page != key->page)
	       return false;
	key->page_match = 1;

	if (wait_page->bit_nr != key->bit_nr)
		return false;

	return true;
}

void __folio_lock(struct folio *folio);
int __folio_lock_killable(struct folio *folio);
extern int __lock_page_or_retry(struct page *page, struct mm_struct *mm,
				unsigned int flags);
void unlock_page(struct page *page);
void folio_unlock(struct folio *folio);

static inline bool folio_trylock(struct folio *folio)
{
	return likely(!test_and_set_bit_lock(PG_locked, folio_flags(folio, 0)));
}

/*
 * Return true if the page was successfully locked
 */
static inline int trylock_page(struct page *page)
{
	return folio_trylock(page_folio(page));
}

static inline void folio_lock(struct folio *folio)
{
	might_sleep();
	if (!folio_trylock(folio))
		__folio_lock(folio);
}

/*
 * lock_page may only be called if we have the page's inode pinned.
 */
static inline void lock_page(struct page *page)
{
	struct folio *folio;
	might_sleep();

	folio = page_folio(page);
	if (!folio_trylock(folio))
		__folio_lock(folio);
}

static inline int folio_lock_killable(struct folio *folio)
{
	might_sleep();
	if (!folio_trylock(folio))
		return __folio_lock_killable(folio);
	return 0;
}

/*
 * lock_page_killable is like lock_page but can be interrupted by fatal
 * signals.  It returns 0 if it locked the page and -EINTR if it was
 * killed while waiting.
 */
static inline int lock_page_killable(struct page *page)
{
	return folio_lock_killable(page_folio(page));
}

/*
 * lock_page_or_retry - Lock the page, unless this would block and the
 * caller indicated that it can handle a retry.
 *
 * Return value and mmap_lock implications depend on flags; see
 * __lock_page_or_retry().
 */
static inline int lock_page_or_retry(struct page *page, struct mm_struct *mm,
				     unsigned int flags)
{
	might_sleep();
	return trylock_page(page) || __lock_page_or_retry(page, mm, flags);
}

/*
 * This is exported only for wait_on_page_locked/wait_on_page_writeback, etc.,
 * and should not be used directly.
 */
extern void wait_on_page_bit(struct page *page, int bit_nr);
extern int wait_on_page_bit_killable(struct page *page, int bit_nr);

/* 
 * Wait for a page to be unlocked.
 *
 * This must be called with the caller "holding" the page,
 * ie with increased "page->count" so that the page won't
 * go away during the wait..
 */
static inline void wait_on_page_locked(struct page *page)
{
	if (PageLocked(page))
		wait_on_page_bit(compound_head(page), PG_locked);
}

static inline int wait_on_page_locked_killable(struct page *page)
{
	if (!PageLocked(page))
		return 0;
	return wait_on_page_bit_killable(compound_head(page), PG_locked);
}

int put_and_wait_on_page_locked(struct page *page, int state);
void wait_on_page_writeback(struct page *page);
int wait_on_page_writeback_killable(struct page *page);
extern void end_page_writeback(struct page *page);
void wait_for_stable_page(struct page *page);

void __set_page_dirty(struct page *, struct address_space *, int warn);
int __set_page_dirty_nobuffers(struct page *page);
int __set_page_dirty_no_writeback(struct page *page);

void page_endio(struct page *page, bool is_write, int err);

/**
 * set_page_private_2 - Set PG_private_2 on a page and take a ref
 * @page: The page.
 *
 * Set the PG_private_2 flag on a page and take the reference needed for the VM
 * to handle its lifetime correctly.  This sets the flag and takes the
 * reference unconditionally, so care must be taken not to set the flag again
 * if it's already set.
 */
static inline void set_page_private_2(struct page *page)
{
	page = compound_head(page);
	get_page(page);
	SetPagePrivate2(page);
}

void end_page_private_2(struct page *page);
void wait_on_page_private_2(struct page *page);
int wait_on_page_private_2_killable(struct page *page);

/*
 * Add an arbitrary waiter to a page's wait queue
 */
extern void add_page_wait_queue(struct page *page, wait_queue_entry_t *waiter);

/*
 * Fault in userspace address range.
 */
size_t fault_in_writeable(char __user *uaddr, size_t size);
size_t fault_in_safe_writeable(const char __user *uaddr, size_t size);
size_t fault_in_readable(const char __user *uaddr, size_t size);

int add_to_page_cache_locked(struct page *page, struct address_space *mapping,
				pgoff_t index, gfp_t gfp_mask);
int add_to_page_cache_lru(struct page *page, struct address_space *mapping,
				pgoff_t index, gfp_t gfp_mask);
extern void delete_from_page_cache(struct page *page);
extern void __delete_from_page_cache(struct page *page, void *shadow);
void replace_page_cache_page(struct page *old, struct page *new);
void delete_from_page_cache_batch(struct address_space *mapping,
				  struct pagevec *pvec);
loff_t mapping_seek_hole_data(struct address_space *, loff_t start, loff_t end,
		int whence);

/*
 * Like add_to_page_cache_locked, but used to add newly allocated pages:
 * the page is new, so we can just run __SetPageLocked() against it.
 */
static inline int add_to_page_cache(struct page *page,
		struct address_space *mapping, pgoff_t offset, gfp_t gfp_mask)
{
	int error;

	__SetPageLocked(page);
	error = add_to_page_cache_locked(page, mapping, offset, gfp_mask);
	if (unlikely(error))
		__ClearPageLocked(page);
	return error;
}

/**
 * struct readahead_control - Describes a readahead request.
 *
 * A readahead request is for consecutive pages.  Filesystems which
 * implement the ->readahead method should call readahead_page() or
 * readahead_page_batch() in a loop and attempt to start I/O against
 * each page in the request.
 *
 * Most of the fields in this struct are private and should be accessed
 * by the functions below.
 *
 * @file: The file, used primarily by network filesystems for authentication.
 *	  May be NULL if invoked internally by the filesystem.
 * @mapping: Readahead this filesystem object.
 * @ra: File readahead state.  May be NULL.
 */
struct readahead_control {
	struct file *file;
	struct address_space *mapping;
	struct file_ra_state *ra;
/* private: use the readahead_* accessors instead */
	pgoff_t _index;
	unsigned int _nr_pages;
	unsigned int _batch_count;
};

#define DEFINE_READAHEAD(ractl, f, r, m, i)				\
	struct readahead_control ractl = {				\
		.file = f,						\
		.mapping = m,						\
		.ra = r,						\
		._index = i,						\
	}

#define VM_READAHEAD_PAGES	(SZ_128K / PAGE_SIZE)

void page_cache_ra_unbounded(struct readahead_control *,
		unsigned long nr_to_read, unsigned long lookahead_count);
void page_cache_sync_ra(struct readahead_control *, unsigned long req_count);
void page_cache_async_ra(struct readahead_control *, struct page *,
		unsigned long req_count);
void readahead_expand(struct readahead_control *ractl,
		      loff_t new_start, size_t new_len);

/**
 * page_cache_sync_readahead - generic file readahead
 * @mapping: address_space which holds the pagecache and I/O vectors
 * @ra: file_ra_state which holds the readahead state
 * @file: Used by the filesystem for authentication.
 * @index: Index of first page to be read.
 * @req_count: Total number of pages being read by the caller.
 *
 * page_cache_sync_readahead() should be called when a cache miss happened:
 * it will submit the read.  The readahead logic may decide to piggyback more
 * pages onto the read request if access patterns suggest it will improve
 * performance.
 */
static inline
void page_cache_sync_readahead(struct address_space *mapping,
		struct file_ra_state *ra, struct file *file, pgoff_t index,
		unsigned long req_count)
{
	DEFINE_READAHEAD(ractl, file, ra, mapping, index);
	page_cache_sync_ra(&ractl, req_count);
}

/**
 * page_cache_async_readahead - file readahead for marked pages
 * @mapping: address_space which holds the pagecache and I/O vectors
 * @ra: file_ra_state which holds the readahead state
 * @file: Used by the filesystem for authentication.
 * @page: The page at @index which triggered the readahead call.
 * @index: Index of first page to be read.
 * @req_count: Total number of pages being read by the caller.
 *
 * page_cache_async_readahead() should be called when a page is used which
 * is marked as PageReadahead; this is a marker to suggest that the application
 * has used up enough of the readahead window that we should start pulling in
 * more pages.
 */
static inline
void page_cache_async_readahead(struct address_space *mapping,
		struct file_ra_state *ra, struct file *file,
		struct page *page, pgoff_t index, unsigned long req_count)
{
	DEFINE_READAHEAD(ractl, file, ra, mapping, index);
	page_cache_async_ra(&ractl, page, req_count);
}

/**
 * readahead_page - Get the next page to read.
 * @rac: The current readahead request.
 *
 * Context: The page is locked and has an elevated refcount.  The caller
 * should decreases the refcount once the page has been submitted for I/O
 * and unlock the page once all I/O to that page has completed.
 * Return: A pointer to the next page, or %NULL if we are done.
 */
static inline struct page *readahead_page(struct readahead_control *rac)
{
	struct page *page;

	BUG_ON(rac->_batch_count > rac->_nr_pages);
	rac->_nr_pages -= rac->_batch_count;
	rac->_index += rac->_batch_count;

	if (!rac->_nr_pages) {
		rac->_batch_count = 0;
		return NULL;
	}

	page = xa_load(&rac->mapping->i_pages, rac->_index);
	VM_BUG_ON_PAGE(!PageLocked(page), page);
	rac->_batch_count = thp_nr_pages(page);

	return page;
}

static inline unsigned int __readahead_batch(struct readahead_control *rac,
		struct page **array, unsigned int array_sz)
{
	unsigned int i = 0;
	XA_STATE(xas, &rac->mapping->i_pages, 0);
	struct page *page;

	BUG_ON(rac->_batch_count > rac->_nr_pages);
	rac->_nr_pages -= rac->_batch_count;
	rac->_index += rac->_batch_count;
	rac->_batch_count = 0;

	xas_set(&xas, rac->_index);
	rcu_read_lock();
	xas_for_each(&xas, page, rac->_index + rac->_nr_pages - 1) {
		if (xas_retry(&xas, page))
			continue;
		VM_BUG_ON_PAGE(!PageLocked(page), page);
		VM_BUG_ON_PAGE(PageTail(page), page);
		array[i++] = page;
		rac->_batch_count += thp_nr_pages(page);

		/*
		 * The page cache isn't using multi-index entries yet,
		 * so the xas cursor needs to be manually moved to the
		 * next index.  This can be removed once the page cache
		 * is converted.
		 */
		if (PageHead(page))
			xas_set(&xas, rac->_index + rac->_batch_count);

		if (i == array_sz)
			break;
	}
	rcu_read_unlock();

	return i;
}

/**
 * readahead_page_batch - Get a batch of pages to read.
 * @rac: The current readahead request.
 * @array: An array of pointers to struct page.
 *
 * Context: The pages are locked and have an elevated refcount.  The caller
 * should decreases the refcount once the page has been submitted for I/O
 * and unlock the page once all I/O to that page has completed.
 * Return: The number of pages placed in the array.  0 indicates the request
 * is complete.
 */
#define readahead_page_batch(rac, array)				\
	__readahead_batch(rac, array, ARRAY_SIZE(array))

/**
 * readahead_pos - The byte offset into the file of this readahead request.
 * @rac: The readahead request.
 */
static inline loff_t readahead_pos(struct readahead_control *rac)
{
	return (loff_t)rac->_index * PAGE_SIZE;
}

/**
 * readahead_length - The number of bytes in this readahead request.
 * @rac: The readahead request.
 */
static inline size_t readahead_length(struct readahead_control *rac)
{
	return rac->_nr_pages * PAGE_SIZE;
}

/**
 * readahead_index - The index of the first page in this readahead request.
 * @rac: The readahead request.
 */
static inline pgoff_t readahead_index(struct readahead_control *rac)
{
	return rac->_index;
}

/**
 * readahead_count - The number of pages in this readahead request.
 * @rac: The readahead request.
 */
static inline unsigned int readahead_count(struct readahead_control *rac)
{
	return rac->_nr_pages;
}

/**
 * readahead_batch_length - The number of bytes in the current batch.
 * @rac: The readahead request.
 */
static inline size_t readahead_batch_length(struct readahead_control *rac)
{
	return rac->_batch_count * PAGE_SIZE;
}

static inline unsigned long dir_pages(struct inode *inode)
{
	return (unsigned long)(inode->i_size + PAGE_SIZE - 1) >>
			       PAGE_SHIFT;
}

/**
 * page_mkwrite_check_truncate - check if page was truncated
 * @page: the page to check
 * @inode: the inode to check the page against
 *
 * Returns the number of bytes in the page up to EOF,
 * or -EFAULT if the page was truncated.
 */
static inline int page_mkwrite_check_truncate(struct page *page,
					      struct inode *inode)
{
	loff_t size = i_size_read(inode);
	pgoff_t index = size >> PAGE_SHIFT;
	int offset = offset_in_page(size);

	if (page->mapping != inode->i_mapping)
		return -EFAULT;

	/* page is wholly inside EOF */
	if (page->index < index)
		return PAGE_SIZE;
	/* page is wholly past EOF */
	if (page->index > index || !offset)
		return -EFAULT;
	/* page is partially inside EOF */
	return offset;
}

/**
 * i_blocks_per_page - How many blocks fit in this page.
 * @inode: The inode which contains the blocks.
 * @page: The page (head page if the page is a THP).
 *
 * If the block size is larger than the size of this page, return zero.
 *
 * Context: The caller should hold a refcount on the page to prevent it
 * from being split.
 * Return: The number of filesystem blocks covered by this page.
 */
static inline
unsigned int i_blocks_per_page(struct inode *inode, struct page *page)
{
	return thp_size(page) >> inode->i_blkbits;
}
#endif /* _LINUX_PAGEMAP_H */
