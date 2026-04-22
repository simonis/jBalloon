#include <jni.h>

#include <dlfcn.h>
#include <err.h>
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

// The userfaultfd file descriptor
static long uffd = -1;
// The default page size
static int PAGE_SIZE = -1;

// This struct is only used to get heap size and base address out of libjvm.so
struct VMStructEntry {
    const char* typeName;
    const char* fieldName;
    const char* typeString;
    int32_t  isStatic;
    uint64_t offset;
    void* address;
};
static uintptr_t heapBase = -1;
static size_t heapSizeBytes = -1;

// This has to be in sync with io.simonis.jballoon.JBalloon.Balloon::MAX_NR_OF_BALLOONS 
static int MAX_NR_OF_BALLOONS = -1;
struct Balloon {
  long id;
  jbyte* addr;
  jint len;
  Balloon(jbyte* addr, jint len, long id) : addr(addr), len(len), id(id) {}
};

enum LogLevel {
  UNINITIALIZED = -1,
  OFF,
  TRACE,
  DEBUG,
  INFO,
  WARNING,
  ERROR
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

static int log(LogLevel l, const char* format, ...) {
  if (logLevel == UNINITIALIZED) {
    init_log_level();
  }
  if (logLevel == OFF) {
    return 0;
  }
  if (l >= logLevel) {
    FILE* stream = stderr;
    if (l < WARNING) {
      stream = stdout; 
    }
    va_list args;
    va_start(args, format);
    int ret = vfprintf(stream, format, args);
    va_end(args);
    return ret;
  }
  return 0;
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
  log(TRACE, "userfaultfd_register(%p, %ld)\n", addr, len);
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
  log(TRACE, "userfaultfd_unregister(%p, %ld)\n", addr, len);
  return true;
}

// We only expect to support a small number of Ballons, so use a simple array for storing them for now
static Balloon** ballons;
static jlong id = 0;

static Balloon* new_balloon(jbyte* addr, jint len) {
  int i = 0;
  while (ballons[i] != nullptr && ++i < MAX_NR_OF_BALLOONS);
  if (i < MAX_NR_OF_BALLOONS) {
    ballons[i] = new Balloon(addr, len, id++);
    return ballons[i];
  } else {
    return nullptr;
  }
}

static Balloon* find_balloon(jbyte* addr, jlong id) {
  Balloon* found = nullptr;
  jlong foundId = -1;
  for (int i = 0; i < MAX_NR_OF_BALLOONS; i++) {
    if (ballons[i] != nullptr && addr >= ballons[i]->addr && addr < (ballons[i]->addr + ballons[i]->len)) {
      if (ballons[i]->id == id) {
        // Exact match
        return ballons[i];  
      } else if (id == -1) {
        // Find the latest Balloon at this address
        if (ballons[i]->id > foundId) {
          foundId = ballons[i]->id;
          found = ballons[i];
        }
      }
    }
  }
  return found;
}

static jboolean remove_balloon(Balloon* balloon) {
  for (int i = 0; i < MAX_NR_OF_BALLOONS; i++) {
    if (ballons[i] == balloon) {
      ballons[i] = nullptr;
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

static void* userfaultfd_handler(void* arg) {
  pthread_mutex_lock(&lock);
  userfaultfd_handler_ready = 1;
  pthread_cond_signal(&cond);
  pthread_mutex_unlock(&lock);
  log(TRACE, "userfaultfd_handler() started in new thread\n");

  long uffd = (long)arg;
  if (uffd == -1) {
    return nullptr;
  }

  struct pollfd pollfd;
  struct uffd_msg msg;
  struct uffdio_zeropage uffdio_zeropage;

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
      log(TRACE, "userfaultfd_handler(%p)\n", (void *)msg.arg.pagefault.address);

      uffdio_zeropage.range.start = (msg.arg.pagefault.address / PAGE_SIZE) * PAGE_SIZE;
      // Find the latest newest balloon which was inflated at this address
      Balloon* balloon = find_balloon((jbyte*)uffdio_zeropage.range.start, -1);
      if (balloon == nullptr) {
        log(WARNING, "Can't find Ballon for address %p\n", (void*)uffdio_zeropage.range.start);
        continue;
      }
      // This should the first page of the balloon unless e.g. the array is processed in parallel
      if ((jbyte*)uffdio_zeropage.range.start != balloon->addr) {
        log(INFO, "Pagefault at %p not in the first page of balloon at %p\n", (void*)uffdio_zeropage.range.start, balloon->addr);
        // If not use the balloons start address to cover the full address range
        uffdio_zeropage.range.start = (jlong)balloon->addr;
      }
      // We page in all pages of the balloon
      uffdio_zeropage.range.len = balloon->len;
      uffdio_zeropage.mode = 0;
      if (ioctl(uffd, UFFDIO_ZEROPAGE, &uffdio_zeropage) == -1) {
        warn("ioctl(UFFDIO_ZEROPAGE)");
        return nullptr;
      }
      log(TRACE, "ioctl(UFFDIO_ZEROPAGE, %p, %d)\n", (void*)uffdio_zeropage.range.start, balloon->len);
      // And unregister the balloon from userfaultfd
      userfaultfd_unregister((void*)uffdio_zeropage.range.start, balloon->len);
    } else {
      log(WARNING, "Unexpected event %d on userfaultfd\n", msg.event);
    }
  }
  return nullptr;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_io_simonis_jballoon_JBalloon_nativeInit(JNIEnv* env, jclass clazz) {

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
  uffdio_api.features = 0;
  if (ioctl(uffd, UFFDIO_API, &uffdio_api) == -1) {
    warn("ioctl(UFFDIO_API, 0)");
    return false;
  }
  log(INFO, "userfaultfd supported features = %llx\n", uffdio_api.features);
  uffdio_api.features = UFFD_FEATURE_EXACT_ADDRESS | UFFD_FEATURE_PAGEFAULT_FLAG_WP;
  if (uffdio_api.features & UFFD_FEATURE_EXACT_ADDRESS /* don't mask fault addresses */) {
    log(INFO, "userfaultfd supports UFFD_FEATURE_EXACT_ADDRESS, using it\n");
  }
  if (uffdio_api.features & UFFD_FEATURE_PAGEFAULT_FLAG_WP /* registration with write protection */) {
    log(INFO, "userfaultfd supports UFFD_FEATURE_PAGEFAULT_FLAG_WP, using it\n");
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
  ballons = new Balloon*[MAX_NR_OF_BALLOONS];
  for (int i = 0; i < MAX_NR_OF_BALLOONS; i++) {
    ballons[i] = nullptr;
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
          heapBase = *(uintptr_t*)(memRegionAddr + baseFieldOffset);
          size_t heapSizeWords = *(size_t*)(memRegionAddr + sizeFieldOffset);
          heapSizeBytes = heapSizeWords * sizeof(void*);
          log(INFO, "Heap base: %p, Heap size: %ld\n", (void*)heapBase, heapSizeBytes);
        } else {
          log(WARNING, "Can't get all the required vmStructs (collectedHeapAddr=%p, reservedFieldOffset=%d, baseFieldOffset=%d, sizeFieldOffset=%d)\n",
                  (void*)collectedHeapAddr, reservedFieldOffset, baseFieldOffset, sizeFieldOffset);
        }
      } else {
        warn("dlsym()");
      }
  } else {
    warn("dlopen()");
  }

  return true;
}

extern "C"
JNIEXPORT jobject JNICALL Java_io_simonis_jballoon_JBalloon_inflateNative(JNIEnv* env, jobject obj, jbyteArray array) {
  jboolean isCopy;
  jint len = env->GetArrayLength(array);
  if (len < 2 * PAGE_SIZE) {
    log(WARNING, "Can't inflate balloon because length (%d) is maller than 2*PAGE_SIZE (%d).\n", len, 2*PAGE_SIZE);
    return nullptr;
  }
  jbyte* bytes = (jbyte*)env->GetPrimitiveArrayCritical(array, &isCopy);
  if (isCopy) {
    log(WARNING, "Can't inflate balloon because GetPrimitiveArrayCritical returns a copy.\n");
    return nullptr;
  }
  long offset = (PAGE_SIZE - ((uintptr_t)bytes % PAGE_SIZE));
  jbyte* addr = bytes + offset;
  len = ((len - offset) / PAGE_SIZE) * PAGE_SIZE;
  if (madvise(addr, len, MADV_DONTNEED) != 0) {
    warn("madv(MADV_DONTNEED)");
    env->ReleasePrimitiveArrayCritical(array, bytes, 0);
    return nullptr;
  }
  log(TRACE, "inflateNative::madvise(%p, %d)\n", addr, len);
  userfaultfd_register(addr, len);
  env->ReleasePrimitiveArrayCritical(array, bytes, 0);

  Balloon* balloonC = new_balloon(addr, len);
  if (balloonC == nullptr) {
    log(TRACE, "Should never happen (Can't create native Balloon object)\n");
  }
  jobject balloon = env->NewObject(balloonCls, balloonCstr, addr, len, balloonC->id);
  return balloon;
}

extern "C"
JNIEXPORT void JNICALL Java_io_simonis_jballoon_JBalloon_removeNative(JNIEnv* env, jobject obj, jobject balloon, jbyteArray array) {
  jlong address = env->GetLongField(balloon, balloonAddressFd);
  jint length = env->GetIntField(balloon, balloonSizeFd);
  jlong id = env->GetLongField(balloon, balloonIdFd);
  Balloon* ballonC = find_balloon((jbyte*)address, id);
  if (ballonC != nullptr) {
    remove_balloon(ballonC);
  }
}

extern "C"
JNIEXPORT jobject JNICALL Java_io_simonis_jballoon_JBalloon_reinflateNative(JNIEnv* env, jobject obj, jobject balloon, jbyteArray array) {
  jlong originalAddr = env->GetLongField(balloon, balloonAddressFd);
  jint originalLen = env->GetIntField(balloon, balloonSizeFd);
  jlong id = env->GetLongField(balloon, balloonIdFd);
  Balloon* ballonC = find_balloon((jbyte*)originalAddr, id);
  if (ballonC != nullptr) {
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
      log(TRACE, "reinflateNative::madvise(%p, %d)\n", addr, len);
      // Register the new location with userfaultfd
      userfaultfd_register(addr, len);
      // And update the location in both, the Java..
      env->SetLongField(balloon, balloonAddressFd, (jlong)addr);
      env->SetIntField(balloon, balloonSizeFd, len);
      // ..and the C balloon structure
      ballonC->addr = addr;
      ballonC->len = len;
    } else {
      log(TRACE, "No need to reinflate because balloon at address %p didn't move.\n", (jbyte*)originalAddr);
    }
    env->ReleasePrimitiveArrayCritical(array, bytes, 0);
    return balloon;
  }
  log(WARNING, "Can't reinflate because no ballon found at original address %p.\n", (jbyte*)originalAddr);
  return nullptr;
}

unsigned char* return_vec = nullptr;

extern "C"
JNIEXPORT jlong JNICALL Java_io_simonis_jballoon_JBalloon_committedHeapSize(JNIEnv* env, jclass clazz) {
  if (heapBase != -1 && heapSizeBytes != -1) {
    if (return_vec == nullptr) {
      return_vec = new unsigned char[heapSizeBytes / PAGE_SIZE];
    }
    if (mincore((void*)heapBase, heapSizeBytes, return_vec) == 0) {
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

extern "C"
JNIEXPORT jlong JNICALL Java_io_simonis_jballoon_JBalloon_heapSize(JNIEnv* env, jclass clazz) {
  return heapSizeBytes;
}
