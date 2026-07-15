package io.simonis.jballoon.test;

import io.simonis.jballoon.JBalloon;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Paths;

public class JBalloonTest {
    private static final int PAGE_SIZE = (int)JBalloon.pageSize();
    private static volatile byte[] sink;

    private static long getRSS() {
        try {
            String statm = Files.readString(Paths.get("/proc/self/statm"));
            String[] parts = statm.trim().split("\\s+");
            if (parts.length > 1) {
                return (Long.parseLong(parts[1]) * PAGE_SIZE) / 1024;
            }
        } catch (IOException | NumberFormatException e) {
            System.err.println("Could not read RSS: " + e.getMessage());
        }
        return -1;
    }

    private static void printRSS(String msg) {
        System.out.println("RSS (" + msg + ") = " + getRSS() + "kb, Java Heap (reserved/mincore/RSS) = (" +
                (JBalloon.heapSize() / 1024) + "kb / " + (JBalloon.mincoreHeapSize() / 1024) + "kb / " +
                (JBalloon.rssHeapSize() / 1024) + "kb)");
    }

    private static void allocate(int bytes) {
        for (int i = 0; i < (bytes / 1024); i++) {
            sink = new byte[1024];
        }
    }

    public static void main(String[] args) throws InterruptedException {
        JBalloon jb = JBalloon.getInstance();
        if (jb == null) {
            System.err.println("Can't initialize JBalloon");
            return;
        }

        allocate(1024*1024*128);
        printRSS("after allocation");
        JBalloon.Balloon b1 = jb.inflate(1024*1024*32);
        printRSS("after inflating first balloon");
        JBalloon.Balloon b2 = jb.inflate(1024*1024*32);
        printRSS("after inflating second balloon");
        jb.deflate(b1);
        printRSS("after deflating first balloon");
        allocate(1024*1024*128);
        printRSS("after second allocation");
        System.out.println("Balloon size = " + jb.balloonSize()/1024 + "kb, (heap usage " + jb.balloonHeapSize()/1024 + "kb)");
        Thread.sleep(1_000);
    }
}
