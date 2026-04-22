package io.simonis.jballoon;

import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.GroupLayout;
import java.lang.foreign.Linker;
import java.lang.foreign.MemoryLayout;
import java.lang.foreign.MemoryLayout.PathElement;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.SymbolLookup;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.VarHandle;
import java.lang.ref.WeakReference;
import java.util.HashMap;

import static java.lang.foreign.ValueLayout.ADDRESS;
import static java.lang.foreign.ValueLayout.JAVA_BYTE;
import static java.lang.foreign.ValueLayout.JAVA_CHAR;
import static java.lang.foreign.ValueLayout.JAVA_INT;
import static java.lang.foreign.ValueLayout.JAVA_LONG;

public class JBalloonFFM {
    private static final int MADV_DONTNEED = 4;
    private static final int PAGE_SIZE = 4096; // We should really get this with sysconf

    private static final int SYS_userfaultfd = (int)323L; // sys/syscall.h / x86_64
    private static final int O_CLOEXEC = 0x80000;         // fcntl.h / x86_64
    private static final int O_NONBLOCK = 0x800;          // fcntl.h / x86_64
    private static final int POLLIN = 0x1;                // asm-generic/poll.h / x86_64

    private static final GroupLayout uffdio_api = MemoryLayout.structLayout(
            JAVA_LONG.withName("api"),
            JAVA_LONG.withName("features"),
            JAVA_LONG.withName("ioctls")
    ).withName("uffdio_api");
    private static final VarHandle apiHandle = uffdio_api.varHandle(PathElement.groupElement("api"));
    private static final VarHandle featuresHandle = uffdio_api.varHandle(PathElement.groupElement("features"));
    private static final VarHandle ioctlsHandle = uffdio_api.varHandle(PathElement.groupElement("ioctls"));

    private static final GroupLayout uffdio_range = MemoryLayout.structLayout(
            JAVA_LONG.withName("start"),
            JAVA_LONG.withName("len")
    ).withName("uffdio_range");

    private static final GroupLayout uffdio_register = MemoryLayout.structLayout(
            uffdio_range.withName("range"),
            JAVA_LONG.withName("mode"),
            JAVA_LONG.withName("ioctls")
    ).withName("uffdio_register");
    private static final VarHandle registerStartHandle = uffdio_register.varHandle(PathElement.groupElement("range"),
            PathElement.groupElement("start"));
    private static final VarHandle registerLenHandle = uffdio_register.varHandle(PathElement.groupElement("range"),
            PathElement.groupElement("len"));
    private static final VarHandle registerModeHandle = uffdio_register.varHandle(PathElement.groupElement("mode"));

    private static final GroupLayout uffdio_zeropage = MemoryLayout.structLayout(
            uffdio_range.withName("range"),
            JAVA_LONG.withName("mode"),
            JAVA_LONG.withName("zeropage")
    ).withName("uffdio_zeropage");
    private static final VarHandle zeropageStartHandle = uffdio_zeropage.varHandle(PathElement.groupElement("range"),
            PathElement.groupElement("start"));
    private static final VarHandle zeropageLenHandle = uffdio_zeropage.varHandle(PathElement.groupElement("range"),
            PathElement.groupElement("len"));
    private static final VarHandle zerpageModeHandle = uffdio_zeropage.varHandle(PathElement.groupElement("mode"));

    private static final GroupLayout uffdio_copy = MemoryLayout.structLayout(
            JAVA_LONG.withName("dst"),
            JAVA_LONG.withName("src"),
            JAVA_LONG.withName("len"),
            JAVA_LONG.withName("mode"),
            JAVA_LONG.withName("copy")
    ).withName("uffdio_copy");

    private static final GroupLayout uffd_msg = MemoryLayout.structLayout(
            JAVA_BYTE.withName("event"),
            JAVA_BYTE.withName("reserved1"),
            JAVA_CHAR.withName("reserved2"),
            JAVA_INT.withName("reserved3"),
            MemoryLayout.unionLayout(
                    // For now we only need the `pagefault` version of `uffd_msg.arg`
                    MemoryLayout.structLayout(
                            JAVA_LONG.withName("flags"),
                            JAVA_LONG.withName("address"),
                            JAVA_INT.withName("ptid")
                    ).withName("pagefault")
            ).withName("arg")
    ).withName("uffd_msg");
    private static final VarHandle eventHandle = uffd_msg.varHandle(PathElement.groupElement("event"));
    private static final VarHandle addressHandle = uffd_msg.varHandle(PathElement.groupElement("arg"),
            PathElement.groupElement("pagefault"), PathElement.groupElement("address"));

