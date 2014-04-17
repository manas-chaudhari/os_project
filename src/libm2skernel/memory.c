/*
 *  Multi2Sim
 *  Copyright (C) 2007  Rafael Ubal Tena (raurte@gap.upv.es)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <m2skernel.h>
#include <sys/mman.h>

/* Total space allocated for memory pages */
unsigned long mem_mapped_space = 0;
unsigned long mem_max_mapped_space = 0;
//uint32_t DISK_POINTER_ALL=4500;
/* Safe mode */
int mem_safe_mode = 1;

struct mem_page_t *get_page_from_ptentry(struct mem_t *mem, struct ptentry *e) {
	if (e == NULL)
		return NULL;
	uint32_t ptag = (e->paddr) >> MEM_LOGPAGESIZE;
	return mem->pages[ptag];
}

struct ptentry *mem_ptentry_get(struct mem_t *mem, uint32_t addr)
{
	uint32_t vindex, vtag;
	
	vtag = addr & ~(MEM_PAGESIZE - 1);
	vindex = (addr >> MEM_LOGPAGESIZE) % MEM_PAGE_COUNT;
	
	struct ptentry *e, *prev;

	e = mem->pt->entries[vindex];
	prev = NULL;

	/* Look for entry */
	while (e && e->tag != vtag) {
		prev = e;
		e = e->next;
	}

	/* Place entry into list head */
	if (prev && e) {
		prev->next = e->next;
		e->next = mem->pt->entries[vindex];
		mem->pt->entries[vindex] = e;
	}
	
	return e;
}

/* Return mem page corresponding to an address. */
struct mem_page_t *mem_page_get(struct mem_t *mem, uint32_t addr)
{
	struct ptentry *entry = mem_ptentry_get(mem, addr);

	if (!entry) 
	{
		printf("Page not found for addr: %u\n", addr);
		return NULL;
	}
	else {
		entry->used = 1;
		if (!entry->valid_bit)
		{
			vmem_load_page(mem, entry);
		}
		return get_page_from_ptentry(mem, entry);
	}
}


/* Return the memory page following addr in the current memory map. This function
 * is useful to reconstruct consecutive ranges of mapped pages. */
struct mem_page_t *mem_page_get_next(struct mem_t *mem, uint32_t addr)
{
	uint32_t vtag, vindex, minvtag;
	struct ptentry *prev, *entry, *minentry;

	/* Get tag of the page just following addr */
	vtag = (addr + MEM_PAGESIZE) & ~(MEM_PAGESIZE - 1);
	if (!vtag)
		return NULL;
	vindex = (vtag >> MEM_LOGPAGESIZE) % MEM_PAGE_COUNT;
	
	entry = mem->pt->entries[vindex];
	prev = NULL;

	/* Look for entry exactly following addr. If it is found, return the page. */
	while (entry && entry->tag != vtag) {
		prev = entry;
		entry = entry->next;
	}
	if (entry)
		return get_page_from_ptentry(mem, entry);

	/* Page following addr is not found, so check all memory pages to find
	 * the one with the lowest tag following addr. */
	minvtag = 0xffffffff;
	minentry = NULL;
	for (vindex = 0; vindex < MEM_PAGE_COUNT; vindex++) {
		for (entry = mem->pt->entries[vindex]; entry; entry = entry->next) {
			if (entry->tag > vtag && entry->tag < minvtag) {
				minvtag = entry->tag;
				minentry = entry;
			}
		}
	}

	return get_page_from_ptentry(mem, minentry);
}

struct ptentry* mem_ptentry_create(struct mem_t* mem, uint32_t vaddr)
{
	if (vaddr == 134512640)
		printf("First page\n");
	uint32_t vindex, vtag, offset;
	vtag = vaddr & ~(MEM_PAGESIZE - 1);
	vindex = (vaddr >> MEM_LOGPAGESIZE) % MEM_PAGE_COUNT;
	
	struct ptentry * E;
	E = calloc(1, sizeof(struct ptentry));
	E->valid_bit = 0;
	E->dirtybit = 1;
	E->paddr = 0;
	E->disk_start = DISK_POINTER_ALL;
	DISK_POINTER_ALL += MEM_PAGESIZE;
	E->tag = vtag;

