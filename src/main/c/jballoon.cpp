#include <jni.h>

#include <assert.h>
#include <dlfcn.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
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
static int PAGE_SIZE = -1;

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

static jlong* heapBase = nullptr;
static size_t heapSizeBytes = -1;

static int javaVersion;
static const char* gc;
static jboolean useCompressedClassPointers;
static jboolean compactObjectHeadersVM;
static jboolean useCompactObjectHeaders;
static jint objectHeaderSize;
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

void* forwardingPointer(jlong* oop) {
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
  void* handle = dlopen(NULL, RTLD_LAZY);
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
    log(WARNING, "Can't inflate balloon because length (%d) is smaller than 3*PAGE_SIZE (%d).\n", len, 3*PAGE_SIZE);
    return nullptr;
  }
  jbyte* bytes = (jbyte*)env->GetPrimitiveArrayCritical(array, &isCopy);
  if (isCopy) {
    log(WARNING, "Can't inflate balloon because GetPrimitiveArrayCritical returns a copy.\n");
    return nullptr;
  }
  long offset = (PAGE_SIZE - ((uintptr_t)bytes % PAGE_SIZE));
  jbyte* addr = bytes + offset;
  // We cut off the last page because optimized versions of memmove() may read the end of the "from" region although they copy
  // from left to right and this might confuse our bookkeeping. 
  len = ((len - offset - PAGE_SIZE/* XXX */) / PAGE_SIZE) * PAGE_SIZE;
  if (madvise(addr, len, MADV_DONTNEED) != 0) {
    warn("madv(MADV_DONTNEED)");
    env->ReleasePrimitiveArrayCritical(array, bytes, 0);
    return nullptr;
  }
  log(DEBUG, "inflateNative::madvise(%p, %d)\n", addr, len);
  userfaultfd_register(addr, len);
  env->ReleasePrimitiveArrayCritical(array, bytes, 0);

  // GetPrimitiveArrayCritical() returns a pointer to the array elements, but we want the actual oject address.
  Balloon* balloonC = new_balloon(bytes - byteArrayOffset, addr, len);
  if (balloonC == nullptr) {
    log(DEBUG, "Should never happen (Can't create native Balloon object)\n");
  }
  jobject balloon = env->NewObject(balloonCls, balloonCstr, addr, len, balloonC->id);
  return balloon;
}

extern "C"
JNIEXPORT void JNICALL Java_io_simonis_jballoon_JBalloon_removeNative(JNIEnv* env, jclass clazz, jobject balloon, jbyteArray array) {
  jlong address = env->GetLongField(balloon, balloonAddressFd);
  jint length = env->GetIntField(balloon, balloonSizeFd);
  jlong id = env->GetLongField(balloon, balloonIdFd);
  Balloon* balloonC = find_balloon((jbyte*)address, id);
  if (balloonC != nullptr) {
    remove_balloon(balloonC);
  }
}

extern "C"
JNIEXPORT jobject JNICALL Java_io_simonis_jballoon_JBalloon_reinflateNative(JNIEnv* env, jclass clazz, jobject balloon, jbyteArray array) {
  jlong originalAddr = env->GetLongField(balloon, balloonAddressFd);
  jint originalLen = env->GetIntField(balloon, balloonSizeFd);
  jlong id = env->GetLongField(balloon, balloonIdFd);
  Balloon* balloonC = find_balloon((jbyte*)originalAddr, id);
  if (balloonC != nullptr) {
    jboolean isCopy;
    jint len = env->GetArrayLength(array);
    if (len < 2 * PAGE_SIZE) {
      log(ERROR, "Can't reinflate balloon because length (%d) is smaller than 2*PAGE_SIZE (%d).\n", len, 2*PAGE_SIZE);
      return nullptr;
    }
    jbyte* bytes = (jbyte*)env->GetPrimitiveArrayCritical(array, &isCopy);
    if (isCopy) {
      log(ERROR, "Can't reinflate balloon because GetPrimitiveArrayCritical returns a copy.\n");
      return nullptr;
    }
    long offset = (PAGE_SIZE - ((uintptr_t)bytes % PAGE_SIZE));
    jbyte* addr = bytes + offset;
    len = ((len - offset) / PAGE_SIZE) * PAGE_SIZE;
    // If the original balloon was moved by GC
    if ((jbyte*)originalAddr != addr) {
      // Inflate the balloon in the new location
      if (madvise(addr, len, MADV_DONTNEED) != 0) {
        warn("madvise(MADV_DONTNEED)");
        env->ReleasePrimitiveArrayCritical(array, bytes, 0);
        return nullptr;
      }
      log(DEBUG, "reinflateNative::madvise(%p, %d)\n", addr, len);
      // Register the new location with userfaultfd
      userfaultfd_register(addr, len);
      // And update the location in both, the Java..
      env->SetLongField(balloon, balloonAddressFd, (jlong)addr);
      env->SetIntField(balloon, balloonSizeFd, len);
      // ..and the C balloon structure
      balloonC->addr = addr;
      balloonC->len = len;
    } else {
      log(DEBUG, "No need to reinflate because balloon at address %p didn't move.\n", (jbyte*)originalAddr);
    }
    env->ReleasePrimitiveArrayCritical(array, bytes, 0);
    return balloon;
  }
  log(WARNING, "Can't reinflate because no balloon found at original address %p.\n", (jbyte*)originalAddr);
  return nullptr;
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
