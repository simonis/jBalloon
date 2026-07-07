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

// The userfaultfd file descriptor.
static long uffd = -1;
// The default page size.
static size_t PAGE_SIZE = -1;

static jlong OVERLAP_CHUNK = 1024 * 1024;

// This struct is only used to get heap size and base address out of libjvm.so.
struct VMStructEntry {
    const char* typeName;
    const char* fieldName;
    const char* typeString;
    int32_t  isStatic;
    uint64_t offset;
    void* address;
};

// base address of libjballoon.so
static char* jBalloonBase = nullptr;
// base address of libjvm.so
static char* libJvmBase = nullptr;

static jlong* heapBase = nullptr;
static size_t heapSizeBytes = -1;

static int javaVersion;
static const char* gc;
static jboolean useCompressedClassPointers;
static jboolean compactObjectHeadersVM;
static jboolean useCompactObjectHeaders;
static jint objectHeaderSize;
static jlong regionSize; // Currently only implemented for G1 GC
// The offset of the actual byte array relative to the "[b" objects start address.
static jint byteArrayOffset;

static jlong mincoreHeapSize();
static jlong rssHeapSize();

// This struct is used to get markWord::{lock_bits, lock_shift, klass_shift} out of libjvm.so.
struct VMLongConstantEntry {
    const char* name;
    uint64_t value;
};
static jlong markWordKlassShift = -1;
static jlong markWordLockBits = -1;
static jlong markWordLockShift = -1;
static jlong fullGCForwardingShift = -1;
static jlong fullGCForwardingLowBits = -1;

// This has to be in sync with io.simonis.jballoon.JBalloon.Balloon::MAX_NR_OF_BALLOONS.
static int MAX_NR_OF_BALLOONS = -1;
struct Balloon {
  long id;
  jbyte* objAddr;
  jbyte* addr;
  jbyte* fwdAddr;
  jlong overlapOffset;
  jbyte* overlapAddr;
  jint len;
  Balloon(jbyte* objAddr, jbyte* addr, jint len, long id) :
    objAddr(objAddr), addr(addr), len(len), id(id), fwdAddr(nullptr), overlapOffset(0), overlapAddr(nullptr) {}
};

// We only expect to support a small number of Balloons, so use a simple array for storing them for now
static Balloon** balloons;
static jlong id = 0;

static Balloon* new_balloon(jbyte* objAddr, jbyte* addr, jint len) {
  int i = 0;
  while (balloons[i] != nullptr && ++i < MAX_NR_OF_BALLOONS);
  if (i < MAX_NR_OF_BALLOONS) {
    balloons[i] = new Balloon(objAddr, addr, len, id++);
    return balloons[i];
  } else {
    return nullptr;
  }
}

static Balloon* find_balloon(jbyte* addr, jlong id) {
  Balloon* found = nullptr;
  jlong foundId = -1;
  for (int i = 0; i < MAX_NR_OF_BALLOONS; i++) {
    if (balloons[i] != nullptr && addr >= balloons[i]->addr && addr < (balloons[i]->addr + balloons[i]->len)) {
      if (balloons[i]->id == id) {
        // Exact match
        return balloons[i];
      } else if (id == -1) {
        // Find the latest Balloon at this address
        if (balloons[i]->id > foundId) {
          foundId = balloons[i]->id;
          found = balloons[i];
        }
      }
    }
  }
  return found;
}

static jboolean userfaultfd_unregister(void* addr, size_t len);