	vmem_load_page(mem, E);

	E->next = mem->pt->entries[vindex];
	mem->pt->entries[vindex] = E;
	return E;
}

/* Create new mem page */
static struct mem_page_t *mem_page_create(struct mem_t *mem, uint32_t addr, int perm)
{
	printf("Creating page\n");
	struct mem_page_t *page;

	struct ptentry* entry = mem_ptentry_create(mem, addr);

	uint32_t ptag = (entry->paddr) >> MEM_LOGPAGESIZE;

	page = mem->pages[ptag];
	page->perm = perm;
	entry->perm = perm;
	
	/* Insert in pages hash table */
	// page->next = mem->pages[index];
	// mem->pages[index] = page;
	mem_mapped_space += MEM_PAGESIZE;
	mem_max_mapped_space = MAX(mem_max_mapped_space, mem_mapped_space);
	return page;
}


/* Free mem pages */
static void mem_page_free(struct mem_t *mem, uint32_t addr)
{
	uint32_t vindex, vtag;
	struct ptentry *prev, *entry;
	struct mem_host_mapping_t *hm;
	
	vtag = addr & ~(MEM_PAGESIZE - 1);
	vindex = (addr >> MEM_LOGPAGESIZE) % MEM_PAGE_COUNT;
	prev = NULL;

	/* Find page */
	entry = mem->pt->entries[vindex];
	while (entry && entry->tag != vtag) {
		prev = entry;
		entry = entry->next;
	}
	if (!entry)
		return;

	/* If page belongs to a host mapping, release it if
 		* this is the last page allocated for it. */
	hm = entry->host_mapping;
	if (hm) 
	{
		assert(hm->pages > 0);
		assert(vtag >= hm->addr && vtag + MEM_PAGESIZE <= hm->addr + hm->size);
		hm->pages--;
	if (!hm->pages)
		mem_unmap_host(mem, hm->addr);
	}

	/* Free page */
	if (prev)
		prev->next = entry->next;
	else
		mem->pt->entries[vindex] = entry->next;
	mem_mapped_space -= MEM_PAGESIZE;

	if (entry->valid_bit) {
		struct mem_page_t *page = get_page_from_ptentry(mem, entry);
		mem->free_frames[mem->free_frames_size] = entry->paddr;
		mem->free_frames_size++;
		if (page->data)
			free(page->data);
	}
	free(entry);
}


/* Copy memory pages. All parameters must be multiple of the page size.
 * The pages in the source and destination interval must exist. */
void mem_copy(struct mem_t *mem, uint32_t dest, uint32_t src, int size)
{
	struct mem_page_t *page_dest, *page_src;

	/* Restrictions. No overlapping allowed. */
	assert(!(dest & (MEM_PAGESIZE-1)));
	assert(!(src & (MEM_PAGESIZE-1)));
	assert(!(size & (MEM_PAGESIZE-1)));
	if ((src < dest && src + size > dest) ||
		(dest < src && dest + size > src))
		fatal("mem_copy: cannot copy overlapping regions");
	
	/* Copy */
	while (size > 0) {
		
		/* Get source and destination pages */
		page_dest = mem_page_get(mem, dest);
		page_src = mem_page_get(mem, src);
		assert(page_src && page_dest);
		
		/* Different actions depending on whether source and
		 * destination page data are allocated. */
		if (page_src->data) {
			if (!page_dest->data)
				page_dest->data = malloc(MEM_PAGESIZE);
			memcpy(page_dest->data, page_src->data, MEM_PAGESIZE);
		} else {
			if (page_dest->data)
				memset(page_dest->data, 0, MEM_PAGESIZE);
		}
		struct ptentry *entry = mem_ptentry_get(mem, dest);
		entry->dirtybit = 1;

		/* Advance pointers */
		src += MEM_PAGESIZE;
		dest += MEM_PAGESIZE;
		size -= MEM_PAGESIZE;
	}
}


/* Return the buffer corresponding to address 'addr' in the simulated
 * mem. The returned buffer is null if addr+size exceeds the page
 * boundaries. */
