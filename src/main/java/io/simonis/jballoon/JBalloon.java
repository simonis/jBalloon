package io.simonis.jballoon;

import com.sun.management.HotSpotDiagnosticMXBean;
import com.sun.management.VMOption;

import java.io.FileNotFoundException;
import java.io.IOException;
import java.io.InputStream;
import java.lang.management.ManagementFactory;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.util.Collections;
import java.util.HashSet;
import java.util.logging.Handler;
import java.util.logging.Level;
import java.util.logging.Logger;
import java.util.Set;

/**
 * A utility class which allows Java code to instantly release a part of the Java heap memory
 * to the OS and reclaim it back whenever necessary. Releasing memory from the Java heap to the OS
 * (i.e. "inflating" a balloon) happens instantly while re-allocation that memory back to the Java
 * heap (i.e. "deflating" a balloon) happens lazily.
 * <p>
 * It is possible to create an arbitrary number of inflated balloons as long as there is enough
 * unused memory in the Java heap.
 * <p>
 * The intended use cases for {@code JBalloon} are applications where the JVM is running with a large
 * Java heap which consumes almost all of the available native memory, but from time to time they either
 * have to allocate a significant amount of native, off-heap memory (e.g. through
 * <a href="https://docs.oracle.com/en/java/javase/25/docs/api/java.base/java/nio/ByteBuffer.html">
 * NIO ByteBuffers</a>), use the <a href="https://docs.oracle.com/en/java/javase/25/docs/specs/jni/index.html">
 * JNI</a> or <a href="https://openjdk.org/jeps/454">Foreign Function &amp; Memory API</a> with a high
 * native memory demand or have to execute native sub-programs which themselves require a considerable
 * amount of native memory either directly or e.g. through <a href="https://openjdk.org/projects/detroit/">
 * Project Detroit</a> or <a href="https://openjdk.org/projects/babylon/">Project Babylon</a>.
 * <p>
 * The name {@code JBalloon} is inspired by the
 * <a href="https://docs.oasis-open.org/virtio/virtio/v1.4/virtio-v1.4.html">Virtual I/O Device (VIRTIO)</a>'s
 * <a href="https://docs.oasis-open.org/virtio/virtio/v1.4/virtio-v1.4.html#x1-4220005">Memory Balloon Device</a>.
 * 
 * Basic usage of {@code JBalloon} looks as follows:
 *
 * <pre>{@code
 * JBalloon.Balloon balloon;
 * JBalloon jBalloon = JBalloon.getInstance();
 * if (jBalloon != null) {
 *     // Try to release 128mb heap memory back to the OS.
 *     balloon = jBalloon.inflate(128*1024*1024);
 *     if (balloon != null) {
 *         // Now we returned 128mb from the Java Heap back to the OS.
 *         ... // Execute some native code.
 *         jBalloon.deflate(balloon);
 *         // At this point Java can re-use the 128mb again.
 *     }
 * }
 * }</pre>
 */
public class JBalloon {

    private static JBalloon jBalloon;

    private static final Logger logger = Logger.getLogger(JBalloon.class.getName());

    private static boolean useCompressedOops;
    private static boolean useCompressedClassPointers;
    private static boolean compactObjectHeadersVM;
    private static boolean useCompactObjectHeaders;
    private static int objectHeaderSize;
    private static String gc;
    private static long regionSize;
    private static int javaVersion = Runtime.version().feature();

    private static native boolean nativeInit(int javaVersion, boolean useCompressedOops, boolean useCompressedClassPointers,
                                             boolean compactObjectHeadersVM, boolean useCompactObjectHeaders,
                                             int objectHeaderSize, String gc, long regionSize);

