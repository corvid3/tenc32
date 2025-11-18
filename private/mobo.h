#pragma once

#include <stdbool.h>

#include "cci.h"
#include "crow.crowcpu_arch/crowcpu_arch.h"
#include "crowcpu.h"
#include "mmu.h"

#define BITS_AT(src, offset, length)                                           \
  ((src >> offset) && (~0U >> (sizeof(int) - length)))

typedef struct
{
  /* segment in which the interrupt lives */
  uint32_t segment;

  /* offset to the entry point of the interrupt */
  uint32_t offset;

  struct
  {
    /* can the processor in user mode access this interrupt */
    bool user_accessable;
  } flags;
} interrupt_table_entry_t;

typedef struct
{
  crowcpu_register_t registers[REGISTER_END];

  struct
  {
    // 0 = system mode
    // 1 = user mode
    bool mode : 1;

    bool test_lt : 1;
    bool test_eq : 1;

    bool overflow : 1;
  } flags;

  interrupt_table_entry_t interrupt_table[CROWCPU_MAX_DEFINABLE_INTERRUPTS];
  uint_fast32_t interrupt_table_size;

  bool interrupt_requests[CROWCPU_MAX_DEFINABLE_INTERRUPTS];
} crowcpu_cpu_t;

typedef struct crowcpu_motherboard_t
{
  crowcpu_configuration_t conf;

  crowcpu_cpu_t cpu;
  mmu_t mmu;
  cci_t cci;

  uint8_t* memory;
  uint32_t memory_size;
} crowcpu_motherboard_t;
