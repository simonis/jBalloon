#include <jni.h>

#ifndef _GNU_SOURCE
  #define _GNU_SOURCE
#endif
#include <assert.h>
#include <dlfcn.h>
#include <elf.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

// The default page size.
static size_t PAGE_SIZE = -1;

// This struct is only used to get heap size and base address out of libjvm.so.
struct VMStructEntry {
    const char* typeName;
    const char* fieldName;
    const char* typeString;
    int32_t  isStatic;
    uint64_t offset;
    void* address;
};

// For sanity checks
static uint64_t jBalloon_MAGIC = 0x6a42616c6c6f6f6eLL; // ASCII values for 'jBalloon'

static jlong* heapBase = nullptr;
static size_t heapSizeBytes = -1;

static int javaVersion;
static const char* gc;
static jboolean useCompressedClassPointers;
static jboolean compactObjectHeadersVM;
static jboolean useCompactObjectHeaders;
static jint objectHeaderSize;
static jlong regionSize; // Currently only implemented for G1 GC
// The offset of the actual long array relative to the '[J' object's start address.
static jint longArrayOffset;
// The offset of the start of first system memory page relative to 'longArrayOffset'.
// This only works for humongous '[J' objects which start at a region (i.e. system page) boundary.
static jint longArrayPageOffset;

static uint64_t* regionBitMap;

static jlong mincoreHeapSize();
static jlong rssHeapSize();

// This struct is used to get markWord::{lock_bits, lock_shift, klass_shift} out of libjvm.so.
struct VMLongConstantEntry {
    const char* name;
    uint64_t value;
};

static void tag_balloon_region(uintptr_t arrayOop) {
  uint64_t region_nr = (arrayOop - (uintptr_t)heapBase) / regionSize;
  regionBitMap[region_nr / 64] |= (uint64_t)1 << region_nr % 64;
}

static void untag_balloon_region(uintptr_t arrayOop) {
  uint64_t region_nr = (arrayOop - (uintptr_t)heapBase) / regionSize;
  regionBitMap[region_nr / 64] &= ~((uint64_t)1 << region_nr % 64);
}

static bool is_balloon_region_tagged(uintptr_t arrayOop) {
  uint64_t region_nr = (arrayOop - (uintptr_t)heapBase) / regionSize;
  return (regionBitMap[region_nr / 64] & ((uint64_t)1 << region_nr % 64));
}

enum LogLevel {
  UNINITIALIZED = -1,
  TRACE,
  DEBUG,
  INFO,
  WARNING,
  ERROR,
  OFF
};

static LogLevel logLevel = UNINITIALIZED;

static void init_log_level() {
  char* level;
  if ((level = getenv("LOG")) != nullptr) {
    if (!strcasecmp("ERROR", level)) {
      logLevel = ERROR;
    } else if (!strcasecmp("WARNING", level)) {
      logLevel = WARNING;
    } else if (!strcasecmp("INFO", level)) {
      logLevel = INFO;
    } else if (!strcasecmp("DEBUG", level)) {
      logLevel = DEBUG;
    } else if (!strcasecmp("TRACE", level)) {
      logLevel = TRACE;
    } else {
      logLevel = OFF;
    }
    return;
  }
  logLevel = OFF;
}

