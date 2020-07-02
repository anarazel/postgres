#ifndef SQUEUE32_H
#define SQUEUE32_H

struct squeue32;
typedef struct squeue32 squeue32;

extern size_t squeue32_estimate(size_t size);
extern void squeue32_init(squeue32 *queue, size_t items);
extern bool squeue32_enqueue(squeue32 *queue, uint32 value);
extern bool squeue32_dequeue(squeue32 *queue, uint32 *value);

#endif
