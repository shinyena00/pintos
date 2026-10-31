/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include "vm/vm.h"
#include "devices/disk.h"
#include "threads/vaddr.h"
#include "threads/synch.h"
#include "lib/kernel/bitmap.h"
#include "threads/malloc.h"

/* DO NOT MODIFY BELOW LINE */
static struct disk *swap_disk;
static struct bitmap *swap_table;
static struct lock swap_table_lock;

static bool anon_swap_in (struct page *page, void *kva);
static bool anon_swap_out (struct page *page);
static void anon_destroy (struct page *page);

#define NO_SLOT BITMAP_ERROR

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
	/*전역에서 관리하는 bitmap이라 init할 때 만들어주고 power off 될 때까지 destroy할 필요 없음*/
	swap_table = bitmap_create(disk_size(swap_disk) / (PGSIZE / DISK_SECTOR_SIZE));
	lock_init(&swap_table_lock);
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
		.swap_index = NO_SLOT
	};

	return true;
}

static disk_sector_t
get_sector_num (size_t swap_index) {
	return swap_index * (PGSIZE / DISK_SECTOR_SIZE);
}

static void
free_disk_slot (size_t swap_index) {
	lock_acquire(&swap_table_lock);
	bitmap_reset(swap_table, swap_index);
	lock_release(&swap_table_lock);
}

static void
copy_frame_from_swap (size_t swap_index, void *kva) {
	disk_sector_t start_sector = get_sector_num(swap_index);
	for(int i = 0; i < 8; i++) {
		disk_read(swap_disk, (start_sector + i), kva + (i * DISK_SECTOR_SIZE));
	}
}

static void
pop_frame_from_swap (struct anon_page *anon_page, void *kva) {

	ASSERT(anon_page->swap_index != NO_SLOT);

	copy_frame_from_swap(anon_page->swap_index, kva);

	free_disk_slot(anon_page->swap_index);

	anon_page->swap_index = NO_SLOT;
}

/* Swap in the page by read contents from the swap disk. */
static bool /*이건 실패하는 경로가 없는 건가?*/
anon_swap_in (struct page *page, void *kva) {
	struct anon_page *anon_page = &page->anon;

	pop_frame_from_swap(anon_page, kva);

	return;
	
}

static void
frame_copy_to_disk (size_t swap_index, void *kva) {
	disk_sector_t start_sector = get_sector_num(swap_index);

	for(int i = 0; i < 8; i++) {
		disk_write(swap_disk, (start_sector + i), kva + (i * DISK_SECTOR_SIZE));
	}
}

static size_t
acquire_slot (void) {
	lock_acquire(&swap_table_lock);
	size_t swap_index = bitmap_scan_and_flip(swap_table, 0, 1, false);
	lock_release(&swap_table_lock);

	/*swap 할 수 있는 slot이 없으면 panic*/
	if(swap_index == BITMAP_ERROR) {
		PANIC("swap slots are full");
	}

	return swap_index;
}

static size_t
push_frame_to_swap (struct anon_page *anon_page, void *kva) {

	size_t swap_index = acquire_slot();

	frame_copy_to_disk(swap_index, kva);

	return swap_index;

}

/* Swap out the page by writing contents to the swap disk. */
static bool
anon_swap_out (struct page *page) {
	struct anon_page *anon_page = &page->anon;

	anon_page->swap_index = push_frame_to_swap (anon_page, page->frame->kva);

	return true;
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
