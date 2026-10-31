/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "threads/mmu.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "threads/mmu.h"
#include "lib/kernel/hash.h"
#include <string.h>

#define MAX_STACK_ADDR 0x47380000

#define MAX_STACK_ADDR (USER_STACK - (1 << 20))
/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
void
vm_init (void) {
	vm_anon_init ();
	vm_file_init ();
#ifdef EFILESYS  /* For project 4 */
	pagecache_init ();
#endif
	register_inspect_intr ();
	/* DO NOT MODIFY UPPER LINES. */
	/* TODO: Your code goes here. */
}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
enum vm_type
page_get_type (struct page *page) {
	int ty = VM_TYPE (page->operations->type);
	switch (ty) {
		case VM_UNINIT:
			return VM_TYPE (page->uninit.type);
		default:
			return ty;
	}
}

/* Helpers */
static struct frame *vm_get_victim (void);
static bool vm_do_claim_page (struct page *page);
static struct frame *vm_evict_frame (void);

static uint64_t
hash_func (struct hash_elem *elem, void *aux) {
	return (uint64_t)hash_entry(elem, struct page, element)->va>>12;
}

static bool
hash_less (struct hash_elem *a, struct hash_elem *b, void *aux) {
	return hash_func(a,NULL) < hash_func(b,NULL);
}
/* Create the pending page object with initializer and insert page to spt. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
bool
vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable,
		vm_initializer *init, void *aux) {

	ASSERT (VM_TYPE(type) != VM_UNINIT);
	upage = pg_round_down(upage);
	struct page *page;
	struct supplemental_page_table *spt = &thread_current ()->spt;

	/* Check wheter the upage is already occupied or not. */
	if (spt_find_page (spt, upage) == NULL) {
		/* TODO: Create the page, fetch the initialier according to the VM type,
		 * TODO: and then create "uninit" page struct by calling uninit_new. You
		 * TODO: should modify the field after calling the uninit_new. */

		page = malloc(sizeof(struct page));
		if (!page) {
			goto err;
		}

		switch (VM_TYPE(type)) {
			case VM_ANON : {
				uninit_new(page, upage, init, type, aux, anon_initializer);
				break;
			} case VM_FILE : {
				uninit_new(page, upage, init, type, aux, file_backed_initializer);
				break;
			} default : {
				goto err;
			}
		}
	} else {
		return false;
	}
  	page->is_writable = writable; // type 무관

	/* TODO: Insert the page into the spt. */

    if(!spt_insert_page(spt, page)) {
			goto err;
		} else {
			return true;
		}
err:
	free(page);
	return false;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page *
spt_find_page (struct supplemental_page_table *spt, void *va) {
	struct page *page = NULL;
	struct page temp = {.va = va};
	struct hash_elem *element;
	return (element = hash_find(&spt->hash, &temp.element)) ? hash_entry(element, struct page, element) : NULL;
}

/* Insert PAGE into spt with validation. */
bool
spt_insert_page (struct supplemental_page_table *spt,
		struct page *page) {
	return hash_insert(&spt->hash, &page->element)?0:1;
}

void
spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	
	hash_delete(&spt->hash, &page->element);
	vm_dealloc_page (page);
	return true;
}