    private static void initJvmArgs() {
        HotSpotDiagnosticMXBean hsDiagnosticBean = ManagementFactory.getPlatformMXBean(HotSpotDiagnosticMXBean.class);
        VMOption option = hsDiagnosticBean.getVMOption("UseCompressedOops");
        useCompressedOops = Boolean.parseBoolean(option.getValue());
        try {
            option = hsDiagnosticBean.getVMOption("UseCompressedClassPointers");
            useCompressedClassPointers = Boolean.parseBoolean(option.getValue());
        } catch (IllegalArgumentException iae) {
            // 'UseCompressedClassPointers' was removed in JDK 27 and is the default now.
            useCompressedClassPointers = true;
        }
        try {
            option = hsDiagnosticBean.getVMOption("UseCompactObjectHeaders");
            compactObjectHeadersVM = true;
            useCompactObjectHeaders = Boolean.parseBoolean(option.getValue());
        } catch (IllegalArgumentException iae) {
            compactObjectHeadersVM = false;
            useCompactObjectHeaders = false;
        }
        option = hsDiagnosticBean.getVMOption("UseG1GC");
        if (Boolean.parseBoolean(option.getValue())) {
            gc = "G1";
            option = hsDiagnosticBean.getVMOption("G1HeapRegionSize");
            regionSize = Long.parseLong(option.getValue());
        } else {
            option = hsDiagnosticBean.getVMOption("UseShenandoahGC");
            if (Boolean.parseBoolean(option.getValue())) {
                gc = "Shenandoah";
                // Unfortunately, ShenandoahRegionSize is an experimental option and only
                // availabe through the HotSpotDiagnosticMXBean if experimental options
                // were enabled on the command line.
                option = hsDiagnosticBean.getVMOption("UnlockExperimentalVMOptions");
                if (Boolean.parseBoolean(option.getValue())) {
                    option = hsDiagnosticBean.getVMOption("ShenandoahRegionSize");
                    regionSize = Long.parseLong(option.getValue());
                } else {
                    // ToDo: use sun.management.Flag::getVMOption() reflectively
                    // or vmStructs/JVMFlag::flags[] in native code
                    regionSize = 1024 * 1024;
                }
            } else {
                option = hsDiagnosticBean.getVMOption("UseParallelGC");
                if (Boolean.parseBoolean(option.getValue())) {
                    gc = "Parallel";
                    regionSize = 1024 * 1024; // Not really the region size..
                } else {
                    gc = null; // Unsupported GC
                }
            }
        }
        objectHeaderSize = useCompactObjectHeaders ? 8 :
                useCompressedClassPointers ? 12 : 16;
    }

    private static void init() {
        // Setup the logger first.
        // %1=Date, %2=Source, %3=Logger, %4=Level, %5=Message, %6=Throwable
        String format = System.getProperty("java.util.logging.SimpleFormatter.format", "%5$s%6$s%n");
        System.setProperty("java.util.logging.SimpleFormatter.format", format);
        Level logLevel = Level.parse(System.getProperty("io.simonis.jballoon.logLevel", "INFO").toUpperCase());
        Logger root = Logger.getLogger("");
        root.setLevel(logLevel);
        for (Handler h : root.getHandlers()) {
            h.setLevel(logLevel);
        }
        // jBalloon only works if loaded from the one of the three default class loaders.
        // This is required in order to avoid that the class gets unloaded without cleaning
        // up the native machinery which ensures that inflated balloons survive GC events.
        ClassLoader cl = JBalloon.class.getClassLoader();
        if (!(cl == null || cl == ClassLoader.getSystemClassLoader() || cl == ClassLoader.getPlatformClassLoader())) {
            logger.severe("JBalloon only works if loaded from the system class loader but loaded by " + cl.getName());
            return;
        }
        initJvmArgs();
        String libName = "/native/libjballoon.so";
        try {
            Path temp = Files.createTempFile("libjballoon", ".so");
            try (InputStream is = JBalloon.class.getResourceAsStream(libName)) {
                if (is == null) throw new FileNotFoundException("Expected to find '" + libName + "' in jar file");
                Files.copy(is, temp, StandardCopyOption.REPLACE_EXISTING);
            }
            System.load(temp.toAbsolutePath().toString());
            temp.toFile().deleteOnExit();
        } catch (IOException ioe) {
            logger.log(Level.SEVERE, "Can't load: " + libName, ioe);
        }
        if (nativeInit(javaVersion, useCompressedOops, useCompressedClassPointers,
                       compactObjectHeadersVM, useCompactObjectHeaders,
                       objectHeaderSize, gc, regionSize)) {
            jBalloon = new JBalloon();
        }
    }

    static {
        init();
    }

    private JBalloon() {
    }

    /**
     * Initializes and returns the singleton {@code JBalloon} instance. If {@code JBalloon}
     * can't be initialized, {@code null} will be returned. In such cases you can enable the
     * {@code DEBUG} log level of {@code JBalloon}'s native library by setting the environment
     * variable {@code LOG=DEBUG} to get more information about why the initialization failed.
     *
     * @return the {@code JBalloon} singleton instance or {@code null} if {@code JBalloon}
     * can't be initialized.
     */
    public static JBalloon getInstance() {
        return jBalloon;
    }

    private static Set<Balloon> balloons = Collections.synchronizedSet(new HashSet<Balloon>());