static jboolean remove_balloon(Balloon* balloon) {
  for (int i = 0; i < MAX_NR_OF_BALLOONS; i++) {
    if (balloons[i] == balloon) {
      balloons[i] = nullptr;
      // Unregister balloon with userfaulfd
      userfaultfd_unregister(balloon->addr, balloon->len);
      delete balloon;
      return true;
    }
  }
  return false;
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

// This is jBalloons's replacement for the transitive call to 'memmove()' in
// 'G1FullGCCompactTask::compact_humongous_obj()' which is reached through the
// call chain: 'copy_object_to_new_locatio()' -> 'Copy::aligned_conjoint_words()'
// -> 'pd_aligned_conjoint_words()' -> 'pd_conjoint_words()' -> 'memmove()'.
extern "C" void* jBalloon_memmove(void* dest, const void* src, size_t n) {
  // Balloon object are always humongous, so we can do this first, cheap check.
  if (n >= regionSize / 2) {
    // Now check if it is really a jBalloon object.
    Balloon* balloon = nullptr;
    for (int i = 0; i < MAX_NR_OF_BALLOONS; i++) {
      if (balloons[i] != nullptr && balloons[i]->objAddr == src) {
        balloon = balloons[i];
        break;
      }
    }
    if (balloon != nullptr) {
      LOG(TRACE, "Java Heap -> (reserved/mincore/RSS) = (%dkb / %dkb / %dkb)\n", heapSizeBytes/1024, mincoreHeapSize()/1024, rssHeapSize()/1024);
      // Only copy the object header
      memmove(dest, src, 16);
      // madvise the balloon at the new location
      size_t offset = balloon->addr - balloon->objAddr;
      if (madvise((char*)dest + offset, balloon->len, MADV_DONTNEED) != 0) {
        warn("jBalloon_memmove::madv(MADV_DONTNEED)");
      }
      log(DEBUG, "jBalloon_memmove::madvise(%p, %d)\n", (char*)dest + offset, balloon->len);
      // And update the Balloon data with the new location
      balloon->objAddr = (jbyte*)dest;
      balloon->addr = (jbyte*)dest + offset;
      LOG(TRACE, "Java Heap -> (reserved/mincore/RSS) = (%dkb / %dkb / %dkb)\n", heapSizeBytes/1024, mincoreHeapSize()/1024, rssHeapSize()/1024);

      return dest;
    }
    // Fallthrough
  }

  return memmove(dest, src, n);
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

#if defined(__x86_64__)

#define INSTR_INC 1

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
  log(INFO, "Found call to 'memmove()' at %p\n", call_addr);
  uintptr_t patch_addr = call_addr + 1;
  intptr_t patch_target_offset = target - (patch_addr + 4 /* the offset is relative to the IP after the CALL instruction*/);
  if (llabs(patch_target_offset) < (1L << 31) - 1) {
    log(INFO, "Call to 'memmove()' can be patched directly\n");

    mprotect((void*)(patch_addr & ~(PAGE_SIZE - 1)), PAGE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC);
    *(int32_t*)patch_addr = patch_target_offset;
    mprotect((void*)(patch_addr & ~(PAGE_SIZE - 1)), PAGE_SIZE, PROT_READ | PROT_EXEC);

    return true;
  } else {
    log(INFO, "Patching 'memmove()' requires a trampoline\n");
    return false;
  }
}

#elif defined(__aarch64__)

#define INSTR_INC 4

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
  log(INFO, "Found call to '_Copy_conjoint_words()' at %p\n", patch_addr);

  intptr_t patch_target_offset = target - patch_addr /* the offset is relative to the IP at the BL instruction*/;
  if (llabs(patch_target_offset) >= (1L << 26) - 1) {
    log(INFO, "Patching '_Copy_conjoint_words()' requires a trampoline\n");

    patch_target_offset = trampoline_addr - patch_addr;
    if (llabs(patch_target_offset) >= (1L << 26) - 1) {
      log(INFO, "Trampoline address %p too far away from patch address %p (%d)\n", trampoline_addr, patch_addr, patch_target_offset);
      return false;
    }

    // Create the trampoline
    mprotect((void*)(trampoline_addr & ~(PAGE_SIZE - 1)), PAGE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC);
    uint32_t* trampoline = (uint32_t*)trampoline_addr;
    // movz x16, #(bits 0-15)
    trampoline[0] = 0xD2800010 | (((target >> 0)  & 0xFFFF) << 5);
    // movk x16, #(bits 16-31), lsl #16
    trampoline[1] = 0xF2A00010 | (((target >> 16) & 0xFFFF) << 5);
    // movk x16, #(bits 32-47), lsl #32
    trampoline[2] = 0xF2C00010 | (((target >> 32) & 0xFFFF) << 5);
    // movk x16, #(bits 48-63), lsl #48
    trampoline[3] = 0xF2E00010 | (((target >> 48) & 0xFFFF) << 5);
    // br x16
    trampoline[4] = 0xD61F0200;
    mprotect((void*)(trampoline_addr & ~(PAGE_SIZE - 1)), PAGE_SIZE, PROT_READ | PROT_EXEC);

    // Flush cache for the newly generated instructions
    flush_caches((char*)trampoline_addr, 5 * sizeof(uint32_t));

  } else {
    log(INFO, "Call to '_Copy_conjoint_words()' can be patched directly\n");
  }

  mprotect((void*)(patch_addr & ~(PAGE_SIZE - 1)), PAGE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC);
  *(int32_t*)patch_addr = 0b10010100000000000000000000000000 | ((((int32_t)patch_target_offset) >> 2) & 0b00000011111111111111111111111111);
  mprotect((void*)(patch_addr & ~(PAGE_SIZE - 1)), PAGE_SIZE, PROT_READ | PROT_EXEC);
  flush_caches((char*)patch_addr, sizeof(int32_t));

  return true;
}

#else

#define INSTR_INC 1

#endif