void *mem_get_buffer(struct mem_t *mem, uint32_t addr, int size,
	enum mem_access_enum access)
{
	struct mem_page_t *page;
	uint32_t offset;

	/* Get page offset and check page bounds */
	offset = addr & (MEM_PAGESIZE - 1);
	if (offset + size > MEM_PAGESIZE)
		return NULL;
	
	/* Look for page */
	page = mem_page_get(mem, addr);
	if (!page)
		return NULL;
	
	/* Check page permissions */
	// if ((page->perm & access) != access && mem->safe) {
	// 	printf("Permission: %u\n", page->perm);
	// 	fatal("mem_get_buffer: permission denied at 0x%x", addr);
	// }
	
	/* Allocate and initialize page data if it does not exist yet. */
	if (!page->data)
		page->data = calloc(1, MEM_PAGESIZE);
	
	if (access == mem_access_write) {
		struct ptentry *entry = mem_ptentry_get(mem, addr);
		entry->dirtybit = 1;
	}

	/* Return pointer to page data */
	return page->data + offset;
}


/* Access memory without exceeding page boundaries. */
static void mem_access_page_boundary(struct mem_t *mem, uint32_t addr,
	int size, void *buf, enum mem_access_enum access)
{
	struct mem_page_t *page;
	uint32_t offset;

	/* Find memory page and compute offset. */
	page = mem_page_get(mem, addr);
	offset = addr & (MEM_PAGESIZE - 1);
	assert(offset + size <= MEM_PAGESIZE);

	/* On nonexistent page, raise segmentation fault in safe mode,
	 * or create page with full privileges for writes in unsafe mode. */
	if (!page) {
		// if (mem->safe)
		// 	fatal("illegal access at 0x%x: page not allocated", addr);
		if (access == mem_access_read || access == mem_access_exec) {
			memset(buf, 0, size);
			return;
		}
		if (access == mem_access_write || access == mem_access_init)
			page = mem_page_create(mem, addr, mem_access_read |
				mem_access_write | mem_access_exec |
				mem_access_init);
	}
	assert(page);

	/* If it is a write access, set the 'modified' flag in the page
	 * attributes (perm). This is not done for 'initialize' access. */
	if (access == mem_access_write) {
		page->perm |= mem_access_modif;
		// entry->perm |= mem_access_modif;
	}

	/* Check permissions in safe mode */
	if (mem->safe && (page->perm & access) != access){
		//fatal("mem_access: permission denied at 0x%x", addr);
            raise(SIGSEGV);
        }

	/* Read/execute access */
	if (access == mem_access_read || access == mem_access_exec) {
		if (page->data)
			memcpy(buf, page->data + offset, size);
		else
			memset(buf, 0, size);
		return;
	}

	/* Write/initialize access */
	if (access == mem_access_write || access == mem_access_init) {
		if (!page->data)
			page->data = calloc(1, MEM_PAGESIZE);
		memcpy(page->data + offset, buf, size);
		struct ptentry *entry = mem_ptentry_get(mem, addr);
		entry->dirtybit = 1;
		return;
	}

	/* Shouldn't get here. */
	abort();
}


/* Access mem at address 'addr'.
 * This access can cross page boundaries. */
void mem_access(struct mem_t *mem, uint32_t addr, int size, void *buf,
	enum mem_access_enum access)
{
	uint32_t offset;
	int chunksize;

	mem->last_address = addr;
	while (size) {
		offset = addr & (MEM_PAGESIZE - 1);
		chunksize = MIN(size, MEM_PAGESIZE - offset);
		mem_access_page_boundary(mem, addr, chunksize, buf, access);

		size -= chunksize;
		buf += chunksize;
		addr += chunksize;
	}
}

/* Creation and destruction */
struct page_table* mem_page_table_create()
{
	struct page_table *pt;
	pt = calloc(1, sizeof(struct page_table));
	uint32_t i;
	for (i = 0; i < MEM_PAGE_COUNT; ++i)
	{
		pt->entries[i] = NULL;
	}
	// TODO Check if pt->entries are all NULL
	return pt;
}

/* Creation and destruction */
struct mem_t *mem_create()
{
	struct mem_t *mem;
	mem = calloc(1, sizeof(struct mem_t));

