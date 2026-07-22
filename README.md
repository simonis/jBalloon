# jBalloon

> [!WARNING]
> This is an experimental, unsupported and unmaintained library that only works on specific JDK versions and platforms. It currently only supports G1 GC and has only been tested with Corretto 17/21/25 on Linux x86_64/aarch64. Use it at your own risk!

*jBalloon* allows Java code to instantly release a part of the Java heap memory to the OS and reclaim it back whenever necessary. Compared to other solutions like e.g. [`-XX:SoftMaxHeapSize`](https://malloc.se/blog/zgc-softmaxheapsize), [JEP 346: Promptly Return Unused Committed Memory from G1](https://bugs.openjdk.org/browse/JDK-8204089), [Automatic Heap Sizing for G1](https://bugs.openjdk.org/browse/JDK-8359211) or simply relying on the GC to eventually [uncommit unused parts of the heap](https://simonis.io/blog/openjdk/uncommit.html), *jBalloon* can release heap memory instantaneously while re-allocation happens lazily. To achieve this, *jBalloon* heavily relies on undocumented implementation details of the HotSpot JVM.

The intended use cases for *jBalloon* are applications where the JVM is running with a large Java heap which consumes almost all of the available native memory, but from time to time they either have to allocate a significant amount of native, off-heap memory (e.g. through [NIO ByteBuffers](https://docs.oracle.com/en/java/javase/25/docs/api/java.base/java/nio/ByteBuffer.html)), use the [JNI](https://docs.oracle.com/en/java/javase/25/docs/specs/jni/index.html) or [Foreign Function & Memory API](https://openjdk.org/jeps/454) with a high native memory demand or have to execute native sub-programs which themselves require a considerable amount of native memory either directly or e.g. through [Project Detroit](https://openjdk.org/projects/detroit/) or [Project Babylon](https://openjdk.org/projects/babylon/).

The current version should work on Linux (x86_64 and aarch64) with G1 GC on JDK 17/21/25 with all valid combinations of `-XX:+/-UseCompactObjectHeaders`, `-XX:+/-UseCompressedClassPointers` and `-XX:+/-UseCompressedOops`. Supporting other GCs and platforms might be possible but will require considerable effort.

The name "*jBalloon*" is inspired by the [Virtual I/O Device (VIRTIO)](https://docs.oasis-open.org/virtio/virtio/v1.4/virtio-v1.4.html)'s [Memory Balloon Device](https://docs.oasis-open.org/virtio/virtio/v1.4/virtio-v1.4.html#x1-4220005).

## How to use

```Java
import io.simonis.jballoon.JBalloon;

JBalloon.Balloon balloon;
JBalloon jBalloon = JBalloon.getInstance();
if (jBalloon != null) {
    balloon = jBalloon.inflate(128*1024*1024 /*128mb*/);

    if (balloon != null) {
        // Now we returned 128mb from the Java Heap back to the OS.
        ... // Execute some native code.
        jBalloon.deflate(balloon);
        // At this point Java can re-use the 128mb again.
    }
}
```

For more details see the [API documentation](https://simonis.github.io/jBalloon/io/simonis/jballoon/JBalloon.html).
## How it works

Inflating a balloon works by allocating a `long[]` array of corresponding size and immediately freeing the array's content by calling `madvise(MADV_DONTNEED)` on its memory region. By contract, nobody will ever access the array, because it is not exposed by the *jBalloon* API.

`JBalloon::inflate(long size)` rounds up the `size` argument such that the underlying `long[]` array will occupy at least one full G1 region in order to qualify as a "humongous" object for the GC and to be at least as large as three system pages (i.e. `getconf PAGE_SIZE`).

The contents of the backing `long[]` are only ever accessed during a G1 last-ditch Full GC when the GC moves/compacts humongous objects (see [JDK-8191565: Last-ditch Full GC should also move humongous objects](https://bugs.openjdk.org/browse/JDK-8191565)). When the GC reads the array, the kernel will page in the kernel `ZERO_PAGE` for memory regions which were previously `madvise(MADV_DONTNEED)` which doesn't consume any physical memory pages on the system. Moreover, *jBalloon* hooks into the G1 GC code to make sure that once copied, the backing `long[]` will be immediately `madvise(MADV_DONTNEED)` at the new location in order to preserve *jBalloon*'s invariant of freed heap memory. 

## Building

```terminal
$ mvn clean package
```

This will create `jballoon-1.0-SNAPSHOT.jar` and `jballoon-1.0-SNAPSHOT-tests.jar` in the target subdirectory. The former contains the `io.simonis.jballoon.jBallon` class and `libjballoon.so` which is the shared library with the native part of *jBalloon*.

Use `mvn -DDEBUG ..` to build the native shared library `libjballoon.so` with debug information (i.e. `-g`) instead of the default `-O3`.

## Testing

 `jballoon-1.0-SNAPSHOT-tests.jar` contains the `io/simonis/jballoon/test/HumongousFragmentationTest` test which is based on OpenJDK's [`TestAllocHumongousFragment`](https://github.com/openjdk/jdk/blob/9cc01a9e0a2ca7dbe2650c852840e49418a742be/test/hotspot/jtreg/gc/TestAllocHumongousFragment.java) JTreg test. Originally, the test repeatedly creates a large number of humongous objects in a loop, some of which are retained for a certain amount of time while others are immediately released. This creates a highly fragmented heap and triggers frequent full GCs which will compact the humongous objects which are still alive in order to make room for new humongous allocations. This is the perfect scenario for testing that *jBalloon* memory balloons remain inflated even when moved by the GC. For testing *jBalloon*, I've modified the test to inflate a balloon after 10% of the allocations and deflate it again after 90% of the test's allocations.

In order to see how *jBalloon* works, the test has to be run with logging enabled. Logging in the native part is controlled by the environment variable `LOG` which can be one of `OFF` (the default), `ERROR`, `WARNING`, `INFO`, `DEBUG` and `TRACE`. Java logging is controlled by the system property `io.simonis.jballoon.logLevel` and can be any of Java's `java.util.logging` [logging Levels](https://docs.oracle.com/en/java/javase/25/docs/api/java.logging/java/util/logging/Level.html).

We first run the test with the `DEBUG` logging level:

```terminal
$ LOG=DEBUG java -Xmx1g -Xms1g -XX:+AlwaysPreTouch -XX:+UseG1GC \
                 -DballoonSize=209715200 \
                 -Dio.simonis.jballoon.logLevel=fine \
                 -cp jballoon-1.0-SNAPSHOT-tests.jar:jballoon-1.0-SNAPSHOT.jar \
                 io.simonis.jballoon.test.HumongousFragmentationTest
```

With a 1gb heap and 200mb balloon, running on a supported platform with a supported JDK, this will print the following output:

```terminal
...
JBalloon::inflate(209715200)                                                   (1)
JBalloon::inflateNative_impl() -> madvise(0xed301000, 210759680)               (2)
JBalloon::inflateNative_impl() -> created balloon at 0xed300000.               (3) 
JBalloon::inflate(209715200) -> Balloon(210763776, 210759680)                  (4)
JBalloon::move_if_jBalloon() -> moving balloon from 0xed300000 to 0xe8b00000   (5)
JBalloon::move_if_jBalloon() -> madvise(0xe8b01000, 210759680)                 (6)
JBalloon::move_if_jBalloon() -> moving balloon from 0xe8b00000 to 0xe6400000
JBalloon::move_if_jBalloon() -> madvise(0xe6401000, 210759680)
...
JBalloon::move_if_jBalloon() -> moving balloon from 0xc0600000 to 0xc0300000   (7)
JBalloon::move_if_jBalloon() -> madvise(0xc0301000, 210759680)                 
JBalloon::deflate(Balloon(210763776, 210759680))                               (8) 
JBalloon::deflateNative() -> removed balloon at 0xc0300000                     (9)
JBalloon::deflate(Balloon(deflated)) -> long[26345472]                         (10)   
```

I've omitted the first part of the `DEBUG` log for now. That part will be discussed in the [Implementation Details](#implementation-details) section. The following list describes the log lines above in more detail:

1. `Balloon::inflate()` was called with a `size` of `209715200` bytes.
2. The native implementation of `JBalloon` madvises `210759680` bytes (corresponds to `51455` `4k` pages) starting at address `0xed301000` as `MADV_DONTNEED`. This basically returns `51455` system pages back to the OS.
3. The `long[]` which backs up the balloon actually starts at `0xed301000` but we can't madvise the very first bytes, because that would overwrite the object header, so we started to madvise in the second page used by the array (i.e. `0xed301000`).
4. The native implementation returned a `Balloon` object which is backed by a `long[26345472]` array (i.e. `210763776 / 8 = 26345472`) of which `210759680` bytes have been inflated (i.e. returned to the OS).
5. When the GC moves our balloon array, the native part of `JBalloon` realizes this ..
6. ..and immediately madvises the array at the new location. Notice how G1 GC compacts humongous objects towards lower heap addresses. 
7. This goes on for a while, until our balloon array almost reaches the beginning of the heap (which in this example starts at `0xc0000000`).
8. When the test finally calls `JBalloon::deflate()` on our balloon..
9. ..the native implementation will unregister the backing `long[]` array at address `0xc0300000` from its records..
10. ..and return it as return value of the `JBalloon::deflate()` call. Notice that the returned array has the same size like the array created in step 4. It is actually the *same* array.

In order to see the actual effect of a balloon inflation on the memory occupancy of the Java heap, we have to run with the highest logging level `TRACE`. Notice that this will considerably increase the run time of the test, because a this logging level the test will print an accurate number for the memory used by the Java heap in each loop iteration which is computed from [`mincore`](https://man7.org/linux/man-pages/man2/mincore.2.html) (this includes `ZERO` pages) as well as from [`/proc/self/pagemap`](https://www.kernel.org/doc/Documentation/vm/pagemap.txt) (where we only count the occupied, physical pages).

So with the `TRACE` log level, we will get the following output:

```terminal
HumongousFragmentationTest -> (reserved/mincore/RSS) = (1048576kb / 1048576kb / 1048560kb)     (1)
...
JBalloon::inflateNative_impl() -> madvise(0xed301000, 210759680)
JBalloon::inflateNative_impl() -> (reserved/mincore/RSS) = (1048576kb / 842756kb / 842756kb)   (2)
...
HumongousFragmentationTest -> (reserved/mincore/RSS) = (1048576kb / 842756kb / 842756kb)
HumongousFragmentationTest -> (reserved/mincore/RSS) = (1048576kb / 842756kb / 842756kb)
...
HumongousFragmentationTest -> (reserved/mincore/RSS) = (1048576kb / 842756kb / 842756kb)9
JBalloon::move_if_jBalloon() -> moving balloon from 0xed300000 to 0xe8b00000                   (3)
JBalloon::move_if_jBalloon() -> madvise(0xe8b01000, 210759680)
JBalloon::move_if_jBalloon() -> (reserved/mincore/RSS) = (1048576kb / 769028kb / 769028kb)     (4)
HumongousFragmentationTest -> (reserved/mincore/RSS) = (1048576kb / 841404kb / 841404kb)
...
JBalloon::move_if_jBalloon() -> moving balloon from 0xc0500000 to 0xc0300000
JBalloon::move_if_jBalloon() -> (reserved/mincore/RSS) = (1048576kb / 841736kb / 841736kb)
JBalloon::move_if_jBalloon() -> madvise(0xc0301000, 210759680)
JBalloon::move_if_jBalloon() -> (reserved/mincore/RSS) = (1048576kb / 839688kb / 839684kb)     (5)
...
JBalloon::deflate(Balloon(210763776, 210759680))                                               (6)
JBalloon::deflateNative() -> removed balloon at 0xc0300000
JBalloon::deflate(Balloon(deflated)) -> long[26345472]
HumongousFragmentationTest -> (reserved/mincore/RSS) = (1048576kb / 841736kb / 841736kb)
HumongousFragmentationTest -> (reserved/mincore/RSS) = (1048576kb / 891480kb / 891480kb)
HumongousFragmentationTest -> (reserved/mincore/RSS) = (1048576kb / 909476kb / 909476kb)
HumongousFragmentationTest -> (reserved/mincore/RSS) = (1048576kb / 973784kb / 973784kb)
HumongousFragmentationTest -> (reserved/mincore/RSS) = (1048576kb / 992500kb / 992500kb)
HumongousFragmentationTest -> (reserved/mincore/RSS) = (1048576kb / 1029900kb / 1029900kb)
HumongousFragmentationTest -> (reserved/mincore/RSS) = (1048576kb / 1043060kb / 1043060kb)
HumongousFragmentationTest -> (reserved/mincore/RSS) = (1048576kb / 1043060kb / 1043060kb)
HumongousFragmentationTest -> (reserved/mincore/RSS) = (1048576kb / 1043060kb / 1043060kb)     (7)

```

Initially (1), the heap occupies 1048576kb (i.e. 1gb) (because we used `-XX:+AlwaysPreTouch` to simulate the worst case). Once the balloon is inflated (2), RSS drops by roughly `200mb` to `842756kb`. The memory consumption of the heap remains at `842756kb`, no difference how many allocations our test program is doing and how many GC will run (this can be verified by additionally enabling the GC log with `-Xlog:gc`).

When the balloon array is moved by the GC (3), the memory consumption temporarily drops even further down to `769028kb` (4) because `JBalloon` madvises the balloon at the new location while the old madvised region hasn't been fully touched by the GC yet (and thus pages haven't been completely paged in again). But notice how this temporary drop get smaller and smaller (5) as the source and destination locations of the array do overlap more and more the more to array is moved to the beginning of the heap.

Finally, once we deflate the balloon (6), the memory consumption slowly gets back to the initial `1048576kb` as the previously madvised pages get paged in again either because GC moves other objects into those regions previously occupied by the balloon array or because the application can allocate in these regions because they are now free from the point of view of the GC (if the deflated backing array isn't kept alive by the application).
## Implementation Details

As mentioned previously, without builtin JVM support, *jBalloon* can currently only work by hooking deep into the internals of HotSpot's GC. This makes it highly platform and JDK version dependent. In this section we explain in some more detail how exactly *jBalloon* is implemented and how it works.

If we run with `INFO` log level (i.e. set the environment variable `LOG=INFO`) *jBalloon* will print the following output during initialization on Linux/x86_64 with JDK 21:

```terminal
(1) JBalloon::nativeInit() -> page size: 4096, region size: 1048576, object header size: 12
    JBalloon::nativeInit() -> long array offset in object: 16
    JBalloon::nativeInit() -> heap base: 0xc0000000, heap size: 1073741824
(2) JBalloon::can_use_compact_humongous_obj_patching() -> located 'SerialHeap::SerialHeap()' at 0x7ffff6dbf680 (size = 161)
(3) JBalloon::can_use_compact_humongous_obj_patching() -> located 'G1FullGCCompactTask::compact_humongous_obj(HeapRegion*)' at 0x7ffff680cf20 (size = 705)
(4) JBalloon::patch_call_instr() -> found call to 'memmove()' at 0x7ffff680d017
(5) JBalloon::patch_call_instr() -> call to 'memmove()' can be patched directly
```
1. The first three lines contain basic information about the system and the JVM.
2. Locate the constructor `SerialHeap::SerialHeap()`. This memory location will be used as address for a trampoline, if directly patching the call to `memmove()` won't be possible because our modified version of `memmove()` in `libjballoon.so` is too far away from the patch location. Notice that we can be sure that `SerialHeap::SerialHeap()` won't be used if we are running with G1 GC.
3. Locate `G1FullGCCompactTask::compact_humongous_obj(HeapRegion*)`. In JDK 21 on x86_64, `compact_humongous_obj()` inlines the whole call chain from `copy_object_to_new_location()` over `Copy::aligned_conjoint_words()`, `pd_aligned_conjoint_words()`, `pd_conjoint_words()` down to `memmove()`.
4. If we find the inlined call to `memmove()` in `compact_humongous_obj()`..
5. ..patch it to call our modified version of `memmove()`. On x86_64 we can normally patch the call directly because the `JMP` instruction takes a 31-bit IP-relative offset which is usually more then enough to reach our code in `libjballoon.so`.

On Linux/x86_64 with JDK 25 the output looks slightly different (omitting the first three lines which don't change):

```terminal
...
    JBalloon::can_use_compact_humongous_obj_patching() -> located 'SerialHeap::SerialHeap()' at 0x7ffff72daca0 (size = 240)
    JBalloon::can_use_compact_humongous_obj_patching() -> located 'G1FullGCCompactTask::compact_humongous_obj(HeapRegion*)' at 0x7ffff6ccd0f0 (size = 613)
(1) JBalloon::can_use_compact_humongous_obj_patching() -> located 'G1FullGCCompactTask::copy_object_to_new_location(oop obj)' at 0x7ffff6ccc6d0 (size = 441)
(2) JBalloon::can_use_compact_humongous_obj_patching() -> 'G1FullGCCompactTask::compact_humongous_obj()' calls 'copy_object_to_new_location()' at 0x7ffff6ccd206
(3) JBalloon::patch_call_instr() -> found call to 'memmove()' at 0x7ffff6ccc779
(4) JBalloon::patch_call_instr() -> call to 'memmove()' can be patched directly
```

1. On JDK 25, `G1FullGCCompactTask::compact_humongous_obj(HeapRegion*)` can't inline the whole call chain up to `memmove()` (because of [JEP 519: Compact Object Headers](https://openjdk.org/jeps/519) which slightly increased the size of some of the callees). Instead it contains a call to `G1FullGCCompactTask::copy_object_to_new_location(oop obj)`.
2. If we can verify that `compact_humongous_obj()` does indeed call `copy_object_to_new_location()` and..
3. ..`copy_object_to_new_location()` inlined the call to `memmove()`..
4. ..we patch the call to `memmove()` in `copy_object_to_new_location()`.

Finally, on Linux/aarch64 we will see for JDK 21:

```terminal
...
    JBalloon::can_use_compact_humongous_obj_patching() -> located 'SerialHeap::SerialHeap()' at 0xffffbe435f38 (size = 140)
(1) JBalloon::can_use_compact_humongous_obj_patching() -> located '_Copy_conjoint_words()' at 0xffffbde01fc0
    JBalloon::can_use_compact_humongous_obj_patching() -> located 'G1FullGCCompactTask::compact_humongous_obj(HeapRegion*)' at 0xffffbdf15758 (size = 1160)
(2) JBalloon::patch_call_instr() -> found call to '_Copy_conjoint_words()' at 0xffffbdf15b14
(3) JBalloon::patch_call_instr() -> patching '_Copy_conjoint_words()' requires a trampoline
```

1. On aarch64, HotSpot is not using `memmove()` for copying objects but instead uses the hand-crafted assembly stub `_Copy_conjoint_words()` (i.e. `pd_conjoint_words()` calls `_Copy_conjoint_words()` instead of `memmove()`).
2. If we find the call to `_Copy_conjoint_words()` in `compact_humongous_obj()`..
3. ..patch it to call our modified version instead. Notice that on aarch64, the original `BL` call instruction only uses a 26-bit IP relative offset, so we usually need a trampoline with a `BR` instruction which can jump to an absolute 64-bit address loaded from memory. We create this trampoline at the location of `SerialHeap::SerialHeap()` as mentioned previously.


If running on JDK 17, *jBalloon* doesn't have to do any patching at all, because before JDK 21, G1 GC didn't move humongous objects at all. This means that *jBalloon* only has to madvise the backing `long[]` array. Still, running on JDK 17 and creating many humongous objects is not recommended because it can easily lead to out of memory situations because of heap fragmentation.
## Alternatives

Ideally, it should be possible to use the manageable [`-XX:SoftMaxHeapSize`](https://malloc.se/blog/zgc-softmaxheapsize) option to dynamically control the heap size of a Java application. Unfortunately, changing `-XX:SoftMaxHeapSize` doesn't instantly change the heap consumption. Instead, it instructs the JVM to gradually decrease heap usage until it reaches the new value. In addition, the freed heap parts are not immediately returned to the OS but again in a gradual process controlled by various GC parameters. For Shenandoah, the [uncommit process is  controlled](https://mail.openjdk.org/pipermail/hotspot-gc-dev/2018-June/022204.html) by `-XX:ShenandoahUncommit`, `-XX:ShenandoahUncommitDelay` and `-XX:ShenandoahUncommitWithIdle`. G1 supports uncommitting since JDK 12 (see [JEP 346: Promptly Return Unused Committed Memory from G1](https://bugs.openjdk.org/browse/JDK-8204089) and the [CSR for Promptly Return Unused Committed Memory from G1](https://bugs.openjdk.org/browse/JDK-8212658)). In G1, uncommitting can either be done periodically (with `-XX:G1PeriodicGCInterval`) or depending on the system load (with `-XX:G1PeriodicGCSystemLoadThreshold`). Other parameters for uncommitting heap memory like [`UncommitSizeLimit`](https://github.com/openjdk/jdk/blob/ca405d0eb2a0ed63dc169aceb80512bf2a523da1/src/hotspot/share/gc/g1/g1UncommitRegionTask.hpp#L36) and [`UncommitInitialDelayMs`](https://github.com/openjdk/jdk/blob/ca405d0eb2a0ed63dc169aceb80512bf2a523da1/src/hotspot/share/gc/g1/g1UncommitRegionTask.hpp#L38) are not even exposed as command line options. ZGC supports uncommitting since JDK 13 (see [JEP 351: ZGC: Uncommit Unused Memory](https://openjdk.org/jeps/351)) with the command line options `-XX:-ZUncommit` and `-XX:ZUncommitDelay`.

Currently, most GCs treat the maximum configured heap size as a "free" resource which they eagerly consume (similar to the [Linux Page Cache](https://github.com/firmianay/Life-long-Learner/blob/master/linux-kernel-development/chapter-16.md)). but there's currently some activity to make the Java heap sizing more dynamic and let it adapt to e.g. the system load or the GC times. Among these are [JDK-8236073: G1: Use SoftMaxHeapSize to guide GC heuristics
](https://bugs.openjdk.org/browse/JDK-8236073), [JDK-8359211: Automatic Heap Sizing for G1](https://bugs.openjdk.org/browse/JDK-8359211) and [JDK-8377305: Automatic Heap Sizing for ZGC](https://bugs.openjdk.org/browse/JDK-8377305). And finally there's Google's [No More Xmx - Adaptable Heap Sizing for Containerized Java Applications](https://www.youtube.com/watch?v=qOt4vOkk49k) approach, which was presented at Devoxx Belgium in 2022.

Finally, making this a JDK-supported feature would considerably simplify the implementation and make it more robust a the same time. This could be achieved by making `JBalloon.Balloon` a JDK class similar to e.g. `WeakReference` which is known to and specially handled by the GC. In such a case, there would be no need for patching GC internals and the re-inflation in the event of movement could be trivially handled directly by the GC.

