#ifndef __AP2_MRP_SYNC_H_
#define __AP2_MRP_SYNC_H_

#include <pthread.h>

typedef struct {
    pthread_mutex_t mutex;
} ap2_mrp_serial_t;

static inline void ap2_mrp_serial_init(ap2_mrp_serial_t *serial)
{
    pthread_mutex_init(&serial->mutex, NULL);
}

static inline void ap2_mrp_serial_destroy(ap2_mrp_serial_t *serial)
{
    pthread_mutex_destroy(&serial->mutex);
}

static inline void ap2_mrp_serial_lock(ap2_mrp_serial_t *serial)
{
    pthread_mutex_lock(&serial->mutex);
}

static inline void ap2_mrp_serial_unlock(ap2_mrp_serial_t *serial)
{
    pthread_mutex_unlock(&serial->mutex);
}

#endif /* __AP2_MRP_SYNC_H_ */
