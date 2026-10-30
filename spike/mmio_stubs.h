#ifndef MMIO_STUBS_H
#define MMIO_STUBS_H
#include <stdint.h>
enum { OP_LOAD, OP_STORE };
typedef struct { int op; uint64_t addr; int width; uint64_t value; } rec_t;
extern rec_t recs[64];
extern int nrecs;
extern uint64_t scripted;
#endif