    /**
     * A lightweight handle class for managing memory balloons. This class
     * holds an internal {@code long[]} array which backs up the memory balloon.
     * <p>
     * {@code Balloon} instances are created automatically by {@link JBalloon} on every
     * successful call to {@link JBalloon#inflate(long)} or {@link JBalloon#inflate(long[])}
     * and have to be kept alive if the application plans to eventually deflate the balloon
     * again by passing the {@code Balloon} object to {@link JBalloon#deflate(Balloon)}.
     * <p>
     * The backing {@code long[]} of a {@code Balloon} only becomes accessible once the
     * {@code Balloon} is deflated (see {@link JBalloon#deflate(Balloon)}). 
     */
    public static class Balloon {
        private long[] balloon;
        private long balloonSize;

        private Balloon(long[] balloon, long balloonSize) {
            this.balloon = balloon;
            this.balloonSize = balloonSize;
        }

        /**
         * Returns the size in bytes occupied by the {@code long[]} which backs up
         * this {@code Balloon}. This is typically larger than {@link #balloonSize()}.
         * 
         * @return the size in bytes of the backing {@code long[]} or {@code 0} if this
         *         {@code Balloon} is already deflated.
         */
        public long heapSize() {
            return balloon == null ? 0 : balloon.length * 8;
        }

        /**
         * Returns the inflated size in bytes of this {@code Balloon}.
         * This is usually smaller than {@link #heapSize()}.
         * 
         * @return the inflated size in bytes of this {@code Balloon} or {@code 0} if this
         *         {@code Balloon} is already deflated.
         */
        public long balloonSize() {
            return balloon == null ? 0 : balloonSize;
        }

        public String toString() {
            if (balloon == null) {
                return String.format("Balloon(deflated)");
            } else {
                return String.format("Balloon(%d, %d)", heapSize(), balloonSize());
            }
        }
    }

    private static native Balloon inflateNative(long[] array);
    private static native Balloon inflateNative(long size);

    /**
     * Creates and inflates a new {@link JBalloon.Balloon} with {@code size} rounded
     * up to such that it spans at least one system page size (i.e. {@code getconf PAGE_SIZE})
     * and fills up all the GC regions it spans.
     * <p>
     * The {@code Balloon} is backed up by a {@code long[]} so the balloon can't
     * get larger than 8 * {@link java.lang.Integer#MAX_VALUE} bytes.
     * <p>
     * The {@link JBalloon.Balloon#heapSize()} and {@link JBalloon.Balloon#balloonSize()}
     * methods can be used to query the actual size in bytes of the backing array and
     * the size of the inflated balloon.
     * <p>
     * If the {@code Balloon} can't be created ot inflated, this method returns
     * {@code null}, otherwise the newly created {@code Balloon} is returned.
     * 
     * @param  size the size in bytes of the new {@link JBalloon.Balloon}.
     * @return the new {@code Balloon} or {@code null} if a {@code Balloon} of
     *         size {@code size} couldn't be created or inflated.
     * @throws OutOfMemoryError if the backing {@code long[]} can't be allocated.
     */
    public synchronized Balloon inflate(long size) throws OutOfMemoryError {
        logger.fine("JBalloon::inflate(" + size + ")");
        Balloon balloon = inflateNative(size);
        if (balloon != null) {
            balloons.add(balloon);
        }
        logger.fine("JBalloon::inflate(" + size + ") -> " + balloon);
        return balloon;
    }

    /**
     * Creates and inflates a new {@link JBalloon.Balloon} backed up by {@code array}.
     * <p>
     * Once this method returns, {@code array} shouldn't be used any more because its
     * content can be overwritten at any time. Also, writing into {@code array} after
     * this method returns will deflate parts of the created {@code Balloon}.
     * <p>
     * The recommended usage pattern for this method is to only use arrays as input
     * argument which have been returned by {@link #deflate(Balloon)}.
     * <p>
     * The {@link JBalloon.Balloon#balloonSize()} method can be used to query the
     * the actual size in bytes of the inflated balloon.
     * <p>
     * If the {@code Balloon} can't be inflated or if the {@code array} is too small, this
     * method returns {@code null}, otherwise the newly created {@code Balloon} is returned.
     * 
     * @param  array a {@code long[]} to back up the new {@code Balloon}.
     * @return the new {@code Balloon} or {@code null} if the {@code array} is too small
     *         or the {@code Balloon} couldn't be inflated.
     */
    public synchronized Balloon inflate(long[] array) {
        logger.fine("JBalloon::inflate(long[" + array.length + "])");
        Balloon balloon = inflateNative(array);
        if (balloon != null) {
            balloons.add(balloon);
        }
        logger.fine("JBalloon::inflate(long[" + array.length + "]) -> " + balloon);
        return balloon;
    }

