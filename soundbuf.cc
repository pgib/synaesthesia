#include <SDL.h>
#include "syna.h"

#define WITH_SDL
#define RING_SIZE (NumSamples*2)
#define MIN_NEW_SAMPLES (RING_SIZE/2)
/* #define CONVERT_SAMPLES(x) ((x) / 32768.0) */
#undef CONVERT_SAMPLES

/*
 * Incoming samples go into a ring buffer protected by a mutex.
 * Next incoming sample goes to ring[ring_write], and ring_write wraps.
 * When enough samples have arrived sound_retrieve() copies from the ring
 * buffer, protected by that mutex, which prevents new samples from
 * overwriting. It uses ring_write to know how data is wrapped in the
 * ring buffer.
 */
static sampleType ring[RING_SIZE];
static unsigned int ring_write = 0;
#ifdef WITH_SDL
static SDL_mutex *mutex = NULL;
static SDL_cond *cond = NULL;
#else /* Use pthreads */
static pthread_mutex_t mutex;
static pthread_cond_t cond;
#endif
static int signalled = 0;

/* Number of samples put into ring buffer since last sound_retrieve() call is
 * counted by ring_has. If visualizer is slower than input, ring_has could be
 * more than RING_SIZE, which is useful for timing.
 */
static unsigned int ring_has = 0;

void sndbuf_init(void)
{
#ifdef WITH_SDL
    mutex = SDL_CreateMutex();
    if (mutex == NULL) error("creating SDL_mutex for sound buffer");
    cond = SDL_CreateCond();
    if (cond == NULL) error("creating SDL_cond for sound buffer");
#else /* Use pthreads */
    pthread_mutex_init(&mutex, NULL);
    pthread_cond_init(&cond, NULL);
#endif
}

void sndbuf_quit(void)
{
#ifdef WITH_SDL
    SDL_DestroyMutex(mutex);
    mutex = NULL;
    SDL_DestroyCond(cond);
    cond = NULL;
#else /* Use pthreads */
    pthread_mutex_destroy(&mutex);
    pthread_cond_destroy(&cond);
#endif
}

static inline void sndbuf_lock(void)
{
#ifdef WITH_SDL
    SDL_LockMutex(mutex);
#else /* Use pthreads */
    pthread_mutex_lock(&mutex);
#endif
}

static inline void sndbuf_unlock(void)
{
#ifdef WITH_SDL
    SDL_UnlockMutex(mutex);
#else /* Use pthreads */
    pthread_mutex_unlock(&mutex);
#endif
}

void sndbuf_store(const sampleType *input, unsigned int len)
{
#ifdef CONVERT_SAMPLES
    int i;
#endif
    const sampleType *ip;
    sampleType *op;
    unsigned int remain;

    sndbuf_lock();

    if (len >= RING_SIZE) {
        /* If there are too many samples, fill buffer with latest ones */
        ip = input + len - RING_SIZE;
        op = &(ring[0]);
        remain = RING_SIZE;

        ring_write = 0;
    } else {
        int new_ring_write;
        remain = RING_SIZE - ring_write;
        if (remain > len) {
            remain = len;
            new_ring_write = ring_write + len;
        } else if (remain == len) {
            new_ring_write = 0;
        } else {
            /* Do part of store after wrapping around ring */
            new_ring_write = len - remain;
            ip = input + remain;
            op = &(ring[0]);
#ifdef CONVERT_SAMPLES
            for (i = 0; i < new_ring_write; i++) {
                *(op++) = CONVERT_SAMPLES(*(ip++));
            }
#else
            memcpy(op, ip, new_ring_write * sizeof(sampleType));
#endif
        }

        ip = input;
        op = &(ring[ring_write]);

        /* Advance write pointer */
        ring_write = new_ring_write;
    }

    /* Do part of store before wrapping point, maybe whole store */
#ifdef CONVERT_SAMPLES
    for (i = 0; i < remain; i++) {
        *(op++) = CONVERT_SAMPLES(*(ip++));
    }
#else
    memcpy(op, ip, remain * sizeof(sampleType));
#endif

    /* This could theoretically overflow, but is needed for timing */
    ring_has += len;

    if (ring_has >= MIN_NEW_SAMPLES && !signalled) {
        /* Next buffer is full */
        signalled = 1;
#ifdef WITH_SDL
        SDL_CondSignal(cond);
#else /* Use pthreads */
        pthread_cond_signal(&cond);
#endif
    }
    sndbuf_unlock();
}

int getNextFragment(void) {
    unsigned int chunk1, ring_had;

    sndbuf_lock();

    /* Wait for next buffer to be full */
    while (!signalled) {
#ifdef WITH_SDL
        SDL_CondWait(cond, mutex);
#else /* Use pthreads */
        pthread_cond_wait(&cond, &mutex);
#endif
    }
    signalled = 0;

    /* Copy first part, up to wrapping point */
    chunk1 = RING_SIZE - ring_write;
    memcpy(&data[0], &ring[ring_write], sizeof(sampleType) * chunk1);
    if (ring_write > 0) {
        /* Samples are wrapped around the ring. Copy the second part. */
        memcpy(&data[chunk1], &ring[0], sizeof(sampleType) * ring_write);
    }

    ring_had = ring_has;
    ring_has = 0;

    /* After swap the buffer shouldn't be touched anymore by interrupts */
    sndbuf_unlock();

    return ring_had;
}
