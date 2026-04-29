#pragma once

#include <stdint.h>

#include "crow.brbt/brbt.h"
#include "crow.crowcpu_arch/crowcpu_arch.h"
#include "crowcpu.h"

enum
{
  MAX_MMU_INTERCEPTS = 16,

  TLB_PAGE_CAP = 100,
  TLB_SEGMENT_CAP = 100,

  TLB_PAGE_MAXSIZE = 40,
  TLB_SEGMENT_MAXSIZE = 20,
};

struct segment_table_entry
{
  tenc32_segment_descriptor_flags flags;

  unsigned hash;

  /*
   * segment ID
   * == -1U if this entry is free
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

struct tlb_page_node
{
  /* segment & page index */
  uint32_t id;

  struct
  {
    bool active;
    bool written;
    bool read;
  } flags;

  /* offset into physical memory
   * must be aligned to 2^12 bits
   */
  uint32_t offset;
};

struct tlb_segment_node
{
  /* segment index */
  uint32_t id;

  /* flag info */
  struct
  {
    bool active;
    bool write;
    bool execute;
    bool paged;
    bool protected;
    bool io;
  } flags;

  /* payload information within a&b */
  union
  {
    struct
    {
      uint32_t offset;
      uint32_t size;
    } unpaged;

    struct
    {
      uint32_t offset;
    } paged;

    struct
    {
      uint32_t hardware_io;
    } io;
  } data;
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
    struct tlb_page_node pages[TLB_PAGE_CAP];
    struct tlb_segment_node segments[TLB_SEGMENT_CAP];
    unsigned num_pages;
    unsigned num_segments;
  } tlb;
} mmu_t;

typedef enum
{
  MMU_BYTE,
  MMU_WORD,
} mmu_addressing_mode;

struct resolve
{
  bool paged;
  bool io_space;
  struct tlb_segment_node* segment;
  struct tlb_page_node* page;

  uint32_t mmio_space;
  uint32_t phys_addr;
};

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
tlb_segment_consult(tenc32_motherboard_t* mobo, uint32_t addr);

bool
tenc32_read_word_ex(tenc32_motherboard_t* mobo,
                    uint32_t addr,
                    uint32_t* out,
                    struct resolve* rout);
