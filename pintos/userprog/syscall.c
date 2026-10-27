#include "userprog/syscall.h"
#include <stdio.h>
#include <string.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "threads/vaddr.h"
#include "userprog/process.h"
#include "threads/init.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "threads/malloc.h"
#include "filesys/filesys.h"
#include "devices/input.h"
#include "threads/palloc.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);


/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */
#define PAGE_SIZE 4096

static bool is_invaild_filename(char * filename);
static struct fd_info * seek_fd_info (struct thread *curr, int fd);
static struct fd_info * dup_fd_info(struct fd_info *oldfd_info, int newfd);
static bool is_valid_buffer(char *buffer, int size, bool is_write);

static void sys_halt (struct intr_frame *f);
static void sys_exit (struct intr_frame *f);
static void sys_fork (struct intr_frame *f);
static void sys_exec (struct intr_frame *f);
static void sys_wait (struct intr_frame *f);
static void sys_create (struct intr_frame *f);
static void sys_remove (struct intr_frame *f);
static void sys_open (struct intr_frame *f);
static void sys_filesize (struct intr_frame *f);
static void sys_read (struct intr_frame *f);
static void sys_write (struct intr_frame *f);
static void sys_seek (struct intr_frame *f);
static void sys_tell (struct intr_frame *f);
static void sys_close (struct intr_frame *f);
static void sys_dup2(struct intr_frame *f); 

void
syscall_init (void) {
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);

}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f UNUSED) {

	uint64_t syscall_num = f->R.rax;
	thread_current()->user_rsp = f->rsp;
	
	switch (syscall_num){
		case SYS_HALT:
			sys_halt(f);
			break;
		case SYS_EXIT:
			sys_exit(f);
			break;
		case SYS_FORK:
			sys_fork(f);
			break;
		case SYS_EXEC:
			sys_exec(f);
			break;
		case SYS_WAIT:
			sys_wait(f);
			break;
		case SYS_CREATE:
			sys_create(f);
			break;
		case SYS_REMOVE:
			sys_remove(f);
			break;
		case SYS_OPEN:
			sys_open(f);
			break;
		case SYS_READ:
			sys_read(f);
			break;
		case SYS_FILESIZE:
			sys_filesize(f);
			break;
		case SYS_WRITE:
			sys_write(f);
			break;
		case SYS_SEEK:
			sys_seek(f);
			break;
		case SYS_TELL:
			sys_tell(f);
			break;
		case SYS_CLOSE:
			sys_close(f);
			break;
		case SYS_DUP2:
			sys_dup2(f);
			break;
		default:
			printf ("system call!\n");
			thread_exit ();
	}
		
}

static void
sys_halt (struct intr_frame *f UNUSED) {
	power_off();
}

static void
sys_exit (struct intr_frame *f) {
	thread_current()->exit_status = f->R.rdi;
	thread_exit();
}

static void 
sys_fork (struct intr_frame *f) {
	char *fname = (void *)f->R.rdi;	
	
	if(fname == NULL || is_kernel_vaddr(fname)) {
		thread_current()->exit_status = -1;
		thread_exit();
	}
	
	if (is_invaild_filename(fname)) {
		f->R.rax = -1;
		return;
	}

	tid_t child_pid = process_fork(fname, f);
	f->R.rax = child_pid;
}

static void 
sys_exec (struct intr_frame *f) {
	char *fname = (void *)f->R.rdi;
	char *buffer = palloc_get_page(0);

	if (buffer == NULL){
		f->R.rax = -1;
		return;
	}		

	/* 널문자까지 포함해서 page size여야함 */
	if ((strlcpy(buffer, (const char *)fname, PAGE_SIZE)) >= PAGE_SIZE){
		palloc_free_page(buffer);
		/* 커맨드가 페이지에 다 안 들어가면 exec 실패 → exit 해야됨 */
		thread_current()->exit_status = -1;
		thread_exit();
	}

	int reslt = process_exec(buffer);
	
	if (reslt == -1){
		/* 주소 공간이 이미 지워져서 돌아갈 곳이 없음 -> exit 해야됨 */
		thread_current()->exit_status = -1;
		thread_exit();
	}
}


static void
sys_wait (struct intr_frame *f) {
	f->R.rax = process_wait(f->R.rdi);
}

static void
sys_create (struct intr_frame *f) {
	char *fname = (char *)f->R.rdi;
	unsigned int initsize = f->R.rsi;
	
	if (fname == '\0'  || is_kernel_vaddr(fname)) {
		thread_current()->exit_status = -1;
		thread_exit();
	}

	if (is_invaild_filename(fname) || *fname == NULL){
		f->R.rax = 0;
		return;
	}
	
	lock_acquire(&file_lock);
	f->R.rax = filesys_create(fname, initsize);
	lock_release(&file_lock);
}

