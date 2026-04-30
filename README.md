# jBalloon

This is an experimental library which allows Java code to instantly release a part of the Java heap memory to the OS and reclaim it back whenever necessary. It is intended for use cases where the JVM is running with a large heap which consumes all available native memory but from time to time has to execute native applications which themselves require a considerable amount of memory.

The current POC should work on Linux (I've only tried on x86_64) with G1 GC on JDK 21 & 25 (with all combinations of `-XX:+/-UseCompactObjectHeaders`, `-XX:+/-UseCompressedClassPointers` and `-XX:+/-UseCompressedOops`). I think it should be straightforward to port it to Shenandoah while ParallelGC support might require some more effort.

## How to use

```Java
import io.simonis.jballoon.JBalloon;

JBalloon jBalloon = JBalloon.getInstance();
JBalloon.Balloon balloon;
if (jBalloon != null) {
    balloon = jBalloon.inflate(32*1024*1024 /*32mb*/);
}
// Now we returned 32mb from the Java Heap back to the OS
...
if (jBalloon != null) {
    jBalloon.deflate(balloon);
}
// At this point we can re-use the 32mb for Java Heap objects
```

## How it works

Inflating a balloon works by allocating a byte array of corresponding size and immediately freeing the array's content by calling `madvise(MADV_DONTNEED)` on its memory region. By contract, nobody will ever access the array, because it is not exposed by the jBalloon API.

The balloon should be at least as large as a region of the GC in order to qualify as a "humongous" object for the GC. The contents of the backing byte array are only read during a last-ditch Full GC when the GC moves/compacts humongous objects (see [JDK-8191565: Last-ditch Full GC should also move humongous objects
](https://bugs.openjdk.org/browse/JDK-8191565)). When the GC reads the array, the kernel will page in the kernel `ZERO_PAGE` for memory regions which were previously `madvise(MADV_DONTNEED)` which doesn't consume any physical memory pages on the system. Therefore, as long as the source and destination locations of the array don't overlap, moving an object will not re-increase the memory consumption of the Java process because after copying is done, we will immediately inflate the array at its new location.

In order to get notifications when the GC starts to read from the array and when the moving operation has finished, we register the corresponding address space with the [`userfaultfd`](https://docs.kernel.org/6.3/admin-guide/mm/userfaultfd.html) file descriptor. This will invoke a call-back, each time a registered and previously `madvise(MADV_DONTNEED)` page is accessed during GC. In the callback we basically do two things: if the first page of the array is accessed, we will page in the `ZERO_PAGE` for all but the last array page. This will allow the GC to proceed until it reaches the last page. When the last page of the array is touched, we know that the copying process is about to end, so we can `madvise(MADV_DONTNEED)` the array at the destination address, which can be found by following the forwarding pointer of the source object. At the same time, we de-register the memory region used by the source array from `userfaultfd` and instead register the memory region of the new (i.e. moved) object.

Things get a little more complicated, when the source and destination locations of the array overlap. In such situations, the GC will not only read from the source array, but at some point also write into overlapping part of the source array. Although, it will only write zeros (read from the `ZERO_PAGE`), writing will still page in "real" memory pages, because the kernel is not checking what content gets written. In order to prevent situations where the whole overlapping area gets paged in (and thus a potentially significant part of the balloon gets implicitely deflated) before we have a chance to `madvise(MADV_DONTNEED)` the new, moved array, we do the following. Instead of enabling notifications only for the first and last page of a balloon array, we also enable them for the overlapping region. This allows us to selectively `madvise(MADV_DONTNEED)` pages in the overlapping area, once we're done copying it.

## Building and Testing

```terminal
$ mvn clean package
```

This will create `jballoon-1.0-SNAPSHOT.jar` and `jballoon-1.0-SNAPSHOT-tests.jar` in the target subdirectory. The former contains the `io.simonis.jballoon.jBallon` class and `libjballoon.so` which is the shared library with the native part of jBalloon. The latter contains the `io/simonis/jballoon/test/HumongousFragmentationTest` test which is based on OpenJDK's [`TestAllocHumongousFragment`](https://github.com/openjdk/jdk/blob/9cc01a9e0a2ca7dbe2650c852840e49418a742be/test/hotspot/jtreg/gc/TestAllocHumongousFragment.java) JTreg test. The test repeatedly creates a large number of humongous objects in a loop, some of which are retained for a certain amount of time while others are immediately released. This creates a highly fragmented heap and trigger frequent full GCs which will compact the humongous objects which are still alive to make room for new humongous allocations. This is the perfect scenario to test that jBalloon memory balloons remain inflated even when moved by the GC. So in order to test jBalloon, I've modified the test to inflate a balloon after 10% of the allocations and deflate it again after 90% of the test's allocations.

In order to see how jBalloon works, the test has to be run with logging enabled. Logging in the native part is controlled by the environment variable `LOG` which can be one of `OFF` (the default), `ERROR`, `WARNING`, `INFO`, `DEBUG` and `TRACE`. Java logging is controlled by the system property `logLevel` and can be any of Java's `java.util.logging` [logging Levels](https://docs.oracle.com/en/java/javase/25/docs/api/java.logging/java/util/logging/Level.html).

We first run the test with the highest native logging level `TRACE`. Notice that this will considerably increase the run time of the test, because a this logging level, the `userfaultfd` callback will print an accurate number for the memory used by the Java heap at every entry and exit which is computed from [`mincore`](https://man7.org/linux/man-pages/man2/mincore.2.html) (this includes `ZERO` pages) and from [`/proc/self/pagemap`](https://www.kernel.org/doc/Documentation/vm/pagemap.txt) (where we only count the occupied, physical pages). Because the `userfaultfd` callback gets called every time a `madvise(MADV_DONTNEED)` is touched, we can be sure that in between, the balloon remains inflated.

```terminal
$ LOG=TRACE java -Xmx1g -Xms1g -XX:+AlwaysPreTouch -XX:+UseG1GC \
                 -DballoonSize=209715200 \
                 -cp jballoon-1.0-SNAPSHOT-tests.jar:jballoon-1.0-SNAPSHOT.jar \
                 io.simonis.jballoon.test.HumongousFragmentationTest \
                 2>&1 | grep "Java Heap"
```

With a 1gb heap and 200mb balloon, this will print the following output:

```terminal
Java Heap => (reserved/mincore/RSS) = (1048576kb / 1048576kb / 1048576kb)
Java Heap -> (reserved/mincore/RSS) = (1048576kb / 843784kb / 843784kb)
Java Heap <- (reserved/mincore/RSS) = (1048576kb / 954364kb / 844796kb)
Java Heap -> (reserved/mincore/RSS) = (1048576kb / 954364kb / 844796kb)
...
Java Heap <- (reserved/mincore/RSS) = (1048576kb / 843796kb / 843796kb)
Java Heap <= (reserved/mincore/RSS) = (1048576kb / 1048576kb / 1048576kb)
```

Initially, the heap occupies 1048576kb (i.e. 1gb) (because we used `-XX:+AlwaysPreTouch` to simulate the worst case). Once the balloon is inflated, RSS drops by roughly 200mb to 843784kb. Notice, how subsequently the `mincore()` numbers increase because some of the balloon pages are touched (i.e. read) by GC, but RSS remains at ~840mb.

Once the balloon is deflated, the RSS quickly increases back to 1048576kb. Notice that with `LOG=TRACE`, this test can run for a very long time. To shorten the runtime, you can e.g. decrease the balloon size to 32mb.

We can also measure the maximum GC pause times without and with the balloon. For this we only need the plain `-Xlog:gc` logging:

```terminal
$ java -Xlog:gc -Xmx1g -Xms1g -XX:+AlwaysPreTouch -XX:+UseG1GC \
       -DballoonSize=0 \
       -cp jballoon-1.0-SNAPSHOT-tests.jar:jballoon-1.0-SNAPSHOT.jar \
       io.simonis.jballoon.test.HumongousFragmentationTest \
       | egrep -o "[0-9]+,[0-9]+ms" | sort -n | tail -3
81,494ms
82,357ms
86,793ms
```

```terminal
$ java -Xlog:gc -Xmx1g -Xms1g -XX:+AlwaysPreTouch -XX:+UseG1GC \
       -DballoonSize=209715200 \
       -cp jballoon-1.0-SNAPSHOT-tests.jar:jballoon-1.0-SNAPSHOT.jar \
       io.simonis.jballoon.test.HumongousFragmentationTest \
       | egrep -o "[0-9]+,[0-9]+ms" | sort -n | tail -3
278,729ms
278,869ms
285,667ms
```

As you can see, the maximum GC pauses increase from ~85ms to ~280ms. To understand why this is the case, we can take a look at how many times the balloon is moved (i.e. "compacted") during the test. For this it is enough to run with log level `DEBUG` which is considerably faster:

```terminal
$ LOG=DEBUG java -Xmx1g -Xms1g -XX:+AlwaysPreTouch -XX:+UseG1GC \
                 -DballoonSize=209715200 \
                 -cp jballoon-1.0-SNAPSHOT-tests.jar:jballoon-1.0-SNAPSHOT.jar \
                 io.simonis.jballoon.test.HumongousFragmentationTest \
                 2>&1 | grep "moves to"
balloon at 0xed200000 moves to 0xe8b00000 with 135258112 bytes of overlap
balloon at 0xe8b00000 moves to 0xe6400000 with 168812544 bytes of overlap
balloon at 0xe6400000 moves to 0xe0400000 with 109043712 bytes of overlap
balloon at 0xe0400000 moves to 0xda300000 with 107995136 bytes of overlap
balloon at 0xda300000 moves to 0xd3800000 with 97509376 bytes of overlap
balloon at 0xd3800000 moves to 0xd2300000 with 187686912 bytes of overlap
...
balloon at 0xc2300000 moves to 0xc1200000 with 191881216 bytes of overlap
balloon at 0xc1200000 moves to 0xc0300000 with 193978368 bytes of overlap
```

As you can see, the 200mb balloon is moved quite a few times during the execution and a lot of times a significant part of the array at the old and new location overlap, which makes it necessary to trap a lot of page faults in the overlap region. But notice that this is an extreme case that is intentionally triggered by this test case. In normal operation, humongous object are hardly ever moved by  the GC.

## Internals

TBD

## Alternatives

Ideally, it should be possible to use the manageable [`-XX:SoftMaxHeapSize`](https://malloc.se/blog/zgc-softmaxheapsize) option to dynamically control the heap size of a Java application. Unfortunately, changing `-XX:SoftMaxHeapSize` doesn't instantly change the heap consumption. Instead, it instructs the JVM to gradually decrease heap usage until it reaches the new value. In addition, the freed heap parts are not immediately returned to the OS but again in a gradual process controlled by various GC parameters. For Shenandoah, the [uncommit process is  controlled](https://mail.openjdk.org/pipermail/hotspot-gc-dev/2018-June/022204.html) by `-XX:ShenandoahUncommit`, `-XX:ShenandoahUncommitDelay` and `-XX:ShenandoahUncommitWithIdle`. G1 supports uncommitting since JDK 12 (see [JEP 346: Promptly Return Unused Committed Memory from G1](https://bugs.openjdk.org/browse/JDK-8204089) and the [CSR for Promptly Return Unused Committed Memory from G1](https://bugs.openjdk.org/browse/JDK-8212658)). In G1, uncommitting can either be done periodically (with `-XX:G1PeriodicGCInterval`) or depending on the system load (with `-XX:G1PeriodicGCSystemLoadThreshold`). Other parameters for uncommitting heap memory like [`UncommitSizeLimit`](https://github.com/openjdk/jdk/blob/ca405d0eb2a0ed63dc169aceb80512bf2a523da1/src/hotspot/share/gc/g1/g1UncommitRegionTask.hpp#L36) and [`UncommitInitialDelayMs`](https://github.com/openjdk/jdk/blob/ca405d0eb2a0ed63dc169aceb80512bf2a523da1/src/hotspot/share/gc/g1/g1UncommitRegionTask.hpp#L38) are not even exposed as command line options. ZGC supports uncommitting since JDK 13 (see [JEP 351: ZGC: Uncommit Unused Memory](https://openjdk.org/jeps/351)) with the command line options `-XX:-ZUncommit` and `-XX:ZUncommitDelay`.

Currently, most GCs treat the maximum configured heap size as a "free" resource which they eagerly consume (similar to the [Linux Page Cache](https://github.com/firmianay/Life-long-Learner/blob/master/linux-kernel-development/chapter-16.md)). but there's currently some activity to make the Java heap sizing more dynamic and let it adapt to e.g. the system load or the GC times. Among these are [JDK-8236073: G1: Use SoftMaxHeapSize to guide GC heuristics
](https://bugs.openjdk.org/browse/JDK-8236073), [JDK-8359211: Automatic Heap Sizing for G1](https://bugs.openjdk.org/browse/JDK-8359211) and [JDK-8377305: Automatic Heap Sizing for ZGC](https://bugs.openjdk.org/browse/JDK-8377305). And finally there's Google's [No More Xmx - Adaptable Heap Sizing for Containerized Java Applications](https://www.youtube.com/watch?v=qOt4vOkk49k) approach, which was presented at Devoxx Belgium in 2022.