    private static native void deflateNative(long[] array);

    /**
     * Deflates the {@code Balloon} {@code b} and returns the backing {@code long[]}.
     * <p>
     * This method doesn't deflate {@code b} instantly. Instead, deflation will only happen
     * when the GC collects the returned {@code long[]}. This won't happen as long as the returned
     * array is kept alive by the application.
     * <p>
     * The recommended usage pattern for this method is to either ignore the returned {@code long[]}
     * or to store it into a {@link java.util.WeakHashMap} if the application expects to inflate a
     * new {@code Balloon} of similar size any time soon. In the latter case, the array can be retrieved
     * from the {@link java.util.WeakHashMap} if it hasn't been collected by the GC and passed as argument
     * to {@link #inflate(long[])}.
     * 
     * @param  b the {@code Balloon} which will be deflated.
     * @return the backing {@code long[]} array of {@code b} or {@code null} if {@code b}
     *         was already deflated.
     */
    public synchronized long[] deflate(Balloon b) {
        logger.fine("JBalloon::deflate(" + b + ")");
        if (balloons.contains(b)) {
            long[] array = b.balloon;
            b.balloon = null;
            deflateNative(array);
            balloons.remove(b);
            logger.fine("JBalloon::deflate(" + b + ") -> long[" + array.length + "]");
            return b.balloon;
        } else {
            logger.warning("JBalloon::deflate(" + b + ") -> NULL");
            return null;
        }
    }

    /**
     * Deflates all currently active {@code Balloon}s.
     */
    public synchronized void deflateAll() {
        balloons.forEach(b -> deflate(b));
    }

    /**
     * Returns the size in bytes occupied by all the {@code long[]} which back up
     * active (i.e. inflated) {@code Balloon}s. This is typically larger than
     * {@link #balloonSize()}.
     * 
     * @return the size in bytes of all backing {@code long[]}.
     */
    public synchronized long balloonHeapSize() {
        return balloons.stream().mapToLong(Balloon::heapSize).sum();
    }

    /**
     * Returns the size in bytes of all active (i.e. inflated) {@code Balloon}s.
     * This is usually smaller than {@link #balloonHeapSize()}.
     * 
     * @return the size in bytes of all inflated {@code Balloon}s.
     */
    public synchronized long balloonSize() {
        return balloons.stream().mapToLong(Balloon::balloonSize).sum();
    }

    /**
     * Returns the region size of the current Garbage Collector.
     * 
     * @return the GC's region size in bytes. 
     */
    public static long regionSize() {
        return regionSize;
    }

    /**
     * Returns the object header size of a Java object.
     * 
     * @return the object header size in bytes.
     */
    public static long objectHeaderSize() {
        return objectHeaderSize;
    }

    /**
     * Returns the default system page size (i.e. {@code getconf PAGE_SIZE}) in bytes.
     * 
     * @return the default system page size in bytes.
     */
    public static native long pageSize();

    /**
     * Returns the Java object heap size in bytes.
     * 
     * @return the heap size size in bytes.
     */
    public static native long heapSize();

    /**
     * Returns the base address of the Java object heap.
     * 
     * @return the Java heap's base address.
     */
    public static native long heapBase();

    /**
     * Returns the offset in bytes of the first element in a {@code long[]} relatively
     * to the arrays address in the Java heap. This includes the {@link #objectHeaderSize()}
     * and potential alignment gaps.
     * 
     * @return the offset in bytes of the first {@code long[]} element relatively to the {@code long[]} object.
     */
    public static native long longArrayOffset();
    
    /**
     * Returns the size in bytes occupied by the Java heap as returned by the
     * <a href="https://man7.org/linux/man-pages/man2/mincore.2.html">mincore(2)</a> C-library call. Notice
     * that {@code mincore()} result counts each page in the heap that is backed up by the kernel's zero
     * page as "resident" although such pages don't actually consume any physical pages.
     * 
     * @return the heap size in bytes as counted by {@code mincore()}.
     * 
     */
    public static native long mincoreHeapSize();

    /**
     * Returns the size in bytes occupied by the Java heap as accounted for in the
     * {@code /proc/self/pagemap} file. This doesn't account for zero pages in the heap
     * and is therefore more accurate than {@link #mincoreHeapSize()}.
     * 
     * @return the heap size in bytes as accounted for in {@code /proc/self/pagemap}.
     */
    public static native long rssHeapSize();
}