    // From asm-generic/ioctl.h
    private static final int _IOC_NONE = 0;
    private static final int _IOC_WRITE = 1;
    private static final int _IOC_READ = 2;
    private static final int _IOC_NRBITS = 8;
    private static final int _IOC_TYPEBITS = 8;
    private static final int _IOC_SIZEBITS = 14;
    private static final int _IOC_NRSHIFT = 0;
    private static final int _IOC_TYPESHIFT = _IOC_NRSHIFT+_IOC_NRBITS;
    private static final int _IOC_SIZESHIFT = _IOC_TYPESHIFT+_IOC_TYPEBITS;
    private static final int _IOC_DIRSHIFT = _IOC_SIZESHIFT+_IOC_SIZEBITS;
    private static long _IOC(long dir, long type, long nr, long size) {
        return ((dir << _IOC_DIRSHIFT) | (type << _IOC_TYPESHIFT) | (nr << _IOC_NRSHIFT) | (size << _IOC_SIZESHIFT));
    }
    private static long _IOWR(long type, long nr, long size) {
        return _IOC(_IOC_READ | _IOC_WRITE, type, nr, size);
    }

    // From linux/userfaultfd.h / x86_64
    private static final long UFFDIO = 0xaa;
    private static final long _UFFDIO_API = 0x3F;
    private static final long UFFDIO_API = _IOWR(UFFDIO, _UFFDIO_API, uffdio_api.byteSize());
    private static final long _UFFDIO_REGISTER = 0x0;
    private static final long UFFDIO_REGISTER = _IOWR(UFFDIO, _UFFDIO_REGISTER, uffdio_register.byteSize());
    private static final long UFFDIO_REGISTER_MODE_MISSING = 0x1;
    private static final long _UFFDIO_ZEROPAGE = 0x04;
    private static final long UFFDIO_ZEROPAAGE = _IOWR(UFFDIO, _UFFDIO_ZEROPAGE, uffdio_zeropage.byteSize());
    private static final byte UFFD_EVENT_PAGEFAULT = 0x12;

    private static long uffd;
    private static MethodHandle ioctl;
    private static MethodHandle perror;
    private static MethodHandle memmove;
    private static MethodHandle madvise;
    private static MethodHandle poll;
    private static MethodHandle read;

    private static final GroupLayout pollfd = MemoryLayout.structLayout(
            JAVA_INT.withName("fd"),
            JAVA_CHAR.withName("events"),
            JAVA_CHAR.withName("revents"));
    private static final VarHandle fdHandle = pollfd.varHandle(PathElement.groupElement("fd"));
    private static final VarHandle eventsHandle = pollfd.varHandle(PathElement.groupElement("events"));
    private static final VarHandle reventsHandle = pollfd.varHandle(PathElement.groupElement("revents"));