static bool can_use_compact_humongous_obj_patching() {
  Dl_info dl_info;
  if (dladdr((void*)jBalloon_memmove, &dl_info) != 0) {
    jBalloonBase = (char*)dl_info.dli_fbase;
    log(INFO, "jballoon.so loaded at %p\n", jBalloonBase);
  } else {
    warn("Can't get base address of jballoon.so");
  }

  void* libjvm_handle = dlopen("libjvm.so", RTLD_NOLOAD | RTLD_LAZY);
  if (libjvm_handle == nullptr) {
    log(ERROR, "libjvm.so must be loaded at this point.\n");
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
    log(INFO, "Located 'SerialHeap::SerialHeap()' at %p (size = %d)\n", serialHeap_constr_addr, serialHeap_constr_size);
  } else {
    warn("Can't find 'SerialHeap::SerialHeap()' in libjvm.so (dlerror=%s)", dlerror());
    return false;
  }

#if defined(__aarch64__)
  // '_Copy_conjoint_words()' is a generated stub which is used on aarch64 instead of 'memmove()'
  char* copy_conjoint_words_addr = (char*)find_local_symbol(libjvm_handle, "_Copy_conjoint_words", nullptr);
  if (copy_conjoint_words_addr != nullptr) {
    log(INFO, "Located '_Copy_conjoint_words()' at %p\n", copy_conjoint_words_addr);
  } else {
    warn("Can't find '_Copy_conjoint_words()' in libjvm.so (dlerror=%s)", dlerror());
    return false;
  }
#endif
  char* compact_humongous_obj_addr = (char*)find_local_symbol(libjvm_handle, compact_humongous_obj_name, &compact_humongous_obj_size);
  if (compact_humongous_obj_addr != nullptr) {
    log(INFO, "Located 'G1FullGCCompactTask::compact_humongous_obj(HeapRegion*)' at %p\n", compact_humongous_obj_addr);
    if (compact_humongous_obj_size != 0) {
      log(INFO, "Size of 'G1FullGCCompactTask::compact_humongous_obj(HeapRegion*)' is %d\n", compact_humongous_obj_size);
      uintptr_t patch_addr = 0;
      // Now try to find a call to 'memmove()'/'_Copy_conjoint_words()' in 'G1FullGCCompactTask::compact_humongous_obj()'
      for (char* start = compact_humongous_obj_addr; start < compact_humongous_obj_addr + compact_humongous_obj_size; start += INSTR_INC) {
#if defined(__x86_64)
        if (verify_is_got_func_call(libjvm_handle, (uintptr_t)start, "memmove")) {
#elif defined(__aarch64__)
        if (verify_is_func_call((uintptr_t)start, copy_conjoint_words_addr)) {
#else
        if (true) {
          log(WARNING, "compact_humongous_obj_patching() currently only implemented for x86_64 and aarch64\n");
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
          log(INFO, "Located 'G1FullGCCompactTask::copy_object_to_new_location(oop obj)' at %p\n", copy_object_to_new_location_addr);
          log(INFO, "Size of 'G1FullGCCompactTask::copy_object_to_new_location(oop obj)' is %d\n", copy_object_to_new_location_size);
          // Now try to find a call to 'G1FullGCCompactTask::copy_object_to_new_location()' in 'G1FullGCCompactTask::compact_humongous_obj()'.
          for (char* start = compact_humongous_obj_addr; start < compact_humongous_obj_addr + compact_humongous_obj_size; start += INSTR_INC) {
            if (verify_is_func_call((uintptr_t)start, copy_object_to_new_location_addr)) {
              // 'G1FullGCCompactTask::compact_humongous_obj()' calls 'G1FullGCCompactTask::copy_object_to_new_location()' so we can find
              // the call to 'memmove()'/'_Copy_conjoint_words()' in 'G1FullGCCompactTask::copy_object_to_new_location()' and patch it there.
              log(INFO, "'G1FullGCCompactTask::compact_humongous_obj()' calls 'copy_object_to_new_location()' at %p\n", start);

              // Now try to find a call to 'memmove()'/'_Copy_conjoint_words()' in 'G1FullGCCompactTask::copy_object_to_new_location()'
              for (char* s = copy_object_to_new_location_addr; s < copy_object_to_new_location_addr + copy_object_to_new_location_size; s += INSTR_INC) {
#if defined(__x86_64)
                if (verify_is_got_func_call(libjvm_handle, (uintptr_t)s, "memmove")) {
#elif defined(__aarch64__)
                if (verify_is_func_call((uintptr_t)s, copy_conjoint_words_addr)) {
#endif
                  patch_addr = (uintptr_t)s;
                  break;
                }
              }
              break;
            }
          }
        } else {
          if (copy_object_to_new_location_addr != nullptr) {
            warn("Can't get address of 'G1FullGCCompactTask::copy_object_to_new_location(oop obj)' in libjvm.so (dlerror=%s)", dlerror());
          } else {
            warn("Can't get size of 'G1FullGCCompactTask::copy_object_to_new_location(oop obj)' in libjvm.so (dlerror=%s)", dlerror());
          }
        }
      }
      // If we found the call to 'memmove()'/'_Copy_conjoint_words()' patch it to call into our local 'jBalloon_memmove()' instead.
      if (patch_addr != 0) {
        return patch_call_instr(patch_addr, (uintptr_t)jBalloon_memmove, (uintptr_t)serialHeap_constr_addr);
      } else {
        warn("Can't find call to 'memmove()'/'_Copy_conjoint_words()'\n");
      }
    } else {
      warn("Can't get size of 'G1FullGCCompactTask::compact_humongous_obj(HeapRegion*)' in libjvm.so (dlerror=%s)", dlerror());
    }
  } else {
    warn("Can't get address of 'G1FullGCCompactTask::compact_humongous_obj(HeapRegion*)' in libjvm.so (dlerror=%s)", dlerror());
  }
  return false;
}

static void* forwardingPointer(jlong* oop) {
  jlong mark = *oop;
  if (compactObjectHeadersVM) {
    return heapBase + ((mark & fullGCForwardingLowBits) >> fullGCForwardingShift);
  } else {
    return (void*)(mark & ~3LL); // Apply the lock mask, i.e. zero the two lower bits.
  }
}

static jboolean userfaultfd_register(void* addr, size_t len) {
  if (uffd == -1) {
    return false;
  }
  struct uffdio_register uffdio_register;
  uffdio_register.range.start = (unsigned long)addr;
  uffdio_register.range.len = len;
  uffdio_register.mode = UFFDIO_REGISTER_MODE_MISSING;
  if (ioctl(uffd, UFFDIO_REGISTER, &uffdio_register) == -1) {
    warn("ioctl(UFFDIO_REGISTER)");
    return false;
  }
  log(DEBUG, "userfaultfd_register(%p, %ld)\n", addr, len);
  return true;
}

static jboolean userfaultfd_unregister(void* addr, size_t len) {
  if (uffd == -1) {
    return false;
  }
  struct uffdio_range uffdio_range;
  uffdio_range.start = (unsigned long)addr;
  uffdio_range.len = len;
  if (ioctl(uffd, UFFDIO_UNREGISTER, &uffdio_range) == -1) {
    warn("ioctl(UFFDIO_UNREGISTER)");
    return false;
  }
  log(DEBUG, "userfaultfd_unregister(%p, %ld)\n", addr, len);
  return true;
}

static jboolean userfaultfd_zeropage(uffdio_range range, size_t mode = 0) {
  uffdio_zeropage uffdio_zeropage;
  uffdio_zeropage.range = range;
  uffdio_zeropage.mode = mode;
  if (ioctl(uffd, UFFDIO_ZEROPAGE, &uffdio_zeropage) == -1) {
    if (errno == EAGAIN) {
      log(DEBUG, "ioctl(UFFDIO_ZEROPAGE, %p, %d) = EGAIN (%d)\n", (void*)uffdio_zeropage.range.start, uffdio_zeropage.range.len, uffdio_zeropage.zeropage);
      return true;
    } else {
      warn("ioctl(UFFDIO_ZEROPAGE)");
      return false;
    }
  }
  log(DEBUG, "ioctl(UFFDIO_ZEROPAGE, %p, %d)\n", (void*)uffdio_zeropage.range.start, uffdio_zeropage.range.len);
  return true;
}


// io.simonis.jballoon.JBalloon
jclass jballoonCls;
// io.simonis.jballoon.JBalloon.Balloon
jclass balloonCls;
// io.simonis.jballoon.JBalloon.Balloon::<init>
jmethodID balloonCstr;
// io.simonis.jballoon.JBalloon.Balloon::address
jfieldID balloonAddressFd;
// io.simonis.jballoon.JBalloon.Balloon::size
jfieldID balloonSizeFd;
// io.simonis.jballoon.JBalloon.Balloon::id
jfieldID balloonIdFd;

volatile int userfaultfd_handler_ready = 0;
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

static const char* get_pagefault_flag(uint64_t flags) {
  if (flags & UFFD_PAGEFAULT_FLAG_WP) {
    return "WRITE-PROTECTED";
  } else if (flags & UFFD_PAGEFAULT_FLAG_WRITE) {
    return "MISSING-WRITE";
  } else if (flags & UFFD_PAGEFAULT_FLAG_MINOR) {
    return "MISSING-MINOR";
  } else if (flags == 0) {
    return "MISSING-READ";
  } else {
    return "OTHER";
  }
}
static void* userfaultfd_handler(void* arg) {
  pthread_mutex_lock(&lock);
  userfaultfd_handler_ready = 1;
  pthread_cond_signal(&cond);
  pthread_mutex_unlock(&lock);
  log(DEBUG, "userfaultfd_handler() started in new thread\n");

  long uffd = (long)arg;
  if (uffd == -1) {
    return nullptr;
  }

  struct pollfd pollfd;
  struct uffd_msg msg;
  struct uffdio_zeropage uffdio_zeropage;
  struct uffdio_writeprotect uffdio_writeprotect;

  while (true) {
    pollfd.fd = uffd;
    pollfd.events = POLLIN;
    int nready = poll(&pollfd, 1, -1);
    if (nready == -1) {
      warn("poll(uffd)");
      return nullptr;
    }

    size_t nread = read(uffd, &msg, sizeof(msg));
    if (nread == -1) {
      warn("read(uffd)");
      return nullptr;
    } else if (nread == 0) {
      log(ERROR, "EOF in userfaultfd\n");
      return nullptr;
    }
    if (msg.event == UFFD_EVENT_PAGEFAULT) {
      log(DEBUG, "userfaultfd_handler(%u, %p, %s)\n",
        msg.arg.pagefault.feat.ptid, (void *)msg.arg.pagefault.address, get_pagefault_flag(msg.arg.pagefault.flags));

      LOG(TRACE, "Java Heap -> (reserved/mincore/RSS) = (%dkb / %dkb / %dkb)\n", heapSizeBytes/1024, mincoreHeapSize()/1024, rssHeapSize()/1024);

      struct uffdio_range range;
      range.start = (msg.arg.pagefault.address / PAGE_SIZE) * PAGE_SIZE;

      // Find the latest newest balloon which was inflated at this address
      Balloon* balloon = find_balloon((jbyte*)range.start, -1);
      if (balloon == nullptr) {
        log(WARNING, "Can't find Balloon for address %p\n", (void*)uffdio_zeropage.range.start);
        continue;
      }

      if (msg.arg.pagefault.flags == 0 /* reading a missing page */) {
        if ((jbyte*)range.start == balloon->addr) {
          // The first page of the balloon. The first page fault should always happen in the
          // first page of the balloon, because G1 GC compacts/copies humongous objects from
          // left to right and by contract, nobody else should touch the balloon except the GC
          // when compacting/moving it.
          log(INFO, "Pagefault at %p in the first page of balloon at %p\n", (void*)range.start, balloon->addr);
          // If we have been moved already, we will have to inflate our last page (actually our last THREE pages - see below).
          if (madvise(balloon->addr + balloon->len - (3*PAGE_SIZE), 3*PAGE_SIZE, MADV_DONTNEED) != 0) {
            warn("userfaultfd_handler::madv(MADV_DONTNEED, LAST_PAGE)");
            return nullptr;
          }
          log(DEBUG, "userfaultfd_handler::madvise(%p, %d, LAST_PAGE)\n", balloon->addr + balloon->len - (3*PAGE_SIZE), 3*PAGE_SIZE);
          // Extract the forwarding pointer from balloon->objAddr to compute the balloon address at the new location.
          // This highly depends on the configuration (i.e. GC, CompressedObjectHeader, CompressedClassPointers, etc.)
          // For G1 GC, we know that the forwarding pointers have been installed before humongous objects are
          // moved and humongous compaction only happens in a stop the world phase at a safepoint.
          //balloon->fwdAddr = (jbyte*)(heapBase + (*(jlong*)balloon->objAddr >> 2));
          balloon->fwdAddr = (jbyte*)forwardingPointer((jlong*)balloon->objAddr);
          jlong overlapOffset = balloon->objAddr - balloon->fwdAddr;
          jlong overlap = balloon->len - overlapOffset;
          log(DEBUG, "balloon at %p moves to %p with %d bytes of overlap\n", balloon->objAddr, balloon->fwdAddr, overlap > 0 ? overlap : 0);
          if (overlap > (OVERLAP_CHUNK + PAGE_SIZE)) {
            // If there's an overlap between the old and the new location, this overlap region will be paged
            // in when the object is moved, because we don't only read from this overlap area (when reading from
            // the source location) but also write into it (when writing to the destination location). Reading
            // from a MADV_DONTNEED page will only page in the system ZERO page, which doesn't consume any memory
            // at all. But writing into a page which is backed up by the system ZERO page will allocate a real
            // memory page, even if we only write zeros into that page (i.e. the kernel doesn't check what we write).
            // This means that once we reach the read location in the source object which writes into the overlap
            // are, we must consecutively MADV_DONTNEED the previously written pages, to ensure that the balloon
            // maintains its size of un-allocated memory. We therefore only page in the pages up to this overlap
            // location, such that we will be notified when we will have to start this process.
            // To speed up things we handle the inflation of the overlap in bigger chunks.
            size_t chunks = (overlap - PAGE_SIZE) / OVERLAP_CHUNK;
            size_t remaining = (overlap - PAGE_SIZE) % OVERLAP_CHUNK;
            range.len = overlapOffset + remaining;
            // We also record the overlap address to ease processing the overlap range.
            assert(balloon->overlapAddr == nullptr && balloon->overlapOffset == 0);
            balloon->overlapAddr = balloon->addr + overlapOffset + remaining;
            balloon->overlapOffset = overlapOffset;
          } else {
            // No overlap, so we can immediately page in all the pages which belong to the balloon except for the
            // very last one because we still want to be notified when copying of the balloon is done (i.e. when the
            // last page will be copied).
            range.len = balloon->len - PAGE_SIZE;
          }
          userfaultfd_zeropage(range);
        } else if ((jbyte*)range.start == balloon->overlapAddr) {
          // We're in the overlap region.
          log(INFO, "Pagefault at %p in the overlap region of balloon at %p\n", (void*)range.start, balloon->addr);
          // Free the previous overlap page.
          if (madvise(balloon->overlapAddr - balloon->overlapOffset - OVERLAP_CHUNK, OVERLAP_CHUNK, MADV_DONTNEED) != 0) {
            warn("userfaultfd_handler::madv(MADV_DONTNEED, OVERLAP)");
            return nullptr;
          }
          log(DEBUG, "userfaultfd_handler::madvise(%p, %d, OVERLAP)\n", balloon->overlapAddr - balloon->overlapOffset - OVERLAP_CHUNK, OVERLAP_CHUNK);
          // Zero page in the current chunk so we can continue to copy the object.
          range.len = OVERLAP_CHUNK;
          userfaultfd_zeropage(range);
          // And update 'overlapAddr' until we reach the last page of the source object.
          if ((balloon->overlapAddr + OVERLAP_CHUNK) < (balloon->addr + balloon->len - PAGE_SIZE)) {
            balloon->overlapAddr += OVERLAP_CHUNK;
          } else {
            // We're done with the overlap area. Processing the last page of the source object will do the rest.
            balloon->overlapOffset = 0;
            balloon->overlapAddr = nullptr;
          }
        } else if ((jbyte*)range.start == (balloon->addr + balloon->len - PAGE_SIZE)) {
          // The last page of the balloon. This means that copying the balloon is about to finish.
          log(INFO, "Pagefault at %p in the last page of balloon at %p\n", (void*)range.start, balloon->addr);
          jbyte* oldAddr = balloon->addr; // We still need this for unregistering the old range from userfaultfd.
          // Update the balloon to the new location
          assert(balloon->fwdAddr != nullptr);
          balloon->objAddr = balloon->fwdAddr;
          jbyte* bytes = balloon->fwdAddr + byteArrayOffset;
          balloon->addr = bytes + (PAGE_SIZE - ((jlong)bytes % PAGE_SIZE));
          balloon->fwdAddr = nullptr;
          // Inflate the balloon at the new location. We don't inflate the last page, otherwise we would
          // immediately get a page fault while the last page is copied to the new location. We will inflate
          // the last page, when the first page is touched (which can only happen when the balloon will be moved again).
          // We actually dont inflate the last THREE pages, because glibc's optimized memmove() which is used in
          // HotSpot to move objects during GC can use prefetching and striping which might still write to the two
          // previous destination pages while reading from the last source page.
          if (madvise(balloon->addr, balloon->len - (3*PAGE_SIZE), MADV_DONTNEED) != 0) {
            warn("userfaultfd_handler::madv(MADV_DONTNEED, MOVED)");
            return nullptr;
          }
          log(DEBUG, "userfaultfd_handler::madvise(%p, %d, MOVED)\n", balloon->addr, balloon->len - (3*PAGE_SIZE));
          // Now register the new balloon with userfaultfd.
          userfaultfd_register(balloon->addr, balloon->len);
          // Page in the last page at the old balloon location. This will allow the copy process to finish successfully.
          range.len = PAGE_SIZE;
          userfaultfd_zeropage(range);
          // And finally, unregister the current range from userfaultfd, we don't need it any more. We must be careful
          // here, to not unregister a part of the new mapping registered before if the old and new range overlaps!
          // Notice that G1 GC always compacts to the beginning of the heap so oldAddr must be strictly bigger than the
          // address of the new balloon.
          assert(oldAddr > balloon->addr);
          if (oldAddr - balloon->addr >= balloon->len) {
            userfaultfd_unregister(oldAddr, balloon->len);
          } else {
            userfaultfd_unregister(balloon->addr + balloon->len /* end of the new balloon */,
                                   oldAddr - balloon->addr      /* length of non-overlapping part */);
          }
        } else {
          log(INFO, "Pagefault at %p not in the first or last page of the balloon at %p\n", (void*)range.start, balloon->addr);
          range.len = PAGE_SIZE;
          userfaultfd_zeropage(range);
        }
        LOG(TRACE, "Java Heap <- (reserved/mincore/RSS) = (%dkb / %dkb / %dkb)\n", heapSizeBytes/1024, mincoreHeapSize()/1024, rssHeapSize()/1024);
      } else if (msg.arg.pagefault.flags & UFFD_PAGEFAULT_FLAG_WRITE) {
        log(WARNING, "Pagefault UFFD_PAGEFAULT_FLAG_WRITE at %p (should not happen - probably due to optimized memmove()))\n", (void*)range.start);
        range.len = PAGE_SIZE;
        userfaultfd_zeropage(range);
      } else {
        log(WARNING, "Pagefault with flags %d at %p (should not happen))\n", msg.arg.pagefault.flags, (void*)range.start);
      }
    } else {
      log(WARNING, "Unexpected event %d on userfaultfd\n", msg.event);
    }
  }
  return nullptr;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_io_simonis_jballoon_JBalloon_nativeInit(JNIEnv* env, jclass clazz, jint _javaVersion,
                                                                        jboolean _useCompressedOops, jboolean _useCompressedClassPointers,
                                                                        jboolean _compactObjectHeadersVM, jboolean _useCompactObjectHeaders,
                                                                        jint _objectHeaderSize, jstring _gc, jlong _regionSize) {

  init_log_level();

  gc = env->GetStringUTFChars(_gc, nullptr);
  if (gc == nullptr || strcmp("G1", gc)) {
    log(ERROR, "Cant initialize jBalloon (curerntly only G1 GC supported).\n");
    return false;
  }
  useCompressedClassPointers = _useCompressedClassPointers;
  compactObjectHeadersVM = _compactObjectHeadersVM;
  useCompactObjectHeaders = _useCompactObjectHeaders;
  objectHeaderSize = _objectHeaderSize;
  regionSize = _regionSize;
  javaVersion = _javaVersion;
  byteArrayOffset = objectHeaderSize + 4 /* the integer length field*/;
  if (javaVersion < 23) {
    // Need to align to 8 bytes (see https://bugs.openjdk.org/browse/JDK-8314882).
    byteArrayOffset = ((byteArrayOffset + 4) / 8) * 8;
  }

  jballoonCls = env->FindClass("io/simonis/jballoon/JBalloon");
  jballoonCls = (jclass)env->NewGlobalRef(jballoonCls);
  if (jballoonCls == nullptr) {
    log(ERROR, "Can't find io.simonis.jballoon.JBalloon class.\n");
    return false;
  }
  balloonCls = env->FindClass("io/simonis/jballoon/JBalloon$Balloon");
  balloonCls = (jclass)env->NewGlobalRef(balloonCls);
  if (balloonCls == nullptr) {
    log(ERROR, "Can't find io.simonis.jballoon.JBalloon.Balloon class.\n");
    return false;
  }
  balloonCstr = env->GetMethodID(balloonCls, "<init>", "(JIJ)V");
  if (balloonCstr == nullptr) {
    log(ERROR, "Can't find io.simonis.jballoon.JBalloon.Balloon constructor.\n");
    return false;
  }
  balloonAddressFd = env->GetFieldID(balloonCls, "address", "J");
  if (balloonAddressFd == nullptr) {
    log(ERROR, "Can't find io.simonis.jballoon.JBalloon.Balloon.address field.\n");
    return false;
  }
  balloonSizeFd = env->GetFieldID(balloonCls, "size", "I");
  if (balloonSizeFd == nullptr) {
    log(ERROR, "Can't find io.simonis.jballoon.JBalloon.Balloon.size field.\n");
    return false;
  }
  balloonIdFd = env->GetFieldID(balloonCls, "id", "J");
  if (balloonSizeFd == nullptr) {
    log(ERROR, "Can't find io.simonis.jballoon.JBalloon.Balloon.id field.\n");
    return false;
  }

  PAGE_SIZE = sysconf(_SC_PAGE_SIZE);

  jfieldID pageSizeField = env->GetStaticFieldID(balloonCls, "PAGE_SIZE", "I");
  env->SetStaticIntField(balloonCls, pageSizeField, PAGE_SIZE);

  bool use_jBalloon_memmove = can_use_compact_humongous_obj_patching();
  /*
  Dl_info dl_info;
  if (dladdr((void*)jBalloon_memmove, &dl_info) != 0) {
    jBalloonBase = (char*)dl_info.dli_fbase;
    log(INFO, "jballoon.so loaded at %p\n", jBalloonBase);
  } else {
    warn("Can't get base address of jballoon.so");
  }

  void* libjvm_handle = dlopen("libjvm.so", RTLD_NOLOAD | RTLD_LAZY);
  if (libjvm_handle == nullptr) {
    log(ERROR, "libjvm.so must be loaded at this point.\n");
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
  char* compact_humongous_obj_addr = (char*)find_local_symbol(libjvm_handle, compact_humongous_obj_name, &compact_humongous_obj_size);
  if (compact_humongous_obj_addr != nullptr) {
    log(INFO, "Located 'G1FullGCCompactTask::compact_humongous_obj(HeapRegion*)' at %p\n", compact_humongous_obj_addr);
    if (compact_humongous_obj_size != 0) {
      log(INFO, "Size of 'G1FullGCCompactTask::compact_humongous_obj(HeapRegion*)' is %d\n", compact_humongous_obj_size);
      uintptr_t patch_addr = 0;
      // Now try to find a call to 'memmove()' in 'G1FullGCCompactTask::compact_humongous_obj()'
      for (char* start = compact_humongous_obj_addr; start < compact_humongous_obj_addr + compact_humongous_obj_size; start++) {
        if (*start == (char)0xe8 && verify_is_got_func_call(libjvm_handle, (uintptr_t)start, "memmove")) {
          patch_addr = (uintptr_t)(start + 1);
          break;
        }
      }
      // If we didn't find the call to 'memmove()' in 'G1FullGCCompactTask::compact_humongous_obj()' it can be that
      // 'G1FullGCCompactTask::compact_humongous_obj()' didn't inline ''G1FullGCCompactTask::copy_object_to_new_location()'.
      // So check if we can find a call to 'G1FullGCCompactTask::copy_object_to_new_location()' in
      // 'G1FullGCCompactTask::compact_humongous_obj()' and if that's the case, look for the 'memmove()' call in
      // 'G1FullGCCompactTask::copy_object_to_new_location()'.
      if (patch_addr == 0) {
        size_t copy_object_to_new_location_size = 0;
        char* copy_object_to_new_location_addr = (char*)find_local_symbol(libjvm_handle,
                                                                          copy_object_to_new_location_name,
                                                                          &copy_object_to_new_location_size);
        if (copy_object_to_new_location_addr != nullptr && copy_object_to_new_location_size != 0) {
          log(INFO, "Located 'G1FullGCCompactTask::copy_object_to_new_location(oop obj)' at %p\n", copy_object_to_new_location_addr);
          log(INFO, "Size of 'G1FullGCCompactTask::copy_object_to_new_location(oop obj)' is %d\n", copy_object_to_new_location_size);
          // Now try to find a call to 'G1FullGCCompactTask::copy_object_to_new_location()' in 'G1FullGCCompactTask::compact_humongous_obj()'.
          for (char* start = compact_humongous_obj_addr; start < compact_humongous_obj_addr + compact_humongous_obj_size; start++) {
            if (*start == (char)0xe8) {
              int32_t relative_offset;
              memcpy(&relative_offset, (void*)(start + 1), 4);
              if (start + 5 + relative_offset == copy_object_to_new_location_addr) {
                // 'G1FullGCCompactTask::compact_humongous_obj()' calls 'G1FullGCCompactTask::copy_object_to_new_location()'
                // so we can find the call to 'memmove()' in 'G1FullGCCompactTask::copy_object_to_new_location()' and patch it there.
                log(INFO, "'G1FullGCCompactTask::compact_humongous_obj()' calls 'copy_object_to_new_location()' at %p\n", start);

                // Now try to find a call to 'memmove()' in 'G1FullGCCompactTask::compact_humongous_obj()'
                for (char* s = copy_object_to_new_location_addr; s < copy_object_to_new_location_addr + copy_object_to_new_location_size; s++) {
                  if (*s == (char)0xe8 && verify_is_got_func_call(libjvm_handle, (uintptr_t)s, "memmove")) {
                    patch_addr = (uintptr_t)(s + 1);
                    break;
                  }
                }
                break;
              }
            }
          }
        } else {
          if (copy_object_to_new_location_addr != nullptr) {
            warn("Can't get address of 'G1FullGCCompactTask::copy_object_to_new_location(oop obj)' in libjvm.so (dlerror=%s)", dlerror());
          } else {
            warn("Can't get size of 'G1FullGCCompactTask::copy_object_to_new_location(oop obj)' in libjvm.so (dlerror=%s)", dlerror());
          }
        }
      }
      // If we found the call to 'memmove()' patch it to call into our local 'jBalloon_memmove()' instead.
      if (patch_addr != 0) {
        log(INFO, "Found call to 'memmove()' in 'G1FullGCCompactTask::compact_humongous_obj(HeapRegion*)' at %p\n", patch_addr - 1);
        intptr_t patch_target_offset = (uintptr_t)jBalloon_memmove - (patch_addr + 4); // the offset is relative to the IP after the CALL instruction
        if (llabs(patch_target_offset) < (1L << 31) - 1) {
          log(INFO, "Call to 'memmove()' in 'G1FullGCCompactTask::compact_humongous_obj(HeapRegion*)' can be patched directly\n");

          mprotect((void*)(patch_addr & ~(PAGE_SIZE - 1)), PAGE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC);
          *(int32_t*)patch_addr = patch_target_offset;
          mprotect((void*)(patch_addr & ~(PAGE_SIZE - 1)), PAGE_SIZE, PROT_READ | PROT_EXEC);

          use_jBalloon_memmove = true;
        } else {
          log(INFO, "Patching 'memmove()' in 'G1FullGCCompactTask::compact_humongous_obj(HeapRegion*)' require a trampoline\n");
        }
      } else {
        warn("Can't find call to 'memmove()' in 'G1FullGCCompactTask::compact_humongous_obj(HeapRegion*)'\n");
      }
    } else {
      warn("Can't get size of 'G1FullGCCompactTask::compact_humongous_obj(HeapRegion*)' in libjvm.so (dlerror=%s)", dlerror());
    }
  } else {
    warn("Can't get address of 'G1FullGCCompactTask::compact_humongous_obj(HeapRegion*)' in libjvm.so (dlerror=%s)", dlerror());
  }
  */
  if (!use_jBalloon_memmove) {
  // userfaultfd initialization

  uffd = syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY /* useful for debugging */);
  if (uffd == -1) {
    warn("syscall(SYS_userfaultfd)");
    return false;
  }

  struct uffdio_api uffdio_api;
  uffdio_api.api = UFFD_API;
  uffdio_api.features = UFFD_FEATURE_EXACT_ADDRESS | UFFD_FEATURE_THREAD_ID;
  if (ioctl(uffd, UFFDIO_API, &uffdio_api) == -1) {
    warn("ioctl(UFFDIO_API, 0)");
    return false;
  }
  log(INFO, "userfaultfd supported features = %llx\n", uffdio_api.features);
  if (uffdio_api.features & UFFD_FEATURE_EXACT_ADDRESS) {
    log(INFO, "userfaultfd supports UFFD_FEATURE_EXACT_ADDRESS, using it\n");
  }
  if (uffdio_api.features & UFFD_FEATURE_THREAD_ID) {
    log(INFO, "userfaultfd supports UFFD_FEATURE_THREAD_ID, using it\n");
  }

  pthread_t thr;
  if (pthread_create(&thr, nullptr, &userfaultfd_handler, (void *)uffd) != 0) {
    warn("pthread_create()");
    return false;
  }
  // Wait until the thread is running.
  pthread_mutex_lock(&lock);
  while (!userfaultfd_handler_ready) {
    pthread_cond_wait(&cond, &lock);
  }

  } // if (!use_jBalloon_memmove)

  jfieldID balloonMaxNrOfBalloonsField = env->GetStaticFieldID(jballoonCls, "MAX_NR_OF_BALLOONS", "I");
  if (balloonMaxNrOfBalloonsField == nullptr) {
    log(ERROR, "Can't find io.simonis.jballoon.JBalloon.Balloon.MAX_NR_OF_BALLOONS field");
    return false;
  } else {
    MAX_NR_OF_BALLOONS = env->GetStaticIntField(jballoonCls, balloonMaxNrOfBalloonsField);
  }
  balloons = new Balloon*[MAX_NR_OF_BALLOONS];
  for (int i = 0; i < MAX_NR_OF_BALLOONS; i++) {
    balloons[i] = nullptr;
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
          //log(TRACE, ">>> %s, %s\n", entry->typeName, entry->fieldName);
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
          log(INFO, "Heap base: %p, Heap size: %ld\n", heapBase, heapSizeBytes);
        } else {
          log(WARNING, "Can't get all the required vmStructs (collectedHeapAddr=%p, reservedFieldOffset=%d, baseFieldOffset=%d, sizeFieldOffset=%d)\n",
                  (void*)collectedHeapAddr, reservedFieldOffset, baseFieldOffset, sizeFieldOffset);
        }
      } else {
        warn("dlsym(gHotSpotVMStructs)");
      }
      VMLongConstantEntry* vmc = *(VMLongConstantEntry**)dlsym(handle, "gHotSpotVMLongConstants");
      if (vmc) {
        for (VMLongConstantEntry* entry = vmc; entry->name != nullptr; entry++) {
          //log(TRACE, ">>> %s, %d\n", entry->name, entry->value);
          if (strcmp(entry->name, "markWord::klass_shift") == 0) markWordKlassShift = entry->value;
          if (strcmp(entry->name, "markWord::lock_bits") == 0) markWordLockBits = entry->value;
          if (strcmp(entry->name, "markWord::lock_shift") == 0) markWordLockShift = entry->value;
        }
        if ((!compactObjectHeadersVM || markWordKlassShift != -1) && markWordLockBits != -1 && markWordLockShift != -1) {
          log(INFO, "markWord::klass_shift: %d, markWord::lock_bits: %d, markWord::lock_shift: %d\n",
            markWordKlassShift, markWordLockBits, markWordLockShift);
            fullGCForwardingShift = markWordLockBits + markWordLockShift;
            if (useCompactObjectHeaders) {
              fullGCForwardingLowBits = (1LL << markWordKlassShift) - 1;
            } else {
              fullGCForwardingLowBits = -1;
            }
        } else {
          log(WARNING, "Can't get vmStructs for markWord::klass_shift: %d, markWord::lock_bits: %d, markWord::lock_shift: %d\n",
            markWordKlassShift, markWordLockBits, markWordLockShift);
        }
      } else {
        warn("dlsym(gHotSpotVMIntConstants)");
      }
  } else {
    warn("dlopen()");
  }

  return true;
}

extern "C"
JNIEXPORT jobject JNICALL Java_io_simonis_jballoon_JBalloon_inflateNative(JNIEnv* env, jclass clazz, jbyteArray array) {
  jboolean isCopy;
  jint len = env->GetArrayLength(array);
  if (len < 3 * PAGE_SIZE) {
    log(WARNING, "inflateNative::Can't inflate balloon because length (%d) is smaller than 3*PAGE_SIZE (%d).\n", len, 3*PAGE_SIZE);
    return nullptr;
  }
  jbyte* bytes = (jbyte*)env->GetPrimitiveArrayCritical(array, &isCopy);
  if (isCopy) {
    log(WARNING, "inflateNative::Can't inflate balloon because GetPrimitiveArrayCritical returns a copy.\n");
    return nullptr;
  }
  long offset = (PAGE_SIZE - ((uintptr_t)bytes % PAGE_SIZE));
  jbyte* addr = bytes + offset;
  // We cut off the last page because optimized versions of memmove() may read the end of the "from" region although they copy
  // from left to right and this might confuse our bookkeeping.
  len = ((len - offset - PAGE_SIZE/* XXX */) / PAGE_SIZE) * PAGE_SIZE;
  if (madvise(addr, len, MADV_DONTNEED) != 0) {
    warn("inflateNative::madv(MADV_DONTNEED)");
    env->ReleasePrimitiveArrayCritical(array, bytes, 0);
    return nullptr;
  }
  log(DEBUG, "inflateNative::madvise(%p, %d)\n", addr, len);
  LOG(TRACE, "Java Heap -> (reserved/mincore/RSS) = (%dkb / %dkb / %dkb)\n", heapSizeBytes/1024, mincoreHeapSize()/1024, rssHeapSize()/1024);
  userfaultfd_register(addr, len);
  env->ReleasePrimitiveArrayCritical(array, bytes, 0);

  // GetPrimitiveArrayCritical() returns a pointer to the array elements, but we want the actual oject address.
  Balloon* balloon = new_balloon(bytes - byteArrayOffset, addr, len);
  if (balloon == nullptr) {
    log(WARNING, "inflateNative::Should never happen (Can't create native Balloon object at %p)\n", addr);
  } else {
    log(DEBUG, "inflateNative::Created balloon at %p with id %d.\n", addr, balloon->id);
  }
  jobject jballoon = env->NewObject(balloonCls, balloonCstr, addr, len, balloon->id);
  return jballoon;
}

extern "C"
JNIEXPORT void JNICALL Java_io_simonis_jballoon_JBalloon_removeNative(JNIEnv* env, jclass clazz, jobject jballoon, jbyteArray array) {
  jboolean isCopy;
  jbyte* bytes = (jbyte*)env->GetPrimitiveArrayCritical(array, &isCopy);
  if (isCopy) {
    log(WARNING, "Can't inflate balloon because GetPrimitiveArrayCritical returns a copy.\n");
  }
  long offset = (PAGE_SIZE - ((uintptr_t)bytes % PAGE_SIZE));
  jbyte* addr = bytes + offset;
  jlong id = env->GetLongField(jballoon, balloonIdFd);
  Balloon* balloon = find_balloon(addr, id);
  if (balloon != nullptr) {
    if (remove_balloon(balloon)) {
      log(DEBUG, "removeNative::Removed balloon at %p with id %d.\n", addr, id);
    } else {
      log(WARNING, "removeNative::Can't remove balloon at %p with id %d.\n", addr, id);
    }
  } else {
    log(WARNING, "removeNative::Can't find balloon at %p with id %d.\n", addr, id);
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
      warn("mincore()");
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
        warn("open(/proc/self/pagemap)");
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
JNIEXPORT jlong JNICALL Java_io_simonis_jballoon_JBalloon_heapSize(JNIEnv* env, jclass clazz) {
  return heapSizeBytes;
}