static void 
sys_remove (struct intr_frame *f) {
	char *fname = (char *)f->R.rdi;

	if(fname == NULL || is_kernel_vaddr(fname)) {
		thread_current()->exit_status = -1;
		thread_exit();
	}

	if (is_invaild_filename(fname) || *fname == NULL){
		f->R.rax = 0;
		return;
	}

	lock_acquire(&file_lock);
	f->R.rax = filesys_remove(fname);
	lock_release(&file_lock);
}

static void
sys_open (struct intr_frame *f) {
	char *fname = (char *)f->R.rdi;

	struct file *file;

	if(fname == NULL || is_kernel_vaddr(fname)) {
		thread_current()->exit_status = -1;
		thread_exit();
	}
	
	if (!is_valid_buffer(fname, strlen(fname), 0) || is_invaild_filename(fname)) {
		f->R.rax = -1;       
		return;
	}

	lock_acquire(&file_lock);
	if ((file = filesys_open(fname)) == NULL) {
		f->R.rax = -1;
		lock_release(&file_lock);
		return;
	}
	lock_release(&file_lock);
	
	struct thread *curr = thread_current();
	int fd = curr->open;

	struct fd_info *fd_info;
	if ((fd_info = make_fd_info(fd, file)) == NULL) {
		/*실패하면 파일을 닫고 나가야함*/
		lock_acquire(&file_lock);
		file_close(file);
		lock_release(&file_lock);
		
		f->R.rax = -1;
		return;
	}
	
	list_push_front(&curr->file_list, &fd_info->elem);
	curr->open++;

	f->R.rax = fd;
}

static void
sys_filesize(struct intr_frame *f) {
	int fd = f->R.rdi;
	struct fd_info *fd_info;

	if((fd_info = seek_fd_info(thread_current() , fd)) == NULL || !(isnt_stdin_or_stdout(fd_info))) {
		f->R.rax = -1;
		return;
	}

	lock_acquire(&file_lock);
	f->R.rax = file_length(fd_info->file_info->my_file);
	lock_release(&file_lock);

}

static void
sys_read (struct intr_frame *f) {


	int fd = f->R.rdi;
	char *buffer = (char *)f->R.rsi;
	unsigned size = f->R.rdx;

	if(!is_valid_buffer(buffer, size, 0)){
		thread_current()->exit_status = -1;
		thread_exit();
	}

	struct fd_info *fd_info;

	if ((fd_info = seek_fd_info(thread_current() , fd)) == NULL) {
		f->R.rax = -1;
		return;
	}

	if(fd_info->file_info->my_file == STDOUT_FILE){
		f->R.rax = -1;
		return;
	}

	if(size == 0){
		f->R.rax = 0;
		return;
	}


	if(fd_info->file_info->my_file == STDIN_FILE){
		for(int i= 0; i < size; i++){
			buffer[i] = input_getc();
		}
		f->R.rax = size;
		return;
	}

	lock_acquire(&file_lock);
	f->R.rax = file_read(fd_info->file_info->my_file, buffer, size);
	lock_release(&file_lock);
}


static void
sys_seek (struct intr_frame *f) {
	int fd = f->R.rdi;
	unsigned position = f->R.rsi;

	if(position > INT32_MAX){
		return;
	}

	struct fd_info *fd_info;

	if ((fd_info = seek_fd_info(thread_current() , fd)) == NULL) {
		return;
	}

	if(!isnt_stdin_or_stdout(fd_info)) {
		return;
	}

	lock_acquire(&file_lock);
	file_seek(fd_info->file_info->my_file, position);
	lock_release(&file_lock);
}

static void 
sys_tell (struct intr_frame *f) {
	int fd = f->R.rdi;
	struct fd_info *fd_info;
	
	if((fd_info = seek_fd_info(thread_current(), fd)) == NULL || !(isnt_stdin_or_stdout(fd_info))) {
		f->R.rax = -1;
		return;
	}
	
	lock_acquire(&file_lock);
	f->R.rax = file_tell(fd_info->file_info->my_file);
	lock_release(&file_lock);
	
}


static void
sys_write (struct intr_frame *f) {
	void *  buffer = (void *)f->R.rsi;
	int fd = f->R.rdi;
	uint64_t size = f->R.rdx;

	if (!is_valid_buffer(buffer, size, 1)) {
		thread_current()->exit_status = -1;
		thread_exit(); // 접근하면 안 되는 포인터를 주면 프로세스 종료 
		
	}


	struct fd_info *fd_info;

	if((fd_info = seek_fd_info(thread_current(), fd)) == NULL) {
		f->R.rax = -1;
		return;
	}
	
	if (fd_info->file_info->my_file == STDIN_FILE) {
		f->R.rax = -1;
		return;
	}

	if (fd_info->file_info->my_file == STDOUT_FILE) {
		if(size == 0){
			f->R.rax = 0;
			return;
		}
		else{
			//putbuf는 자기가 락을 따로 잡음
			putbuf(buffer, size);
			f->R.rax = size;
			return;
		}
	}

	lock_acquire(&file_lock);
	f->R.rax = file_write(fd_info->file_info->my_file, buffer, size);
	lock_release(&file_lock);
}