	mem->sharing = 1;
	mem->safe = mem_safe_mode;
	mem->clock_pointer = 0;
	mem->valid_ptentries_size = 0;
	uint32_t addr = 0, i;
	for (i = 0; i < MEM_PAGE_COUNT; ++i)
	{
		mem->pages[i] = (struct mem_page_t*) calloc(1, sizeof(struct mem_page_t));
		mem->free_frames[i] = addr;
		addr += MEM_PAGESIZE;
	}
	mem->free_frames_size = MEM_PAGE_COUNT;
	mem->pt = mem_page_table_create();
	return mem;
}

void mem_free(struct mem_t *mem)
{
	int i;
	
	/* Free pages */
	for (i = 0; i < MEM_PAGE_COUNT; i++)
		mem_page_free(mem, mem->valid_ptentries[i]->tag);

	free(mem->pt->entries);
	/* This must have released all host mappings.
	 * Now, free memory structure. */
	assert(!mem->host_mapping_list);
	free(mem);
}


/* This function finds a free memory region to allocate 'size' bytes
 * starting at address 'addr'. */
uint32_t mem_map_space(struct mem_t *mem, uint32_t addr, int size)
{
	uint32_t tag_start, tag_end;

	assert(!(addr & (MEM_PAGESIZE - 1)));
	assert(!(size & (MEM_PAGESIZE - 1)));
	tag_start = addr;
	tag_end = addr;
	for (;;) {

		/* Address space overflow */
		if (!tag_end)
			return (uint32_t) -1;
		
		/* Not enough free pages in current region */
		if (mem_page_get(mem, tag_end)) {
			tag_end += MEM_PAGESIZE;
			tag_start = tag_end;
			continue;
		}
		
		/* Enough free pages */
		if (tag_end - tag_start + MEM_PAGESIZE == size)
			break;
		assert(tag_end - tag_start + MEM_PAGESIZE < size);
		
		/* we have a new free page */
		tag_end += MEM_PAGESIZE;
	}


	/* Return the start of the free space */
	return tag_start;
}


uint32_t mem_map_space_down(struct mem_t *mem, uint32_t addr, int size)
{
	uint32_t tag_start, tag_end;

	assert(!(addr & (MEM_PAGESIZE - 1)));
	assert(!(size & (MEM_PAGESIZE - 1)));
	tag_start = addr;
	tag_end = addr;
	for (;;) {

		/* Address space overflow */
		if (!tag_start)
			return (uint32_t) -1;
		
		/* Not enough free pages in current region */
		if (mem_page_get(mem, tag_start)) {
			tag_start -= MEM_PAGESIZE;
			tag_end = tag_start;
			continue;
		}
		
		/* Enough free pages */
		if (tag_end - tag_start + MEM_PAGESIZE == size)
			break;
		assert(tag_end - tag_start + MEM_PAGESIZE < size);
		
		/* we have a new free page */
		tag_start -= MEM_PAGESIZE;
	}

	/* Return the start of the free space */
	return tag_start;
}


/* Allocate (if not already allocated) all necessary memory pages to
 * access 'size' bytes at 'addr'. These two fields do not need to be
 * aligned to page boundaries.
 * If some page already exists, add permissions. */
void mem_map(struct mem_t *mem, uint32_t addr, int size,
	enum mem_access_enum perm)
{
	uint32_t tag1, tag2, tag;
	struct mem_page_t *page;

	/* Calculate page boundaries */
	tag1 = addr & ~(MEM_PAGESIZE-1);
	tag2 = (addr + size - 1) & ~(MEM_PAGESIZE-1);

	/* Allocate pages */
	for (tag = tag1; tag <= tag2; tag += MEM_PAGESIZE) {
		page = mem_page_get(mem, tag);
		if (!page)
			page = mem_page_create(mem, tag, perm);
		page->perm |= perm;
	}
}


/* Deallocate memory pages. The addr and size parameters must be both
 * multiple of the page size.
 * If some page was not allocated, the corresponding address range is skipped.
 * If a host mapping is caught in the range, it is deallocated with a call
 * to 'mem_unmap_host'. */