// Macro to avoid argument evaluation for log(..) because it might be expensive.
#define LOG(level, fmt, ...) \
  do { \
    if (level >= logLevel) { \
      log(level, fmt, ##__VA_ARGS__); \
    } \
  } while (0);

static int log(LogLevel l, const char* format, ...) {
  if (logLevel == UNINITIALIZED) {
    init_log_level();
  }
  if (logLevel == OFF) {
    return 0;
  }
  if (l >= logLevel) {
    va_list args;
    va_start(args, format);
    // Write to stderr to synchronize with the Java side, because the ConsoleHandler
    // in the java.util.logging.Logger writes to System.err by default as well.
    int ret = vfprintf(stderr, format, args);
    va_end(args);
    return ret;
  }
  return 0;
}

// If the Java heap object at 'src' is a jBalloon object, "move" it to the
// destination address 'dest'. For jBalloon objects, "moving" means to copy
// the object header and MADV_DONTNEED the remaining part of the object at
// the destination address. This ensures that memory which was released when
// a jBalloon object was inflated, will remain released, even if that jBalloon
// object is moved by the GC to a new location.
// If the object at 'src' is not a jBalloon object, this function will simply
// return false and do nothing.
// Note: 'n' is the size of the object in bytes!
static bool move_if_jBalloon(void* dst, const void* src, size_t size) {
  // Check if it is a jBalloon object.
  if (is_balloon_region_tagged((uintptr_t)src)) {
    // For sanity, check that the first array entry contains the correct magic number.
    if (*(uint64_t*)((uint64_t)src + longArrayOffset) != jBalloon_MAGIC) {
      log(ERROR, "JBalloon::move_if_jBalloon() -> can't find jBalloon_MAGIC in an array tagged as balloon");
      return false;
    }
    log(DEBUG, "JBalloon::move_if_jBalloon() -> moving balloon from %p to %p\n", src, dst);
    LOG(TRACE, "JBalloon::move_if_jBalloon() -> (reserved/mincore/RSS) = (%dkb / %dkb / %dkb)\n", heapSizeBytes/1024, mincoreHeapSize()/1024, rssHeapSize()/1024);
    // Only copy the part before the first system page boundary such that we don't
    // unintentionally disclose any data at the 'dest' location.
    memmove(dst, src, longArrayOffset + longArrayPageOffset);
    // madvise the balloon at the new location
    char* address = (char*)dst + longArrayOffset + longArrayPageOffset;
    jlong len = ((size - longArrayOffset - longArrayPageOffset) / PAGE_SIZE) * PAGE_SIZE;
    if (madvise(address, len, MADV_DONTNEED) != 0) {
      log(ERROR, "JBalloon::move_if_jBalloon() -> madv(MADV_DONTNEED returned \"%s\"\n", strerror(errno));
    }
    log(DEBUG, "JBalloon::move_if_jBalloon() -> madvise(%p, %d)\n", address, len);
    // And update the balloon tags
    tag_balloon_region((uintptr_t)dst);   // Tag at the new location
    untag_balloon_region((uintptr_t)src); // Clear tag for old region
    LOG(TRACE, "JBalloon::move_if_jBalloon() -> (reserved/mincore/RSS) = (%dkb / %dkb / %dkb)\n", heapSizeBytes/1024, mincoreHeapSize()/1024, rssHeapSize()/1024);

    return true;
  }
  return false;
}

// This function takes a handle to a loaded shared library and the name of an undefined
// function in that shared library and returns the GOT (Global Offset Table) entry
// (i.e. the absolute address of the entry) for that undefined function.
static uintptr_t get_got_address(void* dl_handle, const char* fun_name) {
  struct link_map* map = nullptr;
  if (dlinfo(dl_handle, RTLD_DI_LINKMAP, &map) != 0) return 0;

  Elf64_Sym* symtab = nullptr;   // ELF symbol table
  char* strtab = nullptr;        // ELF string table
  Elf64_Rela* jmprel = nullptr;  // ELF relocation entries for the procedure linkage table (PLT)
  size_t plt_rel_size = 0;    // ELF size of the relocation entries for the PLT

  for (Elf64_Dyn* dyn = map->l_ld; dyn->d_tag != DT_NULL; dyn++) {
    switch (dyn->d_tag) {
      case DT_SYMTAB:   symtab = (Elf64_Sym*)dyn->d_un.d_ptr; break;
      case DT_STRTAB:   strtab = (char*)dyn->d_un.d_ptr; break;
      case DT_JMPREL:   jmprel = (Elf64_Rela*)dyn->d_un.d_ptr; break;
      case DT_PLTRELSZ: plt_rel_size = dyn->d_un.d_val; break;
    }
  }

  if (!symtab || !strtab || !jmprel || plt_rel_size == 0) return 0;

  size_t rel_count = plt_rel_size / sizeof(Elf64_Rela);
  for (size_t i = 0; i < rel_count; i++) {
    Elf64_Rela* rel = &jmprel[i];
    uint32_t sym_index = ELF64_R_SYM(rel->r_info);
    if (strcmp(&strtab[symtab[sym_index].st_name], fun_name) == 0) {
      return (uintptr_t)(map->l_addr + rel->r_offset);
    }
  }
  return 0;
}

// Helper function to return the address of a local (i.e. un-exported) symbol from a shared library.
// If the the symbol was found and the third argument 'function_size' is not nullptr, it will contain
// the size of the symbol (i.e. the function size for a function symbol) on return.
static void* find_local_symbol(void* dl_handle, const char* symbol_name, size_t* function_size) {
  struct link_map* map = nullptr;

  // Retrieve internal linker information to get the absolute file path of the .so
  if (dlinfo(dl_handle, RTLD_DI_LINKMAP, &map) != 0 || !map->l_name) {
    return nullptr;
  }

  // Open the shared library file directly
  int fd = open(map->l_name, O_RDONLY);
  if (fd < 0) {
    return nullptr;
  }

  // Get the file size to map the entire binary into memory for rapid parsing
  off_t size = lseek(fd, 0, SEEK_END);
  char* file_base = (char*)mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd); // File descriptor can be closed safely right after mmap

  if (file_base == MAP_FAILED) {
    return nullptr;
  }

  // Map ELF header and section headers from the file raw data
  Elf64_Ehdr* ehdr = (Elf64_Ehdr*)file_base;
  Elf64_Shdr* shdr = (Elf64_Shdr*)(file_base + ehdr->e_shoff);

  // Get the section header string table to resolve section names (.symtab, .strtab, etc.)
  char* shstrtab = file_base + shdr[ehdr->e_shstrndx].sh_offset;

  Elf64_Sym* symtab = nullptr;
  char* strtab = nullptr;
  size_t sym_count = 0;

  // Iterate through all section headers to locate .symtab and .strtab
  for (int i = 0; i < ehdr->e_shnum; i++) {
    const char* sec_name = shstrtab + shdr[i].sh_name;

    if (strcmp(sec_name, ".symtab") == 0) {
      // Found the regular symbol table (not loaded into RAM by the OS)
      symtab = (Elf64_Sym*)(file_base + shdr[i].sh_offset);
      sym_count = shdr[i].sh_size / sizeof(Elf64_Sym);
    } else if (strcmp(sec_name, ".strtab") == 0) {
      // Found the corresponding string table for local symbol names
      strtab = file_base + shdr[i].sh_offset;
    }
  }

  void* result_addr = nullptr;

  // Search the symbol table for the requested local function name
  if (symtab && strtab) {
    for (size_t i = 0; i < sym_count; i++) {
      const char* current_name = strtab + symtab[i].st_name;

      if (strcmp(current_name, symbol_name) == 0) {
        // Runtime Address = RAM Base Address of the library + static file offset
        result_addr = (void*)(map->l_addr + symtab[i].st_value);
        // Read the exact function length in bytes from the ELF symbol metadata
        if (function_size != nullptr) {
          *function_size = (size_t)symtab[i].st_size;
        }
        break;
      }
    }
  }

  // Clean up the manual memory mapping
  munmap(file_base, size);
  return result_addr;
}

// Helper function to check if a specific memory address falls into any executable segment of a shared library
static bool is_address_in_executable_segment(void* dl_handle, uintptr_t target_addr) {
  struct link_map* map = nullptr;
  if (dlinfo(dl_handle, RTLD_DI_LINKMAP, &map) != 0) {
    return false;
  }

  // Locate the ELF Executive Header at the base address of the library
  Elf64_Ehdr* ehdr = (Elf64_Ehdr*)map->l_addr;

  // Find the Program Header Table using the offset from the ELF header
  Elf64_Phdr* phdr_table = (Elf64_Phdr*)(map->l_addr + ehdr->e_phoff);

  // Iterate through all program headers to inspect mapped memory segments
  for (Elf64_Half i = 0; i < ehdr->e_phnum; i++) {
    Elf64_Phdr* phdr = &phdr_table[i];

    // We only care about loadable segments (PT_LOAD) that have executable permissions (PF_X)
    if (phdr->p_type == PT_LOAD && (phdr->p_flags & PF_X)) {
      // Calculate the absolute start and end address of this executable segment in RAM
      uintptr_t segment_start = map->l_addr + phdr->p_vaddr;
      uintptr_t segment_end = segment_start + phdr->p_memsz;

      // Check if our target address is within the boundaries of this executable area
      if (target_addr >= segment_start && target_addr < segment_end) {
        return true; // Address is safe to read
      }
    }
  }

  return false; // Address is outside of any executable segment
}

#if defined(__x86_64__)

#define INSTR_INC 1

// This is jBalloons's replacement for the transitive call to 'memmove()' in
// 'G1FullGCCompactTask::compact_humongous_obj()' which is reached through the
// call chain: 'copy_object_to_new_location()' -> 'Copy::aligned_conjoint_words()'
// -> 'pd_aligned_conjoint_words()' -> 'pd_conjoint_words()' -> 'memmove()'.
// Note: on x86_64, pd_conjoint_words() is implemented to call 'memmove()', after
//       doing some argument shuffling, because 'pd_conjoint_words()' takes 'src'
//       as first, 'dest' as second and the size in words (i.e. 8 bytes) as third
//       argument.
extern "C" void* jBalloon_memmove(void* dest, const void* src, size_t n) {
  // jBalloon objects are always humongous, so we can do this first, cheap check.
  if (n >= (regionSize / 2) && move_if_jBalloon(dest, src, n)) {
    return dest;
  }
  // Regular (i.e. non-jBalloon) object
  return memmove(dest, src, n);
}

