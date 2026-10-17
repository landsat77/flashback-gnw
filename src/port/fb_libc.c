/*
 * The few C library functions REminiscence uses that the firmware ABI does
 * not export.
 */
#include <stddef.h>

static int lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 'a' - 'A' : c;
}

int fb_strncasecmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        int x = lower((unsigned char)*a), y = lower((unsigned char)*b);
        if (x != y || !x)
            return x - y;
    }
    return 0;
}

int fb_strcasecmp(const char *a, const char *b)
{
    return fb_strncasecmp(a, b, (size_t)-1);
}

/* sin / cos (cutscene rotations) and bsearch: not in the firmware ABI */
static double reduce(double x)
{
    const double two_pi = 6.283185307179586476925286766559;
    long k = (long)(x / two_pi);
    x -= (double)k * two_pi;
    if (x > 3.14159265358979323846)
        x -= two_pi;
    else if (x < -3.14159265358979323846)
        x += two_pi;
    return x;
}

double sin(double x)
{
    x = reduce(x);
    double x2 = x * x, term = x, sum = x;
    for (int n = 1; n < 12; n++) {
        term *= -x2 / (double)((2 * n) * (2 * n + 1));
        sum += term;
    }
    return sum;
}

double cos(double x)
{
    x = reduce(x);
    double x2 = x * x, term = 1.0, sum = 1.0;
    for (int n = 1; n < 12; n++) {
        term *= -x2 / (double)((2 * n - 1) * (2 * n));
        sum += term;
    }
    return sum;
}

void *bsearch(const void *key, const void *base, size_t n, size_t size,
              int (*cmp)(const void *, const void *))
{
    const char *lo = (const char *)base;
    while (n > 0) {
        const char *mid = lo + (n / 2) * size;
        int r = cmp(key, mid);
        if (r == 0)
            return (void *)mid;
        if (r > 0) {
            lo = mid + size;
            n -= n / 2 + 1;
        } else {
            n /= 2;
        }
    }
    return NULL;
}