void mem_unmap(struct mem_t *mem, uint32_t addr, int size)
{
	uint32_t tag1, tag2, tag;

	/* Calculate page boundaries */
	assert(!(addr & (MEM_PAGESIZE - 1)));
	assert(!(size & (MEM_PAGESIZE - 1)));
	tag1 = addr & ~(MEM_PAGESIZE-1);
	tag2 = (addr + size - 1) & ~(MEM_PAGESIZE-1);

	/* Deallocate pages */
	for (tag = tag1; tag <= tag2; tag += MEM_PAGESIZE)
		mem_page_free(mem, tag);
}


/* Map guest pages with the data allocated by a host 'mmap' call.
 * When this space is allocated with 'mem_unmap', the host memory
 * will be freed with a host call to 'munmap'.
 * Guest pages must already exist.
 * Both 'addr' and 'size' must be a multiple of the page size. */
void mem_map_host(struct mem_t *mem, struct fd_t *fd, uint32_t addr, int size,
	enum mem_access_enum perm, void *host_ptr)
{
	uint32_t ptr;
	struct mem_page_t *page;
	struct mem_host_mapping_t *hm;

	/* Check restrictions */
	if (addr & ~MEM_PAGEMASK)
		fatal("mem_map_host: 'addr' not a multiple of page size");
	if (size & ~MEM_PAGEMASK)
		fatal("mem_map_host: 'size' not a multiple of page size");
	
	/* Create host mapping, and insert it into the list head. */
	hm = calloc(1, sizeof(struct mem_host_mapping_t));
	hm->host_ptr = host_ptr;
	hm->addr = addr;
	hm->size = size;
	hm->next = mem->host_mapping_list;
	strncpy(hm->path, fd->path, MAX_PATH_SIZE);
	mem->host_mapping_list = hm;
	syscall_debug("  host mapping created for '%s'\n", hm->path);
	
	/* Make page data point to new data */
	for (ptr = addr; ptr < addr + size; ptr += MEM_PAGESIZE) {
		page = mem_page_get(mem, ptr);
		if (!page)
			fatal("mem_map_host: requested range not allocated");

		/* It is not allowed that the page belong to a previous host
		 * mapping. If so, it should have been unmapped before. */
		if (page->host_mapping)
			fatal("mem_map_host: cannot overwrite a previous host mapping");

		/* If page is pointing to some data, overwrite it */
		if (page->data)
			free(page->data);

		/* Create host mapping */
		page->host_mapping = hm;
		page->data = ptr - addr + host_ptr;
		hm->pages++;
	}
}


/* Deallocate host mapping starting at address 'addr'.
 * A host call to 'munmap' is performed to unmap host space. */
void mem_unmap_host(struct mem_t *mem, uint32_t addr)
{
	int ret;
	struct mem_host_mapping_t *hm, *hmprev;

	/* Locate host mapping in the list */
	hmprev = NULL;
	hm = mem->host_mapping_list;
	while (hm && hm->addr != addr) {
		hmprev = hm;
		hm = hm->next;
	}

	/* Remove it from the list */
	assert(hm);
	if (hmprev)
		hmprev->next = hm->next;
	else
		mem->host_mapping_list = hm->next;
	
	/* Perform host call to 'munmap' */
	ret = munmap(hm->host_ptr, hm->size);
	if (ret < 0)
		fatal("mem_unmap_host: host call 'munmap' failed");
	
	/* Free host mapping */
	syscall_debug("  host mapping removed for '%s'\n", hm->path);
	free(hm);
}


/* Assign protection attributes to pages */
void mem_protect(struct mem_t *mem, uint32_t addr, int size, enum mem_access_enum perm)
{
	uint32_t tag1, tag2, tag;
	struct mem_page_t *page;
	int prot, err;

	/* Calculate page boundaries */
	assert(!(addr & (MEM_PAGESIZE - 1)));
	assert(!(size & (MEM_PAGESIZE - 1)));
	tag1 = addr & ~(MEM_PAGESIZE-1);
	tag2 = (addr + size - 1) & ~(MEM_PAGESIZE-1);

	/* Allocate pages */
	for (tag = tag1; tag <= tag2; tag += MEM_PAGESIZE) {
		page = mem_page_get(mem, tag);
		if (!page)
			continue;

		/* Set page new protection flags */
		page->perm = perm;

		/* If the page corresponds to a host mapping, host page must
		 * update its permissions, too */
		if (page->host_mapping) {
			prot = (perm & mem_access_read ? PROT_READ : 0) |
				(perm & mem_access_write ? PROT_WRITE : 0) |
				(perm & mem_access_exec ? PROT_EXEC : 0);
			err = mprotect(page->data, MEM_PAGESIZE, prot);
			if (err < 0)
				fatal("mem_protect: host call to 'mprotect' failed");
		}
	}
}


