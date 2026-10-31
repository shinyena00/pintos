/* file.c: Implementation of memory backed file object (mmaped object). */

#include "vm/file.h"
#include "vm/vm.h"
#include "threads/vaddr.h"
#include "lib/string.h"
#include "threads/mmu.h"
#include "filesys/filesys.h"

static bool file_backed_swap_in (struct page *page, void *kva);
static bool file_backed_swap_out (struct page *page);
static void file_backed_destroy (struct page *page);
static bool copy_mmap_info(struct mmap_info *from, void *aux UNUSED);

/* DO NOT MODIFY this struct */
static const struct page_operations file_ops = {
	.swap_in = file_backed_swap_in,
	.swap_out = file_backed_swap_out,
	.destroy = file_backed_destroy,
	.type = VM_FILE,
};

/* The initializer of file vm */
void
vm_file_init (void) {
}

/* Initialize the file backed page */
bool
file_backed_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */
	page->operations = &file_ops;
	struct file_page *file_page = &page->file;

	*file_page = (struct file_page) {
		.file_src = NULL
	};

	return true;
}

static off_t
read_page_from_file( struct file * file, char *kva, uint32_t read_bytes, off_t offset) {
	bool lock_prev_status = false;
	if(lock_prev_status = lock_held_by_current_thread(&file_lock)) {
		lock_release(&file_lock);
	}

	lock_acquire(&file_lock);
	off_t read = file_read_at(file, kva, read_bytes, offset);
	lock_release(&file_lock);

	if(lock_prev_status) {
		lock_acquire(&file_lock);
	}

	memset(kva + read_bytes, 0, PGSIZE - read); 

	return read;
}

static off_t
write_page_to_file ( struct file * file, char *kva, uint32_t write_bytes, off_t offset) {

	bool lock_prev_status = false;
	if(lock_prev_status = lock_held_by_current_thread(&file_lock)) {
		lock_release(&file_lock);
	}

	lock_acquire(&file_lock);
	off_t write = file_write_at(file, kva, write_bytes, offset);
	lock_release(&file_lock);

	if(lock_prev_status) {
		lock_acquire(&file_lock);
	}

	return write;
}

/* Swap in the page by read contents from the file. */
static bool
file_backed_swap_in (struct page *page, void *kva) {
	struct file_page *file_page = &page->file;
	struct file_src *file_src = file_page->file_src;

	off_t read = read_page_from_file(file_src->file, page->frame->kva, file_src->read_bytes, file_src->offset);
	return true;
}