// Checks if a x86_64 CALL instruction (i.e. '0xe8') in a shared library calls
// the function 'fun_name' (which is external to that library) through the
// procedure linkage table (PLT) and the GOT (Global Offset Table).
static bool verify_is_got_func_call(void* dl_handle, uintptr_t call_addr, const char* fun_name) {
  // On x86, a call is '0x8e' plus a 32-bit (i.e. four bytes) offset.
  if (*(char*)call_addr != (char)0xe8) {
    return false;
  }
  int32_t relative_offset;
  memcpy(&relative_offset, (void*)(call_addr + 1), 4);

  // If the target of the call is a function which is external to the shared library
  // then 'relative_offset' is an instruction pointer relative offset based on the address
  // after the CALL instruction which points to a procedure linkage table (PLT) entry.
  uintptr_t plt_entry_addr = call_addr + 5 /* CALL instruction size on x64_64 */ + relative_offset;

  // Make sure the potential address is inside an executable segment of 'dl_handle'
  if (!is_address_in_executable_segment(dl_handle, plt_entry_addr)) {
    return false; // Not a valid, IP-relative call
  }
  // If the call target is indeed a PLT entry it should contain an instruction pointer
  // relative JMP instruction (i.e. "jmp *offset(%rip)") of the form:
  // '0xff 0x25' plus a 32-bit (i.e. four bytes) offset.
  uint8_t* plt_code = (uint8_t*)plt_entry_addr;
  if (plt_code[0] != 0xFF || plt_code[1] != 0x25) {
    return false; // This doesn't seem to be a IP-relative JMP instruction
  }

  // Get the IP-relative offset for the JMP instruction (bytes 2 to 5 on x86_64)
  memcpy(&relative_offset, &plt_code[2], 4);

  // Compute the absolute address of the JMP target in the GOT (Global Offset Table).
  uintptr_t got_entry_addr = plt_entry_addr + 6 /* JMP instruction ssize on x86_64 */ + relative_offset;

  // Now also get the GOT entry address for 'fun_name'.
  uintptr_t fun_got_entry_addr = get_got_address(dl_handle, fun_name);

  return (fun_got_entry_addr != 0 && got_entry_addr == fun_got_entry_addr);
}

// Checks if a x86_64 CALL instruction ('0xe8') in a shared library calls the
// library-internal function 'function'.
static bool verify_is_func_call(uintptr_t call_addr, void* function) {
  // On x86, a call is '0x8e' plus a 32-bit (i.e. four bytes) offset.
  if (*(char*)call_addr != (char)0xe8) {
    return false;
  }
  int32_t relative_offset;
  memcpy(&relative_offset, (void*)(call_addr + 1), 4);
  if (call_addr + 5 + relative_offset == (uintptr_t)function) {
    return true;
  } else {
    return false;
  }
}

static bool patch_call_instr(uintptr_t call_addr, uintptr_t target, uintptr_t trampoline_addr) {
  log(INFO, "JBalloon::patch_call_instr() -> found call to 'memmove()' at %p\n", call_addr);
  uintptr_t patch_addr = call_addr + 1;
  intptr_t patch_target_offset = target - (patch_addr + 4 /* the offset is relative to the IP after the CALL instruction*/);
  if (llabs(patch_target_offset) >= (1L << 31) - 1) {
    log(INFO, "JBalloon::patch_call_instr() -> patching 'memmove()' requires a trampoline\n");

    patch_target_offset = trampoline_addr - (patch_addr + 4);
    if (llabs(patch_target_offset) >= (1L << 31) - 1) {
      log(INFO, "JBalloon::patch_call_instr() -> trampoline address %p too far away from patch address %p (%d)\n", trampoline_addr, patch_addr, patch_target_offset);
      return false;
    }

    // Create the trampoline. The trampoline consists of an indirect jump instruction (6 bytes) and
    // an 8-byte constant in the memory right after the jump instruction (i.e. 6+8=14 bytes in total),
    // so we must account for the fact that the trampoline might span two system pages.
    int nr_of_pages = ((trampoline_addr + 6 + 8) / PAGE_SIZE) - (trampoline_addr / PAGE_SIZE) + 1;
    mprotect((void*)(trampoline_addr & ~(PAGE_SIZE - 1)), nr_of_pages * PAGE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC);
    uint32_t* trampoline = (uint32_t*)trampoline_addr;
    // JMP *0x0(%rip) : ff 25 00 00 00 00        ; JMP opcode (ff 25) plus 4-byte offset relative to current IP
    //                : 00 00 00 00 00 00 00 00  ; 8-byte target address following directly in memory
    trampoline[0] = 0x000025ff;
    trampoline[1] = 0x00000000;
    // The actual 64-bit, absolute JMP target address
    memcpy((void*)(trampoline_addr + 6), &target, sizeof(target));
    mprotect((void*)(trampoline_addr & ~(PAGE_SIZE - 1)), nr_of_pages * PAGE_SIZE, PROT_READ | PROT_EXEC);
  } else {
    log(INFO, "JBalloon::patch_call_instr() -> call to 'memmove()' can be patched directly\n");
  }

  // On x64, even a 32-bit part of an instruction can cross page boundaries
  int nr_of_pages = ((patch_addr + 4) / PAGE_SIZE) - (patch_addr / PAGE_SIZE) + 1;
  mprotect((void*)(patch_addr & ~(PAGE_SIZE - 1)), nr_of_pages * PAGE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC);
  *(int32_t*)patch_addr = patch_target_offset;
  mprotect((void*)(patch_addr & ~(PAGE_SIZE - 1)), nr_of_pages * PAGE_SIZE, PROT_READ | PROT_EXEC);

  return true;
}

#elif defined(__aarch64__)

#define INSTR_INC 4

typedef void (*Copy_conjoint_words_fun)(const uintptr_t* from, uintptr_t* to, size_t count);
Copy_conjoint_words_fun copy_conjoint_words_addr = nullptr;

