// Minimal Darwin/iOS declarations. There is no iOS SDK in the build container, so everything the
// mod calls is declared here with its real Darwin ABI and resolved against the libraries the game
// already loads (see mod/stubs/*.tbd). Keep layouts exact: they are passed to the kernel.
#pragma once

typedef unsigned long size_t;
typedef long ssize_t;
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef signed int int32_t;
typedef signed long long int64_t;
typedef unsigned long uintptr_t;
typedef long intptr_t;
typedef int bool;
#define true 1
#define false 0
#define NULL ((void *)0)

// ---- libc ----
typedef struct FILE FILE;
char *getenv(const char *);
FILE *fopen(const char *, const char *);
int fclose(FILE *);
size_t fwrite(const void *, size_t, size_t, FILE *);
size_t fread(void *, size_t, size_t, FILE *);
int fflush(FILE *);
int snprintf(char *, size_t, const char *, ...);
typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_end(ap) __builtin_va_end(ap)
int vsnprintf(char *, size_t, const char *, va_list);
void *malloc(size_t);
void *calloc(size_t, size_t);
void *realloc(void *, size_t);
void free(void *);
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
void *memset(void *, int, size_t);
int memcmp(const void *, const void *, size_t);
size_t strlen(const char *);
int strcmp(const char *, const char *);
int strncmp(const char *, const char *, size_t);
char *strstr(const char *, const char *);
char *strchr(const char *, int);
char *strdup(const char *);
long strtol(const char *, char **, int);
typedef long time_t;
time_t time(time_t *);
int mkdir(const char *, uint16_t);
int usleep(uint32_t);
int gettimeofday(void *, void *);
void arc4random_buf(void *, size_t);

// ---- dyld / dlfcn ----
void *dlopen(const char *, int);
void *dlsym(void *, const char *);
char *dlerror(void);
#define RTLD_LAZY 0x1
#define RTLD_NOW 0x2
#define RTLD_DEFAULT ((void *)-2)
uint32_t _dyld_image_count(void);
const struct mach_header_64 *_dyld_get_image_header(uint32_t);
intptr_t _dyld_get_image_vmaddr_slide(uint32_t);
const char *_dyld_get_image_name(uint32_t);

// ---- Mach-O (64-bit) ----
struct mach_header_64 { uint32_t magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved; };
struct load_command { uint32_t cmd, cmdsize; };
struct segment_command_64 {
  uint32_t cmd, cmdsize; char segname[16];
  uint64_t vmaddr, vmsize, fileoff, filesize; int32_t maxprot, initprot; uint32_t nsects, flags;
};
struct section_64 {
  char sectname[16], segname[16]; uint64_t addr, size;
  uint32_t offset, align, reloff, nreloc, flags, reserved1, reserved2, reserved3;
};
struct symtab_command { uint32_t cmd, cmdsize, symoff, nsyms, stroff, strsize; };
struct dysymtab_command {
  uint32_t cmd, cmdsize, ilocalsym, nlocalsym, iextdefsym, nextdefsym, iundefsym, nundefsym,
      tocoff, ntoc, modtaboff, nmodtab, extrefsymoff, nextrefsyms, indirectsymoff, nindirectsyms,
      extreloff, nextrel, locreloff, nlocrel;
};
struct nlist_64 { uint32_t n_strx; uint8_t n_type, n_sect; uint16_t n_desc; uint64_t n_value; };
#define LC_SEGMENT_64 0x19
#define LC_SYMTAB 0x2
#define LC_DYSYMTAB 0xb
#define SECTION_TYPE 0x000000ff
#define S_NON_LAZY_SYMBOL_POINTERS 0x6
#define S_LAZY_SYMBOL_POINTERS 0x7
#define INDIRECT_SYMBOL_LOCAL 0x80000000
#define INDIRECT_SYMBOL_ABS 0x40000000

// ---- memory protection ----
typedef uint32_t mach_port_t;
extern mach_port_t mach_task_self_;
int vm_protect(mach_port_t, uintptr_t, size_t, int, int);
// Reads that fail (KERN_INVALID_ADDRESS) instead of faulting; 0 = KERN_SUCCESS.
int mach_vm_read_overwrite(mach_port_t, uint64_t addr, uint64_t size, uint64_t data, uint64_t *outsize);
int mprotect(void *, size_t, int);
#define PROT_READ 1
#define PROT_WRITE 2
#define VM_PROT_COPY 0x10

