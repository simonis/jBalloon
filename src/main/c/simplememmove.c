#include <dlfcn.h>
#include <stddef.h>

int PAGE_SIZE = 4096;

typedef void *(*memmove_t)(void *, const void *, size_t);
void *memmove(void *dest, const void *src, size_t n) {
    static memmove_t original_memmove = NULL;
    if (!original_memmove) {
        original_memmove = (memmove_t)dlsym(RTLD_NEXT, "memmove");
    }

    if (dest == src || n == 0) return dest;

    int pages = n / PAGE_SIZE;
    int remaining = n % PAGE_SIZE;
    for (size_t i = 0; i < pages; i++) {
        original_memmove(dest + (i * PAGE_SIZE), src + (i * PAGE_SIZE), PAGE_SIZE);
    }

    unsigned char *d = dest + pages * PAGE_SIZE;
    const unsigned char *s = src + pages * PAGE_SIZE;

    if (d < s) {
       for (size_t i = 0; i < remaining; i++) {
            d[i] = s[i];
        }
    } else {
        for (size_t i = remaining; i > 0; i--) {
            d[i-1] = s[i-1];
        }
    }
    return dest;
}

void *memcpy(void *dest, const void *src, size_t n) {
    return memmove(dest, src, n);
}