void mem_write_string(struct mem_t *mem, uint32_t addr, char *str)
{
	mem_access(mem, addr, strlen(str) + 1, str, mem_access_write);
}


/* Read a string from memory and return the length of the read string.
 * If the return length is equal to max_size, it means that the string did not
 * fit in the destination buffer. */
int mem_read_string(struct mem_t *mem, uint32_t addr, int size, char *str)
{
	int i;
	for (i = 0; i < size; i++) {
		mem_access(mem, addr + i, 1, str + i, mem_access_read);
		if (!str[i])
			break;
	}
	return i;
}


void mem_zero(struct mem_t *mem, uint32_t addr, int size)
{
	unsigned char zero = 0;
	while (size--)
		mem_access(mem, addr++, 0, &zero, mem_access_write);
}


void mem_dump(struct mem_t *mem, char *filename, uint32_t start, uint32_t end)
{
	FILE *f;
	uint32_t size;
	uint8_t buf[MEM_PAGESIZE];

	f = fopen(filename, "wb");
	if (!f)
		fatal("mem_dump: cannot open file '%s'", filename);
	
	/* Set unsafe mode and dump */
	mem->safe = 0;
	while (start < end) {
		size = MIN(MEM_PAGESIZE, end - start);
		mem_access(mem, start, size, buf, mem_access_read);
		fwrite(buf, size, 1, f);
		start += size;
	}

	/* Restore safe mode */
	mem->safe = mem_safe_mode;
	fclose(f);
}


void mem_load(struct mem_t *mem, char *filename, uint32_t start)
{
	FILE *f;
	uint32_t size;
	uint8_t buf[MEM_PAGESIZE];
	
	f = fopen(filename, "rb");
	if (!f)
		fatal("mem_load: cannot open file '%s'", filename);
	
	/* Set unsafe mode and load */
	mem->safe = 0;
	for (;;) {
		size = fread(buf, 1, MEM_PAGESIZE, f);
		if (!size)
			break;
		mem_access(mem, start, size, buf, mem_access_write);
		start += size;
	}

	/* Restore safe mode */
	mem->safe = mem_safe_mode;
	fclose(f);
}

/*
 * Virtual Memory Implementation
 */

void vmem_add_page(struct mem_t *mem, struct ptentry *page);
struct ptentry *run_clock_policy(struct mem_t *mem, struct ptentry *e);
void perform_page_in(struct mem_t *mem, pageop_t op);
void perform_page_out(struct mem_t *mem, pageop_t op);

/*
 * Page Replacement
 */

void vmem_load_page(struct mem_t *mem, struct ptentry *entry) {
	printf("Starting page load for vaddr: %u\n", entry->tag);
	if (entry->tag == 134512640)
		printf("First page\n");

	pageop_t pagein_op;
	pagein_op.operation = OPERATION_PAGE_IN;
	pagein_op.vaddr = entry->tag;
	pagein_op.pte = entry;

	// Check for free frames
	if (mem->free_frames_size > 0) {
		pagein_op.paddr = mem->free_frames[mem->free_frames_size - 1];
		vmem_add_page(mem, pagein_op.pte);
		printf("Loading into free_frame: %u\n", pagein_op.paddr);
	}
	else
	{
		struct ptentry *pte = run_clock_policy(mem, entry);
		pagein_op.paddr = pte->paddr;
			
		pageop_t pageout_op;
		pageout_op.operation = OPERATION_PAGE_OUT;
		pageout_op.pte = pte;
		pageout_op.vaddr = pte->tag;
		pageout_op.paddr = pte->paddr; // No physical address for pageout
		perform_page_out(mem, pageout_op);
	}
	
	perform_page_in(mem, pagein_op);
}

