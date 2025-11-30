#pragma once

#include <stdbool.h>
#include <threads.h>

#include "cci.h"
#include "crow.crowcpu_arch/crowcpu_arch.h"
#include "crowcpu.h"
#include "mmu.h"

#define MAX_IRQ 8u

#define BITS_AT(src, offset, length)                                           \
  ((src >> offset) && (~0U >> (sizeof(int) - length)))

/* programmable interrupt controller */
struct tenc32_pic
{
  mtx_t mutex;
  uint32_t incoming_flags;
  uint32_t currently_handled;
};

#define GETFLAG(cpu, flag) (cpu.crs.flags & flag)

typedef struct
{
  tenc32_register_t registers[REGISTER_END];

  struct
  {
    tenc32_register_t cr0;

    /* interrupt mask
     * 0 = ignore interrupt
     * 1 = allow interrupt
     */
    tenc32_register_t imask;

    /* execution flag bit */
    tenc32_register_t flags;
    tenc32_register_t idtr;
  } crs;

  /* -1 = no exception */
  int exception;

  /* waiting for an interrupt to happen... */
  bool halting;
} tenc32_cpu_t;

typedef struct tenc32_motherboard_t
{
  tenc32_configuration_t conf;

  tenc32_cpu_t cpu;
  struct tenc32_pic pic;
  mmu_t mmu;
  cci_t cci;

  uint8_t* memory;
  uint32_t memory_size;

  bool poweroff;

  void (*exception_callback)(tenc32_motherboard_t*);
} tenc32_motherboard_t;