/* Get the struct frame, that will be evicted. */
static struct frame *
vm_get_victim (void) {
	struct frame *victim = NULL;
	 /* TODO: The policy for eviction is up to you. */

	return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *
vm_evict_frame (void) {
	struct frame *victim = vm_get_victim ();
	/* TODO: swap out the victim and return the evicted frame. */


	PANIC("todo");

	palloc_free_page(victim->kva);
	/**
	 * Update corresponding page status -
	 * using swap_out would be adeq
	 */
	return NULL;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame *
vm_get_frame (void) {
	struct frame *frame = NULL;
	/* TODO: Fill this function. */

	void *kva = palloc_get_page(PAL_USER); 
	if( kva == NULL) {
		frame = vm_evict_frame();
	}
  else {
  	frame = malloc(sizeof(*frame));
  }
  ASSERT (frame != NULL);

	// TODO: frame table 없는 경우이므로 table 있다면 업데이트 해줘야

	frame->kva = kva;
	frame->page = NULL;
	ASSERT (frame->page == NULL);

	return frame;
}

/* Growing the stack. */
bool
vm_stack_growth (void *addr) {
	addr = pg_round_down(addr);

	if (vm_alloc_page(VM_ANON, addr, true)) {
		if (vm_claim_page(addr)) {
			return true;
		} // TODO: process will terminate if returned false. need vm_dealloc in process cleanup procedure
	}
	return false;
}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp (struct page *page UNUSED) {
}

bool
addr_isnt_user_vaild (void * addr) {
	if (0x400000 > addr || addr >= USER_STACK) {
		return true;
	}
	return false;
}

/* Return true on success */
bool
vm_try_handle_fault (struct intr_frame *f , void *addr ,
		bool user , bool write , bool not_present ) {
	struct supplemental_page_table *spt = &thread_current ()->spt;
	struct page *page = NULL;
	/* TODO: Validate the fault */
	/* TODO: Your code goes here */
	
	if (addr_isnt_user_vaild (addr)) {
		return false;           
	} 

	page = spt_find_page(spt, addr);

	if(page == NULL) {
		//스택 접근 판단 로직 - tc 확인해봐야
		if(addr >= thread_current ()->user_rsp - 8 && addr >= MAX_STACK_ADDR) { // if 또는 thread에 저장한 rsp
			return vm_stack_growth(addr); 
		} else {
			return false;
		}
	}

	
	if(!page->is_writable && write) {
		return false;
	}

	return vm_do_claim_page(page);
	
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void
vm_dealloc_page (struct page *page) {
	destroy (page);
	free (page);
}

/* Claim the page that allocate on VA. */
bool
vm_claim_page (void *va) {
	struct page *page = spt_find_page(&thread_current()->spt, va);
	/* TODO: Fill this function */

	return vm_do_claim_page (page);
}

/* Claim the PAGE and set up the mmu. */
static bool
vm_do_claim_page (struct page *page) {
	struct frame *frame = vm_get_frame ();

	/* Set links */
	frame->page = page;
	page->frame = frame;

	/* TODO: Insert page table entry to map page's VA to frame's PA. */

	if(!pml4_set_page(thread_current()->pml4, page->va, frame->kva, page->is_writable )) { // va, kva should be page aligned
		palloc_free_page(frame->kva);
		free(frame);
		/** TODO: frame table 업데이트 로직 고민하기
		 * cand 1. update frame table
		 * cand 2. process might terminate if returned false. cleanup frames/pages in thread_exit procedure
		 */
		return false;
	}
	return swap_in (page, frame->kva);
}

/* Initialize new supplemental page table */
bool
supplemental_page_table_init (struct supplemental_page_table *spt ) {
	if (!hash_init(spt, hash_func, hash_less, NULL)) {
		return false;
	}
	return true;
}

static void
copy_error_free_memory (struct page *dst_page) {
	if (dst_page->operations->type == VM_UNINIT && dst_page->uninit.aux != NULL) {
		free(dst_page->uninit.aux);
	}
	if (dst_page->frame != NULL) {
		palloc_free_page(dst_page->frame->kva);
		free(dst_page->frame);
	}
	free(dst_page);
}

static bool
page_copy_uninit(struct page *dst_page, struct page *src_page) {
	memcpy(dst_page, src_page, sizeof(struct page));

	if(src_page->uninit.aux == NULL) {
		return true;
	}

	dst_page->uninit.aux = NULL;
	struct file_src *aux = malloc(sizeof(struct file_src));
	if (!aux) {
		return false;
	}
	memcpy(aux, src_page->uninit.aux, sizeof(struct file_src));
	dst_page->uninit.aux = aux;
	return true;
}

static bool
page_copy_anon(struct page *dst_page, struct page *src_page) {
	memcpy(dst_page, src_page, sizeof(struct page));
	dst_page->frame = NULL;

	struct frame *new_frame;
	if (!(new_frame = vm_get_frame())) {
		return false;
	}
		
	new_frame->page = dst_page;
	dst_page->frame = new_frame;
	memcpy(new_frame->kva, src_page->frame->kva, PGSIZE);
	return true;
}

static bool
copy_file_page(struct page *dst_page, struct page *src_page) {
	memcpy(dst_page, src_page, sizeof(struct page));
	dst_page->frame = NULL;

	if (dst_page->file.file_src = copy_file_src(src_page)) {
		return true;
	}
	return false;
}

static bool
copy_file_frame(struct page *dst_page, struct page *src_page) {

}

static bool
page_copy_file_backed(struct page *dst_page, struct page *src_page) {
	if (copy_file_page(dst_page, src_page)) {
		if (copy_file_frame(dst_page, src_page)) {
			return true;
		}
	}
	return false;
}

static struct page * 
page_copy(struct page *src_page) {
	struct page *dst_page;
	if ((dst_page = malloc(sizeof(struct page))) == NULL) {
		return NULL;
	}
	switch (src_page->operations->type) {
		case VM_UNINIT: {
			if (!page_copy_uninit(dst_page, src_page)) {
				goto error;
			}
			break;
		}
		case VM_ANON: {
			/*TODO: eviction 구현*/
			if (!page_copy_anon(dst_page, src_page)) {
				goto error;
			}
			break;
		}
		case VM_FILE: {
			/*TODO: mmap 구현*/
			if (!page_copy_file_backed(dst_page, src_page)) {
				goto error;
			}
			break;
		}
		default: {
			free(dst_page);
			return NULL;
		}
	}

	if (dst_page->frame != NULL) {
		if (!pml4_set_page(thread_current()->pml4, dst_page->va, dst_page->frame->kva, dst_page->is_writable)) {
			goto error;
		}
	}

	return dst_page;

error : 
	copy_error_free_memory(dst_page);
	return NULL;
}

bool
supplemental_page_table_copy (struct supplemental_page_table *dst,
		struct supplemental_page_table *src) {
	struct hash_iterator i;
	struct hash_elem *curr;
	struct page *dst_page;

	hash_first(&i, &src->hash);
	while ((curr = hash_next(&i))){
		if (!(dst_page = page_copy(hash_entry(curr, struct page, element)))){
			return false;
		}
		if (!spt_insert_page(dst, dst_page)){
			if (dst_page->operations->type == VM_UNINIT && dst_page->uninit.aux != NULL){
				free(dst_page->uninit.aux);
			}
			if (dst_page->frame != NULL){
				pml4_clear_page(thread_current()->pml4, dst_page->va);
  				palloc_free_page(dst_page->frame->kva);
				free(dst_page->frame);
			}
			free(dst_page);
			return false;
		}
	}
	return true;
}

static void
spt_free_page (struct hash_elem *e, void *aux) {
	struct page *page = hash_entry(e, struct page, element);
	vm_dealloc_page(page);
}

/* Free the resource hold by the supplemental page table. process_exit시에는 hash_destroy 해줘야 함 */
void
supplemental_page_table_kill (struct supplemental_page_table *spt) {
	/* TODO: Destroy all the supplemental_page_table hold by thread and */
	hash_clear(&spt->hash, spt_free_page);

	/* TODO: writeback all the modified contents to the storage. */ 
		// 아직까지는 file backed 타입 없음, 후순위

}
