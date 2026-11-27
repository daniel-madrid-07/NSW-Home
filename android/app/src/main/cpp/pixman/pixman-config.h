/* pixman's build configuration for Android, written by hand because pixman
 * only ships a Meson build. Everything here holds for the two ABIs the app is
 * built for (arm64-v8a, x86_64) on bionic, API 24 and later. */
#define PACKAGE "pixman"
#define HAVE_PTHREADS 1
#define HAVE_SIGACTION 1
#define HAVE_ALARM 1
#define HAVE_MPROTECT 1
#define HAVE_GETPAGESIZE 1
#define HAVE_MMAP 1
#define HAVE_GETTIMEOFDAY 1
#define HAVE_POSIX_MEMALIGN 1
#define HAVE_SYS_MMAN_H 1
#define HAVE_FENV_H 1
#define HAVE_UNISTD_H 1
#define HAVE_FEDIVBYZERO 1
#define TLS __thread
#define TOOLCHAIN_SUPPORTS_ATTRIBUTE_CONSTRUCTOR 1
#define TOOLCHAIN_SUPPORTS_ATTRIBUTE_DESTRUCTOR 1
#define HAVE_BUILTIN_CLZ 1
#define HAVE_GCC_VECTOR_EXTENSIONS 1
#define SIZEOF_LONG 8
#if defined(__aarch64__)
/* The NEON paths are assembler files. Clang's assembler has no .func
 * directive, so ASM_HAVE_FUNC_DIRECTIVE stays undefined. */
#define USE_ARM_A64_NEON 1
#elif defined(__x86_64__)
#define USE_SSE2 1
#define USE_SSSE3 1
#define USE_GCC_INLINE_ASM 1
#endif