    private static void userfaultfdHandler() {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment pollFd = arena.allocate(pollfd);
            MemorySegment uffdMsg = arena.allocate(uffd_msg);
            MemorySegment uffdioZeropage = arena.allocate(uffdio_zeropage);
            while (true) {
                fdHandle.set(pollFd, 0, (int)uffd);
                eventsHandle.set(pollFd, 0, (char)POLLIN);
                int ret = (int)poll.invokeExact(pollFd, 1L, -1);
                if (ret == -1) {
                    System.err.println("poll() failed");
                    perror.invokeExact(MemorySegment.NULL);
                    return;
                }
                ret = (int)read.invokeExact((int)uffd, uffdMsg, uffdMsg.byteSize());
                if (ret == 0) {
                    System.err.println("EOF on userfaultfd");
                    return;
                } else if (ret == -1) {
                    System.err.println("read() failed");
                    perror.invokeExact(MemorySegment.NULL);
                    return;
                }
                long address = (long)addressHandle.get(uffdMsg, 0);
                System.err.println("Event = " + eventHandle.get(uffdMsg, 0) + " address = " + address);
                zeropageStartHandle.set(uffdioZeropage, 0, (address / PAGE_SIZE) * PAGE_SIZE);
                zeropageLenHandle.set(uffdioZeropage, 0, (long)PAGE_SIZE);
                zerpageModeHandle.set(uffdioZeropage, 0, 0L);
                if ((int)ioctl.invokeExact((int)uffd, UFFDIO_ZEROPAAGE, uffdioZeropage) == -1) {
                    System.err.println("ioctl(UFFDIO_ZEROPAAGE) failed");
                    perror.invokeExact(MemorySegment.NULL);
                }
            }
        } catch (Throwable t) {
            t.printStackTrace(System.err);
            return;
        }
    }

    private void uffdRegister(long address, long length) {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment uffdioRegister = arena.allocate(uffdio_register);
            registerStartHandle.set(uffdioRegister, 0, address);
            registerLenHandle.set(uffdioRegister, 0, length);
            registerModeHandle.set(uffdioRegister, 0, UFFDIO_REGISTER_MODE_MISSING);
            if ((int)ioctl.invokeExact((int)uffd, UFFDIO_REGISTER, uffdioRegister) == -1) {
                System.err.println("ioctl(UFFDIO_REGISTER) failed");
                perror.invokeExact(MemorySegment.NULL);
            }
        } catch (Throwable t) {
            t.printStackTrace(System.err);
        }
    }

    private static void init() {
        Linker linker = Linker.nativeLinker();
        SymbolLookup stdlib = linker.defaultLookup();

        try {
            // void perror(char*)
            perror = stdlib.find("perror")
                    .map(addr -> linker.downcallHandle(addr,
                            FunctionDescriptor.ofVoid(ADDRESS)))
                    .orElseThrow();
            // long syscall(long number, ...)
            MethodHandle syscall = stdlib.find("syscall")
                    .map(addr -> linker.downcallHandle(addr,
                            FunctionDescriptor.of(JAVA_LONG, JAVA_INT, JAVA_INT)))
                    .orElseThrow();
            uffd = (long) syscall.invokeExact(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK);
            if (uffd == -1) {
                System.err.println("syscall(SYS_userfaultfd) failed");
                perror.invokeExact(MemorySegment.NULL);
                return;
            }
        } catch (Throwable t) {
            t.printStackTrace(System.err);
            return;
        }

        try {
            // int poll(struct pollfd *fds, nfds_t nfds, int timeout)
            poll = stdlib.find("poll")
                    .map(addr -> linker.downcallHandle(addr,
                            FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_LONG, JAVA_INT)))
                    .orElseThrow();
            // ssize_t read(int fd, void* buf, size_t count)
            read = stdlib.find("read")
                    .map(addr -> linker.downcallHandle(addr,
                            FunctionDescriptor.of(JAVA_LONG, JAVA_INT, ADDRESS, JAVA_LONG)))
                    .orElseThrow();
        } catch (Throwable t) {
            t.printStackTrace(System.err);
            return;
        }

        try {
            // void* memmove(void* dest, const void* src, size_t n)
            memmove = stdlib.find("memmove")
                    .map(addr -> linker.downcallHandle(addr,
                            FunctionDescriptor.of(ADDRESS, ADDRESS, ADDRESS, JAVA_LONG),
                            Linker.Option.critical(true)))
                    .orElseThrow();
            // int madvise(void* addr, size_t length, int advice)
            madvise = stdlib.find("madvise")
                    .map(addr -> linker.downcallHandle(addr,
                            FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_LONG, JAVA_INT),
                            Linker.Option.critical(true)))
                    .orElseThrow();
            // int ioctl(int fd, unsigned long request, ...)
            ioctl = stdlib.find("ioctl")
                    .map(addr -> linker.downcallHandle(addr,
                            FunctionDescriptor.of(JAVA_INT, JAVA_INT, JAVA_LONG, ADDRESS)))
                    .orElseThrow();
            try (Arena arena = Arena.ofConfined()) {
                MemorySegment uffdApi = arena.allocate(uffdio_api);
                apiHandle.set(uffdApi, 0, UFFDIO);
                featuresHandle.set(uffdApi, 0, 0);
                if ((int)ioctl.invokeExact((int)uffd, UFFDIO_API, uffdApi) == -1) {
                    System.err.println("ioctl(UFFDIO_API) failed");
                    perror.invokeExact(MemorySegment.NULL);
                    return;
                }
                System.out.println("Uffd features supported by the kernel: " + featuresHandle.get(uffdApi, 0));

                Thread userfaultfdHandlerThread = new Thread(JBalloonFFM::userfaultfdHandler);
                userfaultfdHandlerThread.setDaemon(true);
                userfaultfdHandlerThread.start();
            }
        } catch (Throwable t) {
            t.printStackTrace(System.err);
            return;
        }
    }

    static {
        init();
    }

    private static JBalloonFFM jBalloon;

    public static JBalloonFFM getInstance() {
        if (uffd <= 0 || ioctl == null) {
            // Initialization error
            return null;
        }
        if (jBalloon == null) {
            jBalloon = new JBalloonFFM();
        }
        return jBalloon;
    }

    // See jdk.internal.util.ArraysSupport.MAX_ARRAY_LENGTH
    private static final int MAX_ARRAY_LENGTH = Integer.MAX_VALUE - 8;
    private static HashMap<Balloon, Object> balloons = new HashMap<>();

    public static class Balloon {
        private WeakReference<byte[]> array;
        private long size;
        private boolean weak;
        private Balloon(byte[] array, long size, boolean weak) {
            this.size = size;
            this.weak = weak;
            this.array = new WeakReference<byte[]>(array);
        }
        public long size() {
            return size;
        }
    }

    public Balloon inflate(int size) throws OutOfMemoryError {
        return inflate(size, false);
    }

    public synchronized Balloon inflate(int size, boolean weak) throws OutOfMemoryError {
        int chunkSize = Integer.min(size + PAGE_SIZE, MAX_ARRAY_LENGTH);
        byte[] balloonArray = new byte[chunkSize];
        MemorySegment ballonMS = MemorySegment.ofArray(balloonArray);
        try {
            // memmove() will return the real physical address of the first element in ballonMS.
            MemorySegment ballonAddress = (MemorySegment) memmove.invokeExact(ballonMS, ballonMS, 0L);
            long physicalAddress = ballonAddress.address();
            // Align the new start offset to a page boundary
            long offset = (PAGE_SIZE - (physicalAddress % PAGE_SIZE));
            // Make length a multiple of PAGE_SIZE
            long length = ((chunkSize - offset) / PAGE_SIZE) * PAGE_SIZE;
            MemorySegment alignedSegment = ballonMS.asSlice(offset, length);
            int result = (int) madvise.invokeExact(alignedSegment, length, MADV_DONTNEED);
            if (result == 0) {
                // Now  we know that `length` bytes from `balloonArray` were returned to the OS
                Balloon balloon = new Balloon(balloonArray, length, weak);
                // Save the array in the `balloons` HashMap.
                balloons.put(balloon, weak ? new WeakReference<byte[]>(balloonArray) : balloonArray);
                uffdRegister(physicalAddress + offset, length);
                // And return the key to it to the user.
                return balloon;
            } else {
                System.err.println("madvise() failed");
                perror.invokeExact(MemorySegment.NULL);
            }
        } catch (Throwable t) {
            t.printStackTrace(System.err);
        }
        // Fallthrough if anything went wrong
        return null;
    }

    public synchronized boolean deflate(Balloon b) {
        if (balloons.containsKey(b)) {
            if (b.weak) {
                ((WeakReference<byte[]>)balloons.get(b)).clear();
            }
            b.array.clear();
            balloons.remove(b);
            return true;
        } else {
            return false;
        }
    }

    public synchronized void deflateAll() {
        balloons.forEach((b, o) -> deflate(b));
    }

    public synchronized long size() {
        return balloons.keySet().stream().mapToLong(Balloon::size).sum();
    }
}
