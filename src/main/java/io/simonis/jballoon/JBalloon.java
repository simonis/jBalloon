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
import java.util.concurrent.ConcurrentHashMap;
import java.util.logging.Handler;
import java.util.logging.Level;
import java.util.logging.Logger;

public class JBalloon {

    private static JBalloon jBalloon;
    private static final int MAX_NR_OF_BALLOONS = 10;
    private static int nr_of_ballons = 0;

    private static final Logger logger = Logger.getLogger(JBalloon.class.getName());

    private static boolean useCompressedOops;
    private static boolean useCompressedClassPointers;
    private static boolean compactObjectHeadersVM;
    private static boolean useCompactObjectHeaders;
    private static int objectHeaderSize;
    private static String gc;
    private static long regionSize;

    private static native boolean nativeInit(boolean useCompressedOops, boolean useCompressedClassPointers,
                                             boolean compactObjectHeadersVM, boolean useCompactObjectHeaders,
                                             int objectHeaderSize, String gc, long regionSize);

    private static void initJvmArgs() {
        HotSpotDiagnosticMXBean hsDiagnosticBean = ManagementFactory.getPlatformMXBean(HotSpotDiagnosticMXBean.class);
        VMOption option = hsDiagnosticBean.getVMOption("UseCompressedOops");
        useCompressedOops = Boolean.parseBoolean(option.getValue());
        option = hsDiagnosticBean.getVMOption("UseCompressedClassPointers");
        useCompressedClassPointers = Boolean.parseBoolean(option.getValue());
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
                option = hsDiagnosticBean.getVMOption("ShenandoahRegionSize");
                regionSize = Long.parseLong(option.getValue());
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
        // %1=Date, %2=Source, %3=Logger, %4=Level, %5=Messagejni, %6=Throwable
        String format = System.getProperty("java.util.logging.SimpleFormatter.format", "%5$s%6$s%n");
        System.setProperty("java.util.logging.SimpleFormatter.format", format);
        Level logLevel = Level.parse(System.getProperty("logLevel", "INFO").toUpperCase());
        Logger root = Logger.getLogger("");
        root.setLevel(logLevel);
        for (Handler h : root.getHandlers()) {
            h.setLevel(logLevel);
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
        if (nativeInit(useCompressedOops, useCompressedClassPointers,
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

    public static JBalloon getInstance() {
        return jBalloon;
    }

    // See jdk.internal.util.ArraysSupport.MAX_ARRAY_LENGTH
    private static final int MAX_ARRAY_LENGTH = Integer.MAX_VALUE - 8;
    private static ConcurrentHashMap<Balloon, byte[]> balloons = new ConcurrentHashMap<>();

    public static class Balloon {
        public static transient int PAGE_SIZE; // Set from native code in nativeInit();
        private long address;
        private int size;
        private long id;

        private Balloon(long address, int size, long id) {
            this.size = size;
            this.address = address;
            this.id = id;
        }

        public long size() {
            return size;
        }

        public String toString() {
            return String.format("Balloon(%x, %d, %d)", address, size, id);
        }
    }

    private static native Balloon inflateNative(byte[] array);

    public synchronized Balloon inflate(int size) throws OutOfMemoryError {
        logger.fine("-=-> Inflate ");
        if (nr_of_ballons >= MAX_NR_OF_BALLOONS) {
            logger.fine("<-=- Inflate (exceeded MAX_NR_OF_BALLOONS=" + MAX_NR_OF_BALLOONS + ")");
            return null;
        }
        int regionSize = 1024*1024;
        int objectHeader = 16;
        int regionAligned = (size / regionSize) * regionSize - objectHeader;
        // Let the array fully occupy a certain number of GC regions.
        size = regionAligned;
        byte[] balloonArray = new byte[size];
        for (byte i = 0; i < 10; i++) balloonArray[i] = i; // Just for debugging
        Balloon balloon = inflateNative(balloonArray);
        if (balloon != null) {
            balloons.put(balloon, balloonArray);
            nr_of_ballons++;
            logger.fine("<-=- Inflate " + balloon);
            return balloon;
        }
        logger.fine("<-=- Inflate (inflateNative() returned NULL)");
        return null;
    }

    private static native void removeNative(Balloon b, byte[] array);

    public synchronized boolean deflate(Balloon b) {
        if (balloons.containsKey(b)) {
            logger.fine("---> Deflate " + b);
            byte[] array = balloons.get(b);
            removeNative(b, array);
            balloons.remove(b);
            nr_of_ballons--;
            logger.fine("<--- Deflate " + b);
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

    public static native long heapSize();
    public static native long mincoreHeapSize();
    public static native long rssHeapSize();
}