/* Swap out the page by writeback contents to the file. */
static bool
file_backed_swap_out (struct page *page) {
	struct file_page *file_page = &page->file;
	struct file_src *file_src = file_page->file_src;
	off_t write = 0;
	if (!pml4_is_dirty(thread_current ()->pml4, page->va)){
		memset(page->frame->kva, 0, PGSIZE);
	} else {
		write = write_page_to_file(file_src->file, page->frame->kva, file_src->read_bytes, file_src->offset);
	}
	return true;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void
file_backed_destroy (struct page *page) {
	struct file_page *file_page = &page->file;
	struct file_src *file_src = file_page->file_src;

	if(page->frame == NULL) {
		// nothing to do.
	} else {
		off_t write = 0;
		if (!pml4_is_dirty(thread_current ()->pml4, page->va)){
			memset(page->frame->kva, 0, PGSIZE);
		} else {
			write = write_page_to_file(file_src->file, page->frame->kva, file_src->read_bytes, file_src->offset);
		}
		free(page->frame);
	}
	
	free(file_page->file_src);
}
static bool
lazy_load_mmap (struct page *page, void *aux) {

	struct file_src *file_meta = aux;
	uint8_t *kva = page->frame->kva;
	uint32_t read_bytes = file_meta->read_bytes;
	off_t read;

	read = read_page_from_file( file_meta->file, kva, read_bytes, file_meta->offset);

	page->file.file_src = file_meta;

	return true;
}

static void 
mmap_clean_up_for_error (int idx, void *addr){
	struct supplemental_page_table *spt = &thread_current ()->spt;
	for(int i = 0; i < idx; i++) {
		spt_remove_page(spt, spt_find_page(spt, addr + i * PGSIZE));
	}
}

static void *
mmap_alloc_pages (void *addr, size_t length, int writable,
	struct file *file, off_t offset) {
	
	uint64_t idx = 0;	
	for (size_t step = 0; step < length; step += PGSIZE, idx++) {
		struct file_src *aux = malloc(sizeof(struct file_src));
		if (!aux) {
			mmap_clean_up_for_error(idx, addr);
			return NULL;
		}
		aux->offset = (offset + step);
		aux->read_bytes = (length - step) < PGSIZE ? (length - step) : PGSIZE;
		aux->file = file;

		if (!vm_alloc_page_with_initializer(VM_FILE, (addr + step), writable, lazy_load_mmap, aux)) {
			free(aux);
			mmap_clean_up_for_error(idx, addr);
			return NULL;
		}
	}
	return addr;
}

/* Do the mmap */


void *
do_mmap (void *addr, size_t length, int writable,
		struct file *file, off_t offset) {
	
	struct file * new_file = file_reopen(file);
	if(new_file == NULL) {
		return NULL;
	}
	
	if(!mmap_alloc_pages(addr, length, writable, new_file, offset)) {
		file_close(new_file);
		return NULL;
	}

	set_mmap_info(thread_current(), new_file, addr, length);

	return addr;
}

void
fork_mmap (struct thread *src) {
	mmap_info_apply(src, copy_mmap_info, NULL);
}

static bool
copy_mmap_info(struct mmap_info *from, void *aux UNUSED) { 
	return set_mmap_info(thread_current(), file_reopen(from->file), from->addr, from->length);
}


/* Do the munmap */
void
do_munmap (void *addr) {
	struct mmap_info *mmap_info = find_mmap_info(thread_current(), addr);
	if(!mmap_info){
		return;
	}
	destroy_mmap(mmap_info);
}

void
do_munmap_all (struct thread *curr){
	mmap_info_apply(curr, destroy_mmap, NULL);
}


static bool
update_file_page_action_func(struct mmap_info *mmap_info, struct thread *t) {
	int64_t mmap_addr = (int64_t)mmap_info->addr;
	int64_t mmap_length = mmap_info->length;
	struct file *new_file = mmap_info->file;

	for(int64_t addr = mmap_addr; addr < mmap_addr + mmap_length; addr += PGSIZE) {
		struct page *page = spt_find_page(&t->spt, addr);
		if(page == NULL) {
			return false;
		}
		ASSERT(VM_TYPE(page->operations->type) == VM_FILE);

		page->file.file_src->file = new_file;
	}

	return true;
}

void update_file_page(struct thread *t) {
	mmap_info_apply(t, update_file_page_action_func, t);
}

/* --------------------------------- */

static uint64_t 
mmap_info_hash_func (const struct hash_elem *e, void *aux) {

	return (uint64_t)hash_entry(e, struct mmap_info, element)->addr;

		// hash_bytes로 변경

	// struct mmap_info *mmap_info = hash_entry(e, struct mmap_info, element);
	// return hash_bytes(
	// 	&(mmap_info->addr),
	// 	sizeof (mmap_info->addr)
	// )
}

static bool
mmap_info_hash_less (const struct hash_elem *a, const struct hash_elem *b, void *aux UNUSED) {
	return mmap_info_hash_func(a,NULL) < mmap_info_hash_func(b,NULL);
}


bool 
file_init_mmap_info(struct thread *t) {
	struct mmap_info_container *container = &t->mmap_info_container;
	return hash_init (&(container->hash), mmap_info_hash_func, mmap_info_hash_less, NULL);
}

/* delete and free mmap_info from mmap_info_container and close opened file*/
void 
remove_mmap_info(struct thread *t, struct mmap_info *mmap_info) {
	hash_delete(&t->mmap_info_container.hash, &mmap_info->element);

	// TODO: - file lock 걸어야
	file_close(mmap_info->file);
	// TODO: - file lock release
	free(mmap_info);
}

struct mmap_info *
find_mmap_info(struct thread *t, void *addr) {
	struct mmap_info_container *container = &t->mmap_info_container;
	struct mmap_info dummy = {
		.addr = addr
	};

	struct hash_elem *res = hash_find(&t->mmap_info_container.hash, &dummy.element);
	if(res == NULL) {
		return NULL;
	} else {
		return hash_entry(res, struct mmap_info, element);
	}
}

bool 
set_mmap_info(struct thread *t, struct file *f, void *addr, size_t length) {

	struct mmap_info *mmap_info = malloc(sizeof(*mmap_info));
	if(mmap_info == NULL) {
		return false;
	}

	*mmap_info = (struct mmap_info) {
		.addr = addr,
		.file = f,
		.length = length
	};

	if(hash_insert(&(t->mmap_info_container.hash), &mmap_info->element) != NULL) {
		free(mmap_info);
		return false;
	} else {
		return true;
	}
}

/* ----------------- */

struct mmap_action_aux {
	mmap_info_action_func *action;
	void *aux;
};

static bool
mmap_info_translate_to_hash_action_func(struct hash_elem *e, void *aux) {
	struct mmap_action_aux *mmap_aux = aux;
	struct mmap_info *mmap_info = hash_entry(e, struct mmap_info, element);

	if(mmap_aux->action == NULL) {
		return true;
	} else{
		return (mmap_aux->action) (mmap_info, mmap_aux->aux);
	}
}

bool mmap_info_apply(struct thread *t, mmap_info_action_func *mmap_func, void *aux) {
	struct mmap_action_aux mmap_aux = {
		.action = mmap_func,
		.aux = aux
	};
	struct hash *hash = &(t->mmap_info_container.hash);
	hash->aux = &mmap_aux;

	bool res = hash_apply(hash, mmap_info_translate_to_hash_action_func);
	hash->aux = NULL;
	return res;
}

void 
destroy_mmap_info_container(struct thread *t, mmap_info_action_func *mmap_func, void *aux) {
		struct mmap_action_aux mmap_aux = {
		.action = mmap_func,
		.aux = aux
	};

	struct hash *hash = &(t->mmap_info_container.hash);
	hash->aux = &mmap_aux;

	hash_destroy(hash, mmap_info_translate_to_hash_action_func);
	hash->aux = NULL;
}
void
destroy_mmap (struct mmap_info *mmap_info)  {

	char *addr = mmap_info -> addr;
	struct file *file = mmap_info -> file;
	size_t length = mmap_info -> length;
	for(size_t seek = 0; seek < length; seek += PGSIZE){
		struct page *page = spt_find_page(&thread_current()->spt, addr + seek);
		if (page == NULL) {
			return;
		}
		void *va = page->va;
		spt_remove_page(&thread_current()-> spt, page);
		pml4_clear_page(thread_current ()-> pml4, va);
	}
	remove_mmap_info(thread_current(), mmap_info);
}
