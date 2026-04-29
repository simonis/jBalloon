# jBalloon

This is an experimental library which allows Java code to instantly release a part of the Java heap memory to the OS and reclaim it whenever necessary. It should work with G1 GC on JDK 21 & 25 (with all combinations of `-XX:+/-UseCompactObjectHeaders`, `-XX:+/-UseCompressedClassPointers` and `-XX:+/-UseCompressedOops`). I think it should be straightforward to port it to Shenandoah while ParallelGC support might require some more effort.

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

The balloon should be at least as large as a region of the GC in order to qualify it as a "humongous" object for the GC. The contents of the backing byte array are only read during a last-ditch Full GC when the GC also moves/compacts humongous objects (see [JDK-8191565: Last-ditch Full GC should also move humongous objects
](https://bugs.openjdk.org/browse/JDK-8191565)). When the GC reads the array, the kernel will page in the kernel `ZERO_PAGE` for memory regions which were previously `madvise(MADV_DONTNEED)` which don't consume any physical memory pages on the system. So as long as the source and destination locations of the array don't overlap, moving an object will not re-increase the memory consumption of the Java process and after copying is done, we can inflate the array at its new location.

In order to get notifications of when the GC starts to read from the array and when the moving operation has finished, we register the corresponding address space with the [`userfaultfd`](https://docs.kernel.org/6.3/admin-guide/mm/userfaultfd.html) file descriptor. This invokes a call-back, each time a previously `madvise(MADV_DONTNEED)` page is accessed during GC. The callback does basically two things: if the first page of the array is accessed, it will page in the `ZERO_PAGE` for all but the last array page. This will allow the GC to proceed until it reaches the last page. When the last page of the array is touched, we know that the copying process is about to end, so we can `madvise(MADV_DONTNEED)` the array at the destination address, which can be found by following the forwarding pointer of the source object. And at the same time, we de-register the memory region used by the source array from `userfaultfd` and instead register the memory region of the new (i.e. moved) object. 

Things get a little more complicated, when the source and destination locations of the array overlap. In such situations, the GC will not only read from the source array, but at some point also write into overlapping part of the source array. Although, it will only write zeros (read from the `ZERO_PAGE`), writing will still page in "real" memory pages, because the kernel is not checking what content gets written.