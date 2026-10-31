#ifndef VM_FILE_H
#define VM_FILE_H
#include "filesys/file.h"
#include "lib/kernel/hash.h"

struct page;
enum vm_type;
struct thread;  /* 추가 */

struct file_page {
	struct file_src * file_src;
};

struct mmap_info {
	struct file *file;
	void *addr;
	size_t length;
	struct hash_elem element;
};

struct mmap_info_container {
	struct hash hash;
};

void vm_file_init (void);
bool file_backed_initializer (struct page *page, enum vm_type type, void *kva);
void *do_mmap (void *addr, size_t length, int writable,
		struct file *file, off_t offset);
void do_munmap (void *va);

typedef bool mmap_info_action_func(struct mmap_info *mmap_info, void *aux);


bool file_init_mmap_info(struct thread *t);
void remove_mmap_info(struct thread *t, struct mmap_info *mmap_info);
struct mmap_info *find_mmap_info(struct thread *t, void *addr);
bool set_mmap_info(struct thread *t, struct file *f, void *addr, size_t length);

bool mmap_info_apply(struct thread *t, mmap_info_action_func *mmap_func, void *aux);
void destroy_mmap_info_container(struct thread *t, mmap_info_action_func *mmap_func, void *aux);

void do_munmap_all (struct thread *);
void destroy_mmap (struct mmap_info *);
void fork_mmap (struct thread *);
bool copy_mmap_info(struct mmap_info *from, void *aux);

bool mmap_update_file_page(struct thread *);
#endif
