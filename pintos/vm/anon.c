/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include "vm/vm.h"
#include "devices/disk.h"
#include "threads/vaddr.h"

/* DO NOT MODIFY BELOW LINE */
static struct disk *swap_disk;
static struct bitmap *swap_table;
static bool anon_swap_in (struct page *page, void *kva);
static bool anon_swap_out (struct page *page);
static void anon_destroy (struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations anon_ops = {
	.swap_in = anon_swap_in,
	.swap_out = anon_swap_out,
	.destroy = anon_destroy,
	.type = VM_ANON,
};

/* Initialize the data for anonymous pages */
void
vm_anon_init (void) {
	/* TODO: Set up the swap_disk. */
	swap_disk = disk_get (1, 1);
}

/* Initialize the file mapping */
bool
anon_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */

	ASSERT(type == VM_ANON);

	page->operations = &anon_ops;
	struct anon_page *anon_page = &page->anon;
	
	page->anon = (struct anon_page) {
		/* TODO: executable을 evict할 때 swap slot으로 보내지 않는다면, aux 복사해줘야 함*/
		.swap_index = NULL
	};

	return true;
}

static void
copy_frame_from_swap (size_t swap_index, void *kva) {
	for(int i = 0; i < 8; i++) {
		disk_read(swap_disk, anon_page->swap_index, kva + (i * PGSIZE));
	}
}

static bool
pop_frame_from_swap (struct anon_page *anon_page, void *kva) {

	copy_frame_from_swap(anon_page->swap_index, kva);

	destroy_disk(anon_page->swap_index);

	anon_page->swap_index = NULL;
}

/* Swap in the page by read contents from the swap disk. */
static bool
anon_swap_in (struct page *page, void *kva) {
	struct anon_page *anon_page = &page->anon;

	return pop_frame_from_swap(anon_page, kva);
	
}

static size_t
push_frame_to_swap (void * kva) {

}

/* Swap out the page by writing contents to the swap disk. */
static bool
anon_swap_out (struct page *page) {
	struct anon_page *anon_page = &page->anon;

	if ((anon_page->swap_index = push_frame_to_swap (page->frame->kva)) != NULL) {
		return true;
	}
	return false;

}

/* Destroy the anonymous page. PAGE will be freed by the caller. */
static void
anon_destroy (struct page *page) {
	struct anon_page *anon_page = &page->anon;
	// destroy에서 frame NULL인 분기에 대해 PANIC("todo") 넣기
	if(page->frame == NULL) {
		PANIC("todo, after swap Impl. Abnormal if no swapped out frames");
	} else {

		free(page->frame);
	}
}
