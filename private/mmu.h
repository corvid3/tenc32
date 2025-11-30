#pragma once

#include <stdint.h>

#include "crow.brbt/brbt.h"
#include "crow.crowcpu_arch/crowcpu_arch.h"
#include "crowcpu.h"

#define MAX_MMU_INTERCEPTS 16

struct segment_table_entry
{
  tenc32_segment_descriptor_flags flags;

  /*
   * segment ID
   */
  uint32_t id;

  /* PAGED:   offset
   * UNPAGED: offset
   */
  uint32_t b;

  /* PAGED:   unused
   * UNPAGED: size*/
  uint32_t c;
};

struct page_table_entry
{
  tenc32_page_table_entry_flags flags;
  uint32_t offset;
};

typedef struct
{
  /* physical memory address */
  uint32_t segment_table_offset;

  /* IO space information */
  struct brbt hardware_io;

  uint32_t staging_register;

  struct
  {
    struct brbt pages;
    struct brbt segments;
    struct tlb_policy* policy_data;
  } tlb;
} mmu_t;

typedef enum
{
  MMU_BYTE,
  MMU_WORD,
} mmu_addressing_mode;

void
tenc32_mmu_init(struct tenc32_motherboard_t*);
void
tenc32_mmu_reset(struct tenc32_motherboard_t*);

/* will search the translation buffer for a given
 * seg/page address, and will insert a new
 * entry if not found, returning the new translation
 * addr may be any valid address,
 * the lower offset bytes will be set to 0
 */
struct tlb_page_node*
tlb_page_consult(tenc32_motherboard_t*, uint32_t addr);

/* addr should be a full address, where the segment selector
 * is in the upper bits
 */
struct tlb_segment_node*
tlb_segment_consult(tenc32_motherboard_t* mmu, uint32_t addr);
