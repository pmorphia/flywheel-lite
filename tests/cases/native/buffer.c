// flywheel: cflags -DSCALE=3
// flywheel(unix): link m
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stddef.h>
#include "fail.h"
typedef struct { double *data; long count; } Buffer;
Buffer *buffer_new(long count) { Buffer *b = malloc(sizeof *b); b->data = calloc((size_t)count, sizeof(double)); b->count = count; return b; }
void buffer_set(Buffer *b, long i, double v) { b->data[i] = v * SCALE; }
double buffer_norm(const Buffer *b) { double s = 0; for (long i = 0; i < b->count; i++) s += b->data[i] * b->data[i]; return sqrt(s); }
const double *buffer_data(const Buffer *b) { return b->data; }
void buffer_free(Buffer *b) { free(b->data); free(b); }
double sum(const double *values, size_t count) { double t = 0; for (size_t i = 0; i < count; i++) t += values[i]; return t; }
void scale(float *values, int64_t count, double k) { for (int64_t i = 0; i < count; i++) values[i] *= (float)k; }
static void twice(int *values, int n) { for (int i = 0; i < n; i++) values[i] *= 2; }
typedef struct { double x, y; } Point;
void shift(void *points, long n, double by) { Point *p = points; for (long i = 0; i < n; i++) p[i].x += by; }
const char *greet(const char *n) { static char b[64]; snprintf(b, sizeof b, "hi %s", n); return b; }
_Bool even(long n) { return n % 2 == 0; }
int risky(int x) { if (x < 0) fw_foreign_fail("negative input"); return x; }
#define UNUSED \
    int not_a_function(int x) {
int main(void) { return 0; }
