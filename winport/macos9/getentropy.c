/*
 * getentropy.c - getentropy() for the classic Mac OS.
 *
 * The C library of clang-retro/mac-sdk declares getentropy() in
 * <unistd.h>, and its arc4random() and the std::random_device of its
 * C++ library call it, but the library does not have it. libre makes
 * its random numbers (SIP tags, SRTP keys) with arc4random().
 *
 * The classic Mac OS has no source of random numbers. This function
 * collects the jitter of the microsecond clock (Microseconds(), System
 * 7.0 and later): the time of each sample changes with the interrupts,
 * the other threads and the cache. It mixes the samples into a 64-bit
 * state with the finalizer of SplitMix64. The result is not as strong
 * as the random numbers of a modern system.
 *
 * Remove this file when mac-sdk's C library has getentropy().
 */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <Timer.h>

/* The number of clock samples for each byte of output */
#define SAMPLES_PER_BYTE 16

/* Mixes v into the state *s, and gives the new state */
static uint64_t mix(uint64_t *s, uint64_t v)
{
	uint64_t z = (*s += v + UINT64_C(0x9e3779b97f4a7c15));
	z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
	z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
	*s = z ^ (z >> 31);
	return *s;
}

/* Gives the microsecond clock as one 64-bit value */
static uint64_t micros(void)
{
	UnsignedWide w;
	Microseconds(&w);
	return ((uint64_t)w.hi << 32) | w.lo;
}

/* Fills buf with len random bytes, at most 256. Gives 0, or -1 with
   errno EIO when len is more than 256 (as on the BSDs). */
int getentropy(void *buf, size_t len)
{
	static uint64_t state;
	static uint32_t calls;
	unsigned char *out = (unsigned char *)buf;
	size_t i;
	int j;

	if (len > 256) {
		errno = EIO;
		return -1;
	}
	mix(&state, ++calls);
	mix(&state, (uint64_t)(uintptr_t)&i);
	for (i = 0; i < len; i++) {
		uint64_t prev = micros(), now;
		for (j = 0; j < SAMPLES_PER_BYTE; j++) {
			/* (a short wait, its length from the state, so that
			   the samples are not at a fixed interval) */
			volatile unsigned spin = (unsigned)(state & 63);
			while (spin--)
				;
			now = micros();
			mix(&state, now - prev);
			prev = now;
		}
		out[i] = (unsigned char)(mix(&state, now) >> 56);
	}
	return 0;
}