static void
sys_close (struct intr_frame *f) {
	int fd = f->R.rdi;

	struct thread *curr = thread_current();
	struct list_elem *ptr;
	struct list *list = &curr->file_list;

	for(ptr = list_begin(list); ptr != list_end(list); ptr = list_next(ptr)){
		if(list_entry(ptr, struct fd_info, elem)-> fd == fd){
			struct fd_info *fd_info = list_entry(ptr, struct fd_info, elem);
			if (fd_info->file_info->reference != 0){
				fd_info->file_info->reference--;
			}
			if (fd_info->file_info->reference == 0) {
				if(fd_info->file_info->my_file != STDIN_FILE && fd_info->file_info->my_file != STDOUT_FILE) {
					lock_acquire(&file_lock);
					file_close(fd_info->file_info->my_file);
					lock_release(&file_lock);
				}
				free(fd_info->file_info);
			}
			list_remove(ptr);
			free(fd_info);
			break;
		}
	}
}

static void
sys_dup2(struct intr_frame *f) {
	int oldfd = f->R.rdi;
	int newfd = f->R.rsi;

	int open = thread_current()->open;
	thread_current()->open = open+1 > newfd + 1 ? open+1 : newfd+1;

	struct fd_info *oldfd_info;
	if ((oldfd_info = seek_fd_info(thread_current(), oldfd)) == NULL) {
		f->R.rax = -1;
		return;
	}

	if (oldfd == newfd) {
		f->R.rax = newfd;
		return;
	}

	struct fd_info *newfd_info;

	if ((newfd_info = seek_fd_info(thread_current(), newfd)) != NULL) {
		if (!isnt_stdin_or_stdout(newfd_info)) {
			struct fd_info *std_info = newfd_info;
			newfd_info = dup_fd_info(oldfd_info, newfd);
			if (newfd_info == NULL) {
				f->R.rax = -1;
				return;
			}

			list_remove(&std_info->elem);

			std_info->file_info->reference--;
			if(std_info->file_info->reference == 0) {
				free(std_info->file_info);
			}
			free(std_info);	

			list_push_back(&thread_current()->file_list, &newfd_info->elem);
		}
		else {
			newfd_info->file_info->reference--;
			if (newfd_info->file_info->reference == 0) {
				lock_acquire(&file_lock);
				file_close(newfd_info->file_info->my_file);
				lock_release(&file_lock);
				free(newfd_info->file_info);
			}

			newfd_info->file_info = oldfd_info->file_info;
			newfd_info->file_info->reference++;
		}
	}
	else if ((newfd_info = dup_fd_info(oldfd_info, newfd)) != NULL){
		list_push_back(&thread_current()->file_list, &newfd_info->elem);
	}
	else {
		f->R.rax = -1;
		return;
	}
	
	f->R.rax = newfd;

}

static struct fd_info * dup_fd_info(struct fd_info *oldfd_info, int newfd) {
	struct fd_info *newfd_info;
	if((newfd_info = malloc(sizeof(struct fd_info))) != NULL){
		newfd_info->file_info = oldfd_info->file_info;
		newfd_info->fd = newfd;
		newfd_info->file_info->reference++;

		return newfd_info;
	}
	else{
		return NULL;
	}
}

static bool is_invaild_filename(char * filename){
	size_t size;
	size = strnlen(filename, NAME_MAX+1);
	return !((size > 0) && (size <= (NAME_MAX)));
}

/**
 * valid_buffer인지 검사한다는 것은 그 주소에 있는 값을 곧 사용한다는 뜻
 * 따라서 pml4_get_page 실행해보고, NULL이면 vm_claim_page를 사용해 메모리에 등재하는 과정을 동시에 실시한다.
 */
static bool is_valid_buffer(char *buffer, int size, bool is_write) {
	struct thread *curr = thread_current();
	if (is_kernel_vaddr(buffer + size)) {
		return false;
	}
	for (char* addr = pg_round_down(buffer); buffer + size > addr; addr+=(1<<12)) {
		if (!pml4_get_page(curr->pml4 ,addr) && !vm_claim_page(addr)) {
			return false;
		}
	}
	return true;
}

static struct fd_info * seek_fd_info (struct thread *curr, int fd) {
	struct list_elem *ptr;
	struct list *list = &curr->file_list;

	for(ptr = list_begin(list); ptr != list_end(list); ptr = list_next(ptr)){
		if(list_entry(ptr, struct fd_info, elem)-> fd == fd){
			return list_entry(ptr, struct fd_info, elem);
		}
	}

	return NULL;
}