void* read_swap(uint32_t disk_start) {
	FILE* ft;
	printf("Reading from swap\n");
	ft = fopen("../../Sim_disk", "rb+");
	// printf("Executing fseek\n");
	fseek(ft, disk_start, SEEK_SET);
	void* buf = malloc(MEM_PAGESIZE);
	// printf("Fread\n");
	fread(buf, sizeof(char), MEM_PAGESIZE, ft);
	// printf("FClose\n");
	fclose(ft);
	printf("buf %s\n", (char*)buf);
	return buf;
}

void write_swap(uint32_t disk_start, void* data) {
	FILE* ft;
	ft = fopen("../../Sim_disk", "rb+");
	fseek(ft, disk_start, SEEK_SET);
	fwrite(data, sizeof(char), MEM_PAGESIZE, ft);
	fclose(ft);
}

void perform_page_in(struct mem_t *mem, pageop_t op) {
	printf("Page in: %u -> %u, disk_start: %u\n", op.vaddr, op.paddr, op.pte->disk_start);
	void* data = read_swap(op.pte->disk_start);
	struct mem_page_t *page = get_page_from_ptentry(mem, op.pte);
	if (page->data == NULL) {
		page->data = calloc(1, MEM_PAGESIZE);
	}
	// if (data != NULL) {
		// printf("Mempy\n");
		memcpy(page->data, data, MEM_PAGESIZE);
		// printf("Mempy complete\n");		
	// }
	
	page->perm = op.pte->perm;
	// page->host_mapping = ??;

	op.pte->valid_bit = 1;
	op.pte->paddr = op.paddr;
	op.pte->dirtybit = 0;
	mem->free_frames_size--;
}

void perform_page_out(struct mem_t *mem, pageop_t op) {
	printf("Page out: %u from %u, disk_start: %u, dirty: %d\n", op.vaddr, op.paddr, op.pte->disk_start, op.pte->dirtybit);
	if (op.pte->dirtybit) {
		struct mem_page_t *page = get_page_from_ptentry(mem, op.pte);
		op.pte->perm = page->perm;
		if (page->data) {
			void* data = malloc(MEM_PAGESIZE);
			memcpy(data, page->data, MEM_PAGESIZE);
			printf("Data: %s\n", page->data);
			write_swap(op.pte->disk_start, data);
		}
	}

	op.pte->valid_bit = 0;
	mem->free_frames[mem->free_frames_size] = op.paddr;
	mem->free_frames_size++;
}

/*
 * Page Replacement Policy (One Hand Clock Algorithm)
 */

void vmem_add_page(struct mem_t *mem, struct ptentry *page) {
	mem->valid_ptentries[mem->valid_ptentries_size] = page;
	mem->valid_ptentries_size++;
}

void inc_pointer(struct mem_t *mem) {
	mem->clock_pointer++;
	if (mem->clock_pointer == mem->valid_ptentries_size)
		mem->clock_pointer = 0;
}

void display_state(struct mem_t *mem) {
    int i;
    for (i = 0; i < mem->valid_ptentries_size; i++) {
        if (i == mem->clock_pointer)
            printf("*");
        printf("[%u,%u]\t", mem->valid_ptentries[i]->tag, mem->valid_ptentries[i]->used);
    }
    printf("\n");
}

struct ptentry* run_clock_policy(struct mem_t *mem, struct ptentry* newpage) {
    printf("Starting page replacement, initial state:\n");
    // display_state(mem);
    struct ptentry **page_list = mem->valid_ptentries;
	while (1) {
		uint32_t clock_pointer = mem->clock_pointer;
		if (!page_list[clock_pointer]->used) {
			struct ptentry * page_to_replace = page_list[clock_pointer];
			page_list[clock_pointer] = newpage;
			inc_pointer(mem);
            printf("OUT: %u,  IN: %u\n", page_to_replace->tag, newpage->tag);
            return page_to_replace;
		} else {
			page_list[clock_pointer]->used = 0;
			inc_pointer(mem);
		}
        // display_state(mem);
	}
}