// This is jBalloons's replacement for the transitive call to '_Copy_conjoint_words()'
// in 'G1FullGCCompactTask::compact_humongous_obj()' which is reached through the
// call chain: 'copy_object_to_new_location()' -> 'Copy::aligned_conjoint_words()'
// -> 'pd_aligned_conjoint_words()' -> 'pd_conjoint_words()' -> '_Copy_conjoint_words()'.
// Note: on aarch64, 'pd_conjoint_words()' is implemented to call '_Copy_conjoint_words()',
//       for objects larger than 8 words (i.e. 64 bytes) which is fine for us, because we
//       are only interested in humongous objects anyway. '_Copy_conjoint_words()' has the
//       same signature like 'pd_conjoint_words()' (i.e. 'src', 'dest' and size in
//       8-byte words) so we have to manually adjust the parameters before calling
//       'move_if_jBalloon()' which takes 'dest' first, then 'src' and the size in bytes.
extern "C" void* jBalloon_memmove(const void* src, void* dest, size_t n) {
  // For '_Copy_conjoint_words()' the size 'n' is given in units of 'HeapWordSize'.
  n = n * 8 /* HotSpot's 'HeapWordSize' which is always 8 on 64-bit architectures */;
  // jBalloon objects are always humongous, so we can do this first, cheap check.
  if (n >= (regionSize / 2) && move_if_jBalloon(dest, src, n)) {
    return dest;
  }
  // Regular (i.e. non-jBalloon) object
  copy_conjoint_words_addr((const uintptr_t*)src, (uintptr_t*)dest, n / 8);
  return dest;
}

// Checks if a aarch64 BL instruction ('0b100101') in a shared library calls the
// library-internal function 'function'.
static bool verify_is_func_call(uintptr_t call_addr, void* function) {
  // On aarch64, a BL call is '0b100101' (6 bit) and a 26-bit, signed offset which, left-shifted
  // by two and added to the current instruction pointer, gives the target address of the call.
  int32_t relative_offset, instr;
  memcpy(&relative_offset, (void*)call_addr, 4);
  instr =           (relative_offset & 0b10010100000000000000000000000000);
  // First we sign-extend the offset from 26 to 32 bit, then we left-shift by two (i.e. multiply by four)
  relative_offset = (((relative_offset & 0b00000011111111111111111111111111) << 6) >> 6) << 2;

  if (instr == 0b10010100000000000000000000000000 && call_addr + relative_offset == (uintptr_t)function) {
    return true;
  } else {
    return false;
  }
}

void flush_caches(char* addr, size_t size) {
    // 1. Flush Data Cache to Point of Unification
    asm volatile("dc cvau, %0" :: "r"(addr) : "memory");
    // 2. Wait for Data Cache clean to complete globally
    asm volatile("dsb ish" ::: "memory");
    // 3. Invalidate Instruction Cache to Point of Unification
    asm volatile("ic ivau, %0" :: "r"(addr) : "memory");
    // 4. Wait for Instruction Cache invalidation to complete globally
    asm volatile("dsb ish" ::: "memory");
    // 5. Flush the CPU pipeline execution fetch buffers
    asm volatile("isb" ::: "memory");
}


static bool patch_call_instr(uintptr_t patch_addr, uintptr_t  target, uintptr_t trampoline_addr) {
  log(INFO, "JBalloon::patch_call_instr() -> found call to '_Copy_conjoint_words()' at %p\n", patch_addr);

  intptr_t patch_target_offset = target - patch_addr /* the offset is relative to the IP at the BL instruction*/;
  if (llabs(patch_target_offset) >= (1L << 26) - 1) {
    log(INFO, "JBalloon::patch_call_instr() -> patching '_Copy_conjoint_words()' requires a trampoline\n");

    patch_target_offset = trampoline_addr - patch_addr;
    if (llabs(patch_target_offset) >= (1L << 26) - 1) {
      log(INFO, "JBalloon::patch_call_instr() -> trampoline address %p too far away from patch address %p (%d)\n", trampoline_addr, patch_addr, patch_target_offset);
      return false;
    }

    // Create the trampoline. The trampoline consists of 5 instructions (i.e. 5*4=20 bytes),
    // so we must account for the fact that the trampoline might span two system pages.
    int nr_of_pages = ((trampoline_addr + 5*4) / PAGE_SIZE) - (trampoline_addr / PAGE_SIZE) + 1;
    mprotect((void*)(trampoline_addr & ~(PAGE_SIZE - 1)), nr_of_pages * PAGE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC);
    uint32_t* trampoline = (uint32_t*)trampoline_addr;
    // We use the caller-save argument register 'x4' for loading the target address.
    // movz x4, #(bits 0-15)
    trampoline[0] = 0xD2800004 | (((target >> 0)  & 0xFFFF) << 5);
    // movk x4, #(bits 16-31), lsl #16
    trampoline[1] = 0xF2A00004 | (((target >> 16) & 0xFFFF) << 5);
    // movk x4, #(bits 32-47), lsl #32
    trampoline[2] = 0xF2C00004 | (((target >> 32) & 0xFFFF) << 5);
    // movk x4, #(bits 48-63), lsl #48
    trampoline[3] = 0xF2E00004 | (((target >> 48) & 0xFFFF) << 5);
    // br x4 (Branch to the target address, preserving X0-X3 and X30)
    trampoline[4] = 0xD61F0080;
    mprotect((void*)(trampoline_addr & ~(PAGE_SIZE - 1)), nr_of_pages * PAGE_SIZE, PROT_READ | PROT_EXEC);

    // Flush cache for the newly generated instructions
    flush_caches((char*)trampoline_addr, 5 * sizeof(uint32_t));

  } else {
    log(INFO, "JBalloon::patch_call_instr() -> call to '_Copy_conjoint_words()' can be patched directly\n");
  }

  mprotect((void*)(patch_addr & ~(PAGE_SIZE - 1)), PAGE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC);
  *(int32_t*)patch_addr = 0b10010100000000000000000000000000 | ((((int32_t)patch_target_offset) >> 2) & 0b00000011111111111111111111111111);
  mprotect((void*)(patch_addr & ~(PAGE_SIZE - 1)), PAGE_SIZE, PROT_READ | PROT_EXEC);
  flush_caches((char*)patch_addr, sizeof(int32_t));

  return true;
}

#else

#error "Unimplemented CPU architecture"

#endif