// ---- BSD sockets (Darwin layout: sockaddr has a length byte) ----
typedef uint32_t socklen_t;
typedef uint8_t sa_family_t;
struct sockaddr { uint8_t sa_len; sa_family_t sa_family; char sa_data[14]; };
struct in_addr { uint32_t s_addr; };
struct sockaddr_in { uint8_t sin_len; sa_family_t sin_family; uint16_t sin_port; struct in_addr sin_addr; char sin_zero[8]; };
#define AF_INET 2
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOL_SOCKET 0xffff
#define SO_REUSEADDR 0x0004
#define SO_NOSIGPIPE 0x1022
#define SO_BROADCAST 0x0020
#define SO_RCVTIMEO 0x1006
#define SO_SNDTIMEO 0x1005
int *__error(void);
int socket(int, int, int);
int connect(int, const struct sockaddr *, socklen_t);
ssize_t sendto(int, const void *, size_t, int, const struct sockaddr *, socklen_t);
ssize_t send(int, const void *, size_t, int);
ssize_t recv(int, void *, size_t, int);
int bind(int, const struct sockaddr *, socklen_t);
int listen(int, int);
int accept(int, struct sockaddr *, socklen_t *);
int setsockopt(int, int, int, const void *, socklen_t);
int close(int);
int inet_pton(int, const char *, void *);
const char *inet_ntop(int, const void *, char *, socklen_t);
ssize_t recvfrom(int, void *, size_t, int, struct sockaddr *, socklen_t *);
struct ifaddrs {
  struct ifaddrs *ifa_next; char *ifa_name; unsigned int ifa_flags;
  struct sockaddr *ifa_addr, *ifa_netmask, *ifa_dstaddr; void *ifa_data;
};
int getifaddrs(struct ifaddrs **);
void freeifaddrs(struct ifaddrs *);
static inline uint16_t bo_htons(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }

// ---- pthreads ----
typedef struct { long sig; char opaque[56]; } pthread_mutex_t;
typedef struct _opaque_pthread_t *pthread_t;
int pthread_create(pthread_t *, const void *, void *(*)(void *), void *);
int pthread_detach(pthread_t);
int pthread_mutex_init(pthread_mutex_t *, const void *);
int pthread_mutex_lock(pthread_mutex_t *);
int pthread_mutex_unlock(pthread_mutex_t *);

// ---- CommonCrypto ----
typedef int32_t CCCryptorStatus;
CCCryptorStatus CCCrypt(uint32_t op, uint32_t alg, uint32_t options, const void *key, size_t keyLength,
                        const void *iv, const void *dataIn, size_t dataInLength, void *dataOut,
                        size_t dataOutAvailable, size_t *dataOutMoved);
#define kCCEncrypt 0
#define kCCDecrypt 1
#define kCCAlgorithmAES 0

// ---- GCD ----
typedef void *dispatch_queue_t;
extern struct dispatch_queue_s _dispatch_main_q;
void dispatch_async_f(dispatch_queue_t, void *, void (*)(void *));
dispatch_queue_t dispatch_queue_create(const char *, void *);
uint64_t dispatch_time(uint64_t, int64_t);
void dispatch_after_f(uint64_t, dispatch_queue_t, void *, void (*)(void *));
#define DISPATCH_TIME_NOW 0ull
#define NSEC_PER_SEC 1000000000ll
#define MAIN_QUEUE ((dispatch_queue_t)&_dispatch_main_q)

// ---- Objective-C runtime ----
typedef struct objc_class *Class;
typedef struct objc_object *id;
typedef struct objc_selector *SEL;
typedef struct objc_method *Method;
typedef id (*IMP)(id, SEL, ...);
#define nil ((id)0)
#define YES ((signed char)1)
#define NO ((signed char)0)
Class objc_getClass(const char *);
SEL sel_registerName(const char *);
void objc_msgSend(void);
Method class_getInstanceMethod(Class, SEL);
Method class_getClassMethod(Class, SEL);
IMP method_getImplementation(Method);
IMP method_setImplementation(Method, IMP);
const char *method_getTypeEncoding(Method);
signed char class_addMethod(Class, SEL, IMP, const char *);
Class objc_allocateClassPair(Class, const char *, size_t);
void objc_registerClassPair(Class);
Class object_getClass(id);
void *objc_autoreleasePoolPush(void);
void objc_autoreleasePoolPop(void *);

// Typed objc_msgSend helpers (arm64: objc_msgSend must be called through the right prototype).
#define MSG(ret, ...) ((ret(*)(__VA_ARGS__))objc_msgSend)
#define SELN(s) sel_registerName(s)
#define CLS(c) ((id)objc_getClass(c))

// ---- Foundation ----
void NSLog(id fmt, ...);
