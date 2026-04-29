// This test is based on OpenJDK's TestAllocHumongousFragment.java JTreg test

package io.simonis.jballoon.test;

import io.simonis.jballoon.JBalloon;

import java.util.ArrayList;
import java.util.List;
import java.util.Random;

public class HumongousFragmentationTest {
    static final long TARGET_MB = Long.getLong("target", 30_000); // 30 Gb allocations
    static final long LIVE_MB   = Long.getLong("occupancy", 700); // 700 Mb alive

    static volatile Object sink;

    static List<int[]> objects;

    public static void main(String[] args) throws Exception {
        final int min = 128 * 1024;
        final int max = 16 * 1024 * 1024;
        final long count = TARGET_MB * 1024 * 1024 / (16 + 4 * (min + (max - min) / 2));

        objects = new ArrayList<>();
        long current = 0;
        JBalloon jBalloon = JBalloon.getInstance();

        Random rng = new Random(Integer.getInteger("seed", 42));
        for (long c = 0; c < count; c++) {
            while (current > LIVE_MB * 1024 * 1024) {
                int idx = rng.nextInt(objects.size());
                int[] remove = objects.remove(idx);
                current -= remove.length * 4 + 16;
            }

            int[] newObj = new int[min + rng.nextInt(max - min)];
            current += newObj.length * 4 + 16;
            objects.add(newObj);
            sink = new Object();

            if (c == count / 10 && jBalloon != null) {
                JBalloon.Balloon balloon = jBalloon.inflate(Integer.getInteger("balloonSize", 32*1024*1024));
            }

            System.out.println("Allocated: " + (current / 1024 / 1024) + " Mb");
        }
    }
}