static bool can_use_compact_humongous_obj_patching() {

  void* libjvm_handle = dlopen("libjvm.so", RTLD_NOLOAD | RTLD_LAZY);
  if (libjvm_handle == nullptr) {
    log(ERROR, "JBalloon::can_use_compact_humongous_obj_patching() -> libjvm.so must be loaded at this point.\n");
    return false;
  }

  // Locate 'G1FullGCCompactTask::compact_humongous_obj(HeapRegion*)' in libjvm.so and get its size
  size_t compact_humongous_obj_size = 0;
  const char* copy_object_to_new_location_name = "_ZN19G1FullGCCompactTask27copy_object_to_new_locationEP7oopDesc";
  const char* compact_humongous_obj_name = "_ZN19G1FullGCCompactTask21compact_humongous_objEP10HeapRegion";
  if (javaVersion > 21) {
    // 'HeapRegion' was renamed to 'G1HeapRegion' in JDK 2X
    compact_humongous_obj_name = "_ZN19G1FullGCCompactTask21compact_humongous_objEP12G1HeapRegion";
  }
  // jBalloon currently only works for G1 GC so we can "misuse" the 'SerialHeap::SerialHeap()'
  // constructor code as memory region for the trampolines, in case we need them for patching.
  const char* serialHeap_constr_name = "_ZN10SerialHeapC1Ev";
  size_t serialHeap_constr_size = 0;
  char* serialHeap_constr_addr = (char*)find_local_symbol(libjvm_handle, serialHeap_constr_name, &serialHeap_constr_size);
  if (serialHeap_constr_addr != nullptr && serialHeap_constr_size > 20 /* Must be enough to hold the trampoline code */) {
    log(INFO, "JBalloon::can_use_compact_humongous_obj_patching() -> located 'SerialHeap::SerialHeap()' at %p (size = %d)\n", serialHeap_constr_addr, serialHeap_constr_size);
  } else {
    log(ERROR, "JBalloon::can_use_compact_humongous_obj_patching() -> can't find 'SerialHeap::SerialHeap()' in libjvm.so (dlerror=%s)\n", dlerror());
    return false;
  }

#if defined(__aarch64__)
  // '_Copy_conjoint_words()' is a generated stub which is used on aarch64 instead of 'memmove()'
  copy_conjoint_words_addr = (Copy_conjoint_words_fun)find_local_symbol(libjvm_handle, "_Copy_conjoint_words", nullptr);
  if (copy_conjoint_words_addr != nullptr) {
    log(INFO, "JBalloon::can_use_compact_humongous_obj_patching() -> located '_Copy_conjoint_words()' at %p\n", copy_conjoint_words_addr);
  } else {
    log(ERROR, "JBalloon::can_use_compact_humongous_obj_patching() -> can't find '_Copy_conjoint_words()' in libjvm.so (dlerror=%s)\n", dlerror());
    return false;
  }
#endif
  char* compact_humongous_obj_addr = (char*)find_local_symbol(libjvm_handle, compact_humongous_obj_name, &compact_humongous_obj_size);
  if (compact_humongous_obj_addr != nullptr && compact_humongous_obj_size != 0) {
    log(INFO, "JBalloon::can_use_compact_humongous_obj_patching() -> located 'G1FullGCCompactTask::compact_humongous_obj(HeapRegion*)' at %p (size = %d)\n",
              compact_humongous_obj_addr, compact_humongous_obj_size);
    uintptr_t patch_addr = 0;
    // Now try to find a call to 'memmove()'/'_Copy_conjoint_words()' in 'G1FullGCCompactTask::compact_humongous_obj()'
    for (char* start = compact_humongous_obj_addr; start < compact_humongous_obj_addr + compact_humongous_obj_size; start += INSTR_INC) {
#if defined(__x86_64)
      if (verify_is_got_func_call(libjvm_handle, (uintptr_t)start, "memmove")) {
#elif defined(__aarch64__)
      if (verify_is_func_call((uintptr_t)start, (void*)copy_conjoint_words_addr)) {
#else
      if (true) {
        log(WARNING, "JBalloon::can_use_compact_humongous_obj_patching() -> compact_humongous_obj_patching() currently only implemented for x86_64 and aarch64\n");
        return false;
#endif
        patch_addr = (uintptr_t)start;
        break;
      }
    }
    // If we didn't find the call to 'memmove()'/'_Copy_conjoint_words()' in 'G1FullGCCompactTask::compact_humongous_obj()'
    // it can be that 'G1FullGCCompactTask::compact_humongous_obj()' didn't inline 'G1FullGCCompactTask::copy_object_to_new_location()'.
    // So check if we can find a call to 'G1FullGCCompactTask::copy_object_to_new_location()' in
    // 'G1FullGCCompactTask::compact_humongous_obj()' and if that's the case, look for the 'memmove()'/'_Copy_conjoint_words()'
    // call in 'G1FullGCCompactTask::copy_object_to_new_location()'.
    if (patch_addr == 0) {
      size_t copy_object_to_new_location_size = 0;
      char* copy_object_to_new_location_addr = (char*)find_local_symbol(libjvm_handle,
                                                                        copy_object_to_new_location_name,
                                                                        &copy_object_to_new_location_size);
      if (copy_object_to_new_location_addr != nullptr && copy_object_to_new_location_size != 0) {
        log(INFO, "JBalloon::can_use_compact_humongous_obj_patching() -> located 'G1FullGCCompactTask::copy_object_to_new_location(oop obj)' at %p (size = %d)\n",
                  copy_object_to_new_location_addr, copy_object_to_new_location_size);
        // Now try to find a call to 'G1FullGCCompactTask::copy_object_to_new_location()' in 'G1FullGCCompactTask::compact_humongous_obj()'.
        for (char* start = compact_humongous_obj_addr; start < compact_humongous_obj_addr + compact_humongous_obj_size; start += INSTR_INC) {
          if (verify_is_func_call((uintptr_t)start, copy_object_to_new_location_addr)) {
            // 'G1FullGCCompactTask::compact_humongous_obj()' calls 'G1FullGCCompactTask::copy_object_to_new_location()' so we can find
            // the call to 'memmove()'/'_Copy_conjoint_words()' in 'G1FullGCCompactTask::copy_object_to_new_location()' and patch it there.
            log(INFO, "JBalloon::can_use_compact_humongous_obj_patching() -> 'G1FullGCCompactTask::compact_humongous_obj()' calls 'copy_object_to_new_location()' at %p\n", start);

            // Now try to find a call to 'memmove()'/'_Copy_conjoint_words()' in 'G1FullGCCompactTask::copy_object_to_new_location()'
            for (char* s = copy_object_to_new_location_addr; s < copy_object_to_new_location_addr + copy_object_to_new_location_size; s += INSTR_INC) {
#if defined(__x86_64)
              if (verify_is_got_func_call(libjvm_handle, (uintptr_t)s, "memmove")) {
#elif defined(__aarch64__)
              if (verify_is_func_call((uintptr_t)s, (void*)copy_conjoint_words_addr)) {
#endif
                patch_addr = (uintptr_t)s;
                break;
              }
            }
            break;
          }
        }
      } else {
        log(ERROR, "JBalloon::can_use_compact_humongous_obj_patching() -> can't get %s of 'G1FullGCCompactTask::copy_object_to_new_location(oop obj)' in libjvm.so (dlerror=%s)\n",
                   (copy_object_to_new_location_addr == nullptr) ? "address" : "size",  dlerror());
      }
    }
    // If we found the call to 'memmove()'/'_Copy_conjoint_words()' patch it to call into our local 'jBalloon_memmove()' instead.
    if (patch_addr != 0) {
      return patch_call_instr(patch_addr, (uintptr_t)jBalloon_memmove, (uintptr_t)serialHeap_constr_addr);
    } else {
      log(ERROR, "JBalloon::can_use_compact_humongous_obj_patching() ->cCan't find call to 'memmove()'/'_Copy_conjoint_words() (dlerror=%s)'\n", dlerror());
    }
  } else {
    log(ERROR, "JBalloon::can_use_compact_humongous_obj_patching() -> can't get %s of 'G1FullGCCompactTask::compact_humongous_obj(HeapRegion*)' in libjvm.so (dlerror=%s)\n",
               (compact_humongous_obj_addr == nullptr) ? "address" : "size", dlerror());
  }
  return false;
}

// io.simonis.jballoon.JBalloon
jclass jballoonCls;
// io.simonis.jballoon.JBalloon.Balloon
jclass balloonCls;
// io.simonis.jballoon.JBalloon.Balloon::<init>
jmethodID balloonCstr;

extern "C"
JNIEXPORT jboolean JNICALL Java_io_simonis_jballoon_JBalloon_nativeInit(JNIEnv* env, jclass clazz, jint _javaVersion,
                                                                        jboolean _useCompressedOops, jboolean _useCompressedClassPointers,
                                                                        jboolean _compactObjectHeadersVM, jboolean _useCompactObjectHeaders,
                                                                        jint _objectHeaderSize, jstring _gc, jlong _regionSize) {

  init_log_level();

  gc = env->GetStringUTFChars(_gc, nullptr);
  if (gc == nullptr || strcmp("G1", gc)) {
    log(ERROR, "JBalloon::nativeInit() -> can't initialize jBalloon (currently only G1 GC supported)\n");
    return false;
  }
  useCompressedClassPointers = _useCompressedClassPointers;
  compactObjectHeadersVM = _compactObjectHeadersVM;
  useCompactObjectHeaders = _useCompactObjectHeaders;
  objectHeaderSize = _objectHeaderSize;
  regionSize = _regionSize;
  javaVersion = _javaVersion;

  PAGE_SIZE = sysconf(_SC_PAGE_SIZE);
  if (PAGE_SIZE <= regionSize && regionSize % PAGE_SIZE == 0) {
    log(INFO, "JBalloon::nativeInit() -> page size: %d, region size: %d, object header size: %d\n", PAGE_SIZE, regionSize, objectHeaderSize);
  } else {
    log(ERROR, "JBalloon::nativeInit() -> the region size (%d) must be a multiple of the system page size (%d)\n", regionSize, PAGE_SIZE);
    return false;
  }

  // Compute the offset of the first long of a long array relative to the array's address in the heap.
  // For long arrays, the elements are always at least 8-byte aligned
  longArrayOffset = ((objectHeaderSize + 4 /* the integer length field*/ + 4) / 8) * 8;
  // And do a sanity check to make sure our computation is correct
  jarray array = env->NewLongArray(1);
  if (array == nullptr || env->ExceptionCheck()) {
    if (env->ExceptionOccurred() != nullptr) {
      env->ExceptionClear();
    }
    log(ERROR, "JBalloon::nativeInit() -> can't allocate test long array\n");
    return false;
  } else {
    jboolean isCopy;
    jbyte* bytes = (jbyte*)env->GetPrimitiveArrayCritical(array, &isCopy);
    if (isCopy) {
      log(ERROR, "JBalloon::nativeInit() -> GetPrimitiveArrayCritical returns a copy\n");
      return false;
    }
    uintptr_t arrayOop = *(uintptr_t*)array;
    env->ReleasePrimitiveArrayCritical(array, bytes, 0);
    if ((uintptr_t)bytes - arrayOop != longArrayOffset) {
      log(ERROR, "JBalloon::nativeInit() -> can't compute longArrayOffset correctly (%d vs %d)\n", longArrayOffset, (uintptr_t)bytes - arrayOop);
      return false;
    } else {
      log(INFO, "JBalloon::nativeInit() -> long array offset in object: %d\n", longArrayOffset);
    }
  }
  longArrayPageOffset = (PAGE_SIZE - ((uintptr_t)longArrayOffset % PAGE_SIZE));

  jballoonCls = env->FindClass("io/simonis/jballoon/JBalloon");
  jballoonCls = (jclass)env->NewGlobalRef(jballoonCls);
  if (jballoonCls == nullptr) {
    log(ERROR, "JBalloon::nativeInit() -> can't find io.simonis.jballoon.JBalloon class\n");
    return false;
  }
  balloonCls = env->FindClass("io/simonis/jballoon/JBalloon$Balloon");
  balloonCls = (jclass)env->NewGlobalRef(balloonCls);
  if (balloonCls == nullptr) {
    log(ERROR, "JBalloon::nativeInit() -> can't find io.simonis.jballoon.JBalloon.Balloon class\n");
    return false;
  }
  balloonCstr = env->GetMethodID(balloonCls, "<init>", "([JJ)V");
  if (balloonCstr == nullptr) {
    log(ERROR, "JBalloon::nativeInit() -> can't find io.simonis.jballoon.JBalloon.Balloon constructor\n");
    return false;
  }

  // Try to get the heap size and base address from the vmStructs
  void* handle = dlopen(nullptr, RTLD_LAZY);
  if (handle) {
    VMStructEntry* vms = *(VMStructEntry**)dlsym(handle, "gHotSpotVMStructs");
    if (vms) {
      uintptr_t collectedHeapAddr = 0;
      int32_t reservedFieldOffset = -1;
      int32_t baseFieldOffset = -1;
      int32_t sizeFieldOffset = -1;
      for (VMStructEntry* entry = vms; entry->typeName != nullptr; entry++) {
        //log(TRACE, "JBalloon::nativeInit() -> %s, %s\n", entry->typeName, entry->fieldName);
        if (strcmp(entry->typeName, "Universe") == 0 && strcmp(entry->fieldName, "_collectedHeap") == 0) {
          collectedHeapAddr = *(uintptr_t*)(entry->address);
        }
        if (strcmp(entry->typeName, "CollectedHeap") == 0 && strcmp(entry->fieldName, "_reserved") == 0) {
          reservedFieldOffset = (int32_t)entry->offset;
        }
        if (strcmp(entry->typeName, "MemRegion") == 0) {
          if (strcmp(entry->fieldName, "_start") == 0) baseFieldOffset = (int32_t)entry->offset;
          if (strcmp(entry->fieldName, "_word_size") == 0) sizeFieldOffset = (int32_t)entry->offset;
        }
      }
      if (collectedHeapAddr && reservedFieldOffset != -1 && baseFieldOffset != -1 && sizeFieldOffset != -1) {
        // Universe::_collectedHeap -> CollectedHeap { MemRegion _reserved; } -> MemRegion { HeapWord* _start; size_t __word_size; }
        uintptr_t memRegionAddr = collectedHeapAddr + reservedFieldOffset;
        heapBase = *(jlong**)(memRegionAddr + baseFieldOffset);
        size_t heapSizeWords = *(size_t*)(memRegionAddr + sizeFieldOffset);
        heapSizeBytes = heapSizeWords * sizeof(void*);
        log(INFO, "JBalloon::nativeInit() -> heap base: %p, heap size: %ld\n", heapBase, heapSizeBytes);

        uint64_t nurOfRegions = heapSizeBytes / regionSize;
        if (nurOfRegions * regionSize != heapSizeBytes) {
          log(WARNING, "JBalloon::nativeInit() -> the heap size (%ld) should be a multiple of the region size (%ld).\n", heapSizeBytes, regionSize);
        }
        regionBitMap = new uint64_t [(nurOfRegions + 63) / 64]();
      } else {
        log(ERROR, "JBalloon::nativeInit() -> can't get all the required vmStructs (collectedHeapAddr=%p, reservedFieldOffset=%d, baseFieldOffset=%d, sizeFieldOffset=%d)\n",
                (void*)collectedHeapAddr, reservedFieldOffset, baseFieldOffset, sizeFieldOffset);
        return false;
      }
    } else {
      log(ERROR, "JBalloon::nativeInit() -> dlsym(gHotSpotVMStructs) returned \"%s\"\n", strerror(errno));
      return false;
    }
  } else {
    log(ERROR, "JBalloon::nativeInit() -> dlopen() returned \"%s\"\n", strerror(errno));
    return false;
  }

  // Before JDK 21, G1 GC didn't move humongous objects at all, so there's no need for patching.
  // See: Last-ditch Full GC should also move humongous objects (https://bugs.openjdk.org/browse/JDK-8191565)
  if (javaVersion < 21) {
    log(INFO, "JBalloon::nativeInit() -> detected JDK version %d < 21, no patching required\n", javaVersion);
    return true;
  }

  return can_use_compact_humongous_obj_patching();
}


static jobject inflateNative_impl(JNIEnv* env, jarray array, jlong size) {
  jboolean isCopy;
  jbyte* bytes = (jbyte*)env->GetPrimitiveArrayCritical(array, &isCopy);
  if (isCopy) {
    log(WARNING, "JBalloon::inflateNative_impl() -> Can't inflate balloon because GetPrimitiveArrayCritical returns a copy.\n");
    return nullptr;
  }

  // GetPrimitiveArrayCritical() returns a pointer to the array elements, but we also want the actual oject address.
  uintptr_t arrayOop = (uintptr_t)bytes - longArrayOffset;
  // Sanity check: a balloon array should always start at a region boundary.
  if (arrayOop % regionSize != 0) {
    log(WARNING, "JBalloon::inflateNative_impl() -> balloon array should start at region boundary %p\n", arrayOop);
    env->ReleasePrimitiveArrayCritical(array, bytes, 0);
    return nullptr;
  }

  if (is_balloon_region_tagged(arrayOop)) {
    log(WARNING, "JBalloon::inflateNative_impl() -> balloon array shouldn't be tagged before inflation %p\n", arrayOop);
    env->ReleasePrimitiveArrayCritical(array, bytes, 0);
    return nullptr;
  }

  // Write jBalloon_MAGIC into the first array element. Notice that we can safely use the first array
  // element, because the array oop is at least region and/or PAGE_SIZE aligned, the first array
  // element starts at 'longArrayOffset' (which is much smaller than the region size or PAGE_SIZE) and we
  // only MADV_DONTNEED at the next  PAGE_SIZE boundary (see the 'offset' computation in the next step).
  *(uint64_t*)bytes = jBalloon_MAGIC;

  // Now compute the lowest address in the array which starts at a PAGE_SIZE
  jbyte* addr = bytes + longArrayPageOffset;
  // Compute the number of full system pages we can madvise
  jlong len = ((size - longArrayPageOffset) / PAGE_SIZE) * PAGE_SIZE;
  LOG(TRACE, "JBalloon::inflateNative_impl() -> (reserved/mincore/RSS) = (%dkb / %dkb / %dkb)\n", heapSizeBytes/1024, mincoreHeapSize()/1024, rssHeapSize()/1024);
  if (madvise(addr, len, MADV_DONTNEED) != 0) {
    log(ERROR, "JBalloon::inflateNative_impl() -> madv(MADV_DONTNEED) returned \"%s\"\n", strerror(errno));
    env->ReleasePrimitiveArrayCritical(array, bytes, 0);
    return nullptr;
  }
  log(DEBUG, "JBalloon::inflateNative_impl() -> madvise(%p, %d)\n", addr, len);
  LOG(TRACE, "JBalloon::inflateNative_impl() -> (reserved/mincore/RSS) = (%dkb / %dkb / %dkb)\n", heapSizeBytes/1024, mincoreHeapSize()/1024, rssHeapSize()/1024);

  // Remember the region where the balloon array starts
  tag_balloon_region(arrayOop);
  log(DEBUG, "JBalloon::inflateNative_impl() -> created balloon at %p.\n", arrayOop);

  jobject jballoon = env->NewObject(balloonCls, balloonCstr, array, len);

  // Only do this at the very end, after we have set up all required data structures!
  env->ReleasePrimitiveArrayCritical(array, bytes, 0);

  return jballoon;
}

extern "C"
JNIEXPORT jobject JNICALL Java_io_simonis_jballoon_JBalloon_inflateNative___3J(JNIEnv* env, jclass clazz, jlongArray array) {
  // Make sure the array spans at least 3 system pages
  jlong size = env->GetArrayLength(array) * 8 /* jlong is 8 bytes */;
  if (size < 3 * PAGE_SIZE || size < (regionSize / 2)) {
    log(WARNING, "JBalloon::inflateNative(long[%d]) -> array too small for a balloon, should be at least %d bytes.\n", size, regionSize / 2);
    return nullptr;
  }

  return inflateNative_impl(env, array, size);
}

extern "C"
JNIEXPORT jobject JNICALL Java_io_simonis_jballoon_JBalloon_inflateNative__J(JNIEnv* env, jclass clazz, jlong size) {
  jlong origSize = size;
  // Make sure the balloon spans at least 3 system pages
  size = (((size + PAGE_SIZE - 1) / PAGE_SIZE) * PAGE_SIZE);
  size = (size < 3 * PAGE_SIZE) ? 3 * PAGE_SIZE : size;
  // Make sure the balloon fully fills all the region it spans and occupies at least one region (i.e. is an humongous object).
  // This takes into account the object header and a potential alignment gap which is stored in 'longArrayOffset'.
  size = ((size + longArrayOffset + regionSize - 1) / regionSize) * regionSize;
  // Cap the size at the max array length (see jdk.internal.util.ArraysSupport.MAX_ARRAY_LENGTH)
  jlong maxArrayLengthInBytes = ((jlong)INT32_MAX - 8) * 8;
  size = size > maxArrayLengthInBytes ? maxArrayLengthInBytes : size;
  // Now create the balloon array
  jarray array = env->NewLongArray(size / 8);
  if (array == nullptr || env->ExceptionCheck()) {
    if (env->ExceptionOccurred() != nullptr) {
      env->ExceptionClear();
    }
    log(WARNING, "JBalloon::inflateNative(%d) -> Can't allocate long[%d] for balloon.\n", origSize, size / 8);
    return nullptr;
  }

  return inflateNative_impl(env, array, size);
}

extern "C"
JNIEXPORT void JNICALL Java_io_simonis_jballoon_JBalloon_deflateNative(JNIEnv* env, jclass clazz, jlongArray array) {
  jboolean isCopy;
  jbyte* bytes = (jbyte*)env->GetPrimitiveArrayCritical(array, &isCopy);
  if (isCopy) {
    log(WARNING, "JBalloon::deflateNative() -> can't inflate balloon because GetPrimitiveArrayCritical returns a copy\n");
  }

  // GetPrimitiveArrayCritical() returns a pointer to the array elements, but we also want the actual oject address.
  uintptr_t arrayOop = (uintptr_t)bytes - longArrayOffset;
  // Sanity check: a balloon array should always start at a region boundary.
  if (arrayOop % regionSize != 0) {
    log(WARNING, "JBalloon::deflateNative() -> balloon array should start at region boundary %p\n", arrayOop);
    env->ReleasePrimitiveArrayCritical(array, bytes, 0);
    return;
  }

  // As sanity check, veritfy that the array has the magic value in the first entry.
  if (!(*(uint64_t*)bytes == jBalloon_MAGIC)) {
    log(WARNING, "JBalloon::deflateNative() -> can't find jBalloon_MAGIC in ballon array\n");
    env->ReleasePrimitiveArrayCritical(array, bytes, 0);
    return;
  }

  if (is_balloon_region_tagged((uintptr_t)arrayOop)) {
    untag_balloon_region((uintptr_t)arrayOop);
    // Also remove the magic number from the first array element because the array isn't special any more.
    *(uint64_t*)bytes = (uint64_t)0;
    log(DEBUG, "JBalloon::deflateNative() -> removed balloon at %p\n", arrayOop);
  } else {
    log(WARNING, "JBalloon::deflateNative() -> balloon array isn't tagged %p\n", arrayOop);
  }

  env->ReleasePrimitiveArrayCritical(array, bytes, 0);
}

unsigned char* return_vec = nullptr;

extern "C"
JNIEXPORT jlong JNICALL Java_io_simonis_jballoon_JBalloon_mincoreHeapSize(JNIEnv* env, jclass clazz) {
  if (heapBase != nullptr && heapSizeBytes != -1) {
    if (return_vec == nullptr) {
      return_vec = new unsigned char[heapSizeBytes / PAGE_SIZE];
    }
    if (mincore(heapBase, heapSizeBytes, return_vec) == 0) {
      uint64_t* vec = (uint64_t*)return_vec;
      size_t nrOfPages = heapSizeBytes / PAGE_SIZE;
      size_t pages = 0;
      for (size_t i = 0; i < (nrOfPages / 8); i++) {
        uint64_t lsbs = vec[i] & 0x0101010101010101ULL;
        pages += __builtin_popcountll(lsbs);
      }
      for (size_t i = (nrOfPages / 8) * 8; i < nrOfPages; i++) {
        pages += (return_vec[i] & 1);
      }
      return pages * PAGE_SIZE;
    } else {
      log(ERROR, "JBalloon::mincoreHeapSize() -> mincore() returned \"%s\"\n", strerror(errno));
      return -1;
    }
  } else {
    return -1;
  }

}

static jlong mincoreHeapSize() {
  return Java_io_simonis_jballoon_JBalloon_mincoreHeapSize(nullptr, 0);
}

extern "C"
JNIEXPORT jlong JNICALL Java_io_simonis_jballoon_JBalloon_rssHeapSize(JNIEnv* env, jclass clazz) {
    size_t num_pages = heapSizeBytes / PAGE_SIZE;
    uint64_t counts = 0;

    int fd = open("/proc/self/pagemap", O_RDONLY);
    if (fd < 0) {
        log(ERROR, "JBalloon::rssHeapSize() -> open(/proc/self/pagemap) returned \"%s\"\n", strerror(errno));
        return 0;
    }

    for (size_t i = 0; i < num_pages; i++) {
        uint64_t entry;
        uintptr_t vaddr = (uintptr_t)heapBase + (i * PAGE_SIZE);
        off_t offset = (vaddr / PAGE_SIZE) * sizeof(uint64_t);

        if (pread(fd, &entry, sizeof(entry), offset) != sizeof(entry)) {
            continue;
        }

        // Bit 63 indicates if a page is in memory (but this can still be the zero page)
        int present = (entry >> 63) & 1;
        // Bits 0-54 contain the Page Frame Number (PFN). This requires CAP_SYS_ADMIN privileges.
        uint64_t pfn = entry & 0x7FFFFFFFFFFFFFULL;
        // The exclusive bit is a good alternative for distinguishing zero pages from other pages in memory
        // because the zero page is almost for sure also used in other processes on the system (so it is not exclusive)
        // and all the other Java heap pages are in an anonymous, private mapping and are not shared by design.
        // And this solution doesn't require any elevated capabilities.
        int exclusive = (entry >> 56) & 1;

        // Only pages which
        if (present && (exclusive || pfn) > 0) {
            counts++;
        }
    }

    close(fd);
    return counts * PAGE_SIZE;
}

static jlong rssHeapSize() {
  return Java_io_simonis_jballoon_JBalloon_rssHeapSize(nullptr, 0);
}

extern "C"
JNIEXPORT jlong JNICALL Java_io_simonis_jballoon_JBalloon_pageSize(JNIEnv* env, jclass clazz) {
  return PAGE_SIZE;
}

extern "C"
JNIEXPORT jlong JNICALL Java_io_simonis_jballoon_JBalloon_heapSize(JNIEnv* env, jclass clazz) {
  return heapSizeBytes;
}

extern "C"
JNIEXPORT jlong JNICALL Java_io_simonis_jballoon_JBalloon_heapBase(JNIEnv* env, jclass clazz) {
  return (jlong)heapBase;
}

extern "C"
JNIEXPORT jlong JNICALL Java_io_simonis_jballoon_JBalloon_longArrayOffset(JNIEnv* env, jclass clazz) {
  return longArrayOffset;
}
