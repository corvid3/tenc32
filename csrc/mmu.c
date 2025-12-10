#include <assert.h>
#include <crow.crowcpu_arch/crowcpu_arch.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "crow.brbt/brbt.h"
#include "crowcpu.h"
#include "mmu.h"
#include "mobo.h"

#define CLOCK_EMPTY ((unsigned)-1)
#define TLB_PAGE_CAP 16
#define TLB_SEGMENT_CAP 8
#define MAX_HARDWARE_IO 32

static void
dump_tlb(tenc32_motherboard_t* mobo);

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

struct tlb_policy
{
  // brbt_node page_clock[TLB_PAGE_CAP];
  // unsigned page_clock_arm;

  /* need to know if the TLB has been fully flushed,
   * so we use a size tracker.
   * in that case, this should be 0 and we set arm to EMPTY
   */
  // unsigned page_clock_size;

  // brbt_node segment_clock[TLB_SEGMENT_CAP];
  // unsigned segment_clock_arm;
};

struct resolve
{
  bool paged;
  bool io_space;
  struct tlb_segment_node* segment;
  struct tlb_page_node* page;

  uint32_t mmio_space;
  uint32_t phys_addr;
};

static bool
mmu_resolve_address(tenc32_motherboard_t* mobo,
                    uint32_t req,
                    struct resolve* out);

static int
page_table_comparison(uint32_t* lhs, uint32_t* rhs)
{
  if (*lhs < *rhs)
    return -1;

  if (*lhs > *rhs)
    return 1;

  return 0;
}

static int
segment_table_comparison(uint32_t* lhs, uint32_t* rhs)
{
  if (*lhs < *rhs)
    return -1;

  if (*lhs > *rhs)
    return 1;

  return 0;
}

static int
hardware_io_comparison(uint32_t* lhs, uint32_t* rhs)
{
  if (*lhs < *rhs)
    return -1;

  if (*lhs > *rhs)
    return 1;

  return 0;
}

/* TODO: policy intercept for tlb delete, write
 * changes back into memory if write bit is set
 */

static brbt_node
policy_decide(struct brbt* tree, [[maybe_unused]] mmu_t* mmu)
{
  /* more advanced replacement algorithms can come in the future
   * for now, just evict a random line
   */

  unsigned v;
  assert(brbt_capacity(tree) != 0);

  /* trick to get even distribution */
  do
    v = rand();
  while (v > (1u << 31));

  return v % brbt_capacity(tree);
}

typedef void (*brbt_abort)(struct brbt*,
                           void* userdata,
                           int internal_source_line);

static void
brbt_internal_abort([[maybe_unused]] struct brbt* tree,
                    [[maybe_unused]] void* userdata,
                    int internal_source_line)
{
  fprintf(stderr, "internal brbt error: line %i\n", internal_source_line);
  assert(false);
}

static struct brbt_allocator_out
page_tlb_allocator(struct brbt* tree,
                   [[maybe_unused]] void* userdata,
                   [[maybe_unused]] void* array,
                   [[maybe_unused]] struct brbt_bookkeeping_info* bk)
{
  static struct tlb_page_node pages[TLB_PAGE_CAP];
  static struct brbt_bookkeeping_info page_bookkeeping[TLB_PAGE_CAP];

  struct brbt_allocator_out out;
  out.data_array = NULL;
  out.bk_array = NULL;
  out.size = 0;

  /* first allocation, provide it with the space */
  if (brbt_capacity(tree) == 0) {
    out.data_array = pages;
    out.bk_array = page_bookkeeping;
    out.size = TLB_PAGE_CAP;
  }

  return out;
}

static struct brbt_allocator_out
segment_tlb_allocator(struct brbt* tree,
                      [[maybe_unused]] void* userdata,
                      [[maybe_unused]] void* array,
                      [[maybe_unused]] struct brbt_bookkeeping_info* bk)
{
  static struct tlb_segment_node segments[TLB_SEGMENT_CAP];
  static struct brbt_bookkeeping_info segments_bookkeeping[TLB_SEGMENT_CAP];

  struct brbt_allocator_out out;
  out.data_array = NULL;
  out.bk_array = NULL;
  out.size = 0;

  /* first allocation, provide it with the space */
  if (brbt_capacity(tree) == 0) {
    out.data_array = segments;
    out.bk_array = segments_bookkeeping;
    out.size = TLB_PAGE_CAP;
  }

  return out;
}

static bool
mmu_self_read(void* data, uint32_t addr, uint32_t* out)
{
  tenc32_motherboard_t* mobo = data;

  addr &= 0xFFFF;
  if (addr != 0)
    return false;

  *out = mobo->mmu.staging_register;
  return true;
}

static bool
mmu_self_write(void* data, uint32_t addr, uint32_t val)
{
  tenc32_motherboard_t* mobo = data;

  addr &= 0xFFFF;

  if (addr == 0) {
    mobo->mmu.staging_register = val;
    return true;
  } else if (addr == 1) {
    if (val == 0) {
      /* update the table physical location */
      mobo->mmu.segment_table_offset = mobo->mmu.staging_register;
      return true;
    } else if (val == 1) {
      /* invalidate the TLB */
      brbt_clear(&mobo->mmu.tlb.pages);
      brbt_clear(&mobo->mmu.tlb.segments);
      assert(brbt_size(&mobo->mmu.tlb.pages) == 0);
      return true;
    } else if (val == 2) {
      /* translate a virtual address to a physical one */
      struct resolve r;
      if (!mmu_resolve_address(mobo, mobo->mmu.staging_register, &r)) {
        mobo->mmu.staging_register = -1;
        // fprintf(stderr, "failed to translate address\n");
      } else {
        // fprintf(stderr,
        //         "TRANSLATING: %#x to %#x\n",
        //         mobo->mmu.staging_register,
        //         r.phys_addr);
        mobo->mmu.staging_register = r.phys_addr;
      }

      return true;
    } else {
      return false;
    }
  } else {
    return false;
  }
}

void
tenc32_mmu_init(struct tenc32_motherboard_t* mobo)
{
  mobo->mmu.segment_table_offset = 0;

  mobo->mmu.tlb.policy_data = NULL;
  // mobo->mmu.tlb.policy_data = malloc(sizeof(struct tlb_policy));
  // assert(mobo->mmu.tlb.policy_data);

  struct brbt_policy page_policy;
  page_policy.policy_data = mobo;
  page_policy.abort = brbt_internal_abort;
  page_policy.insert_hook = NULL;
  page_policy.remove_hook = NULL;
  page_policy.resize = page_tlb_allocator;
  page_policy.free = NULL;
  page_policy.select = (brbt_policy_select)policy_decide;

  struct brbt_policy segment_policy;
  segment_policy.policy_data = mobo;
  segment_policy.abort = brbt_internal_abort;
  segment_policy.insert_hook = NULL;
  segment_policy.remove_hook = NULL;
  segment_policy.resize = segment_tlb_allocator;
  segment_policy.free = NULL;
  segment_policy.select = (brbt_policy_select)policy_decide;

  /* TODO: set up page table replacement algorithms */
  mobo->mmu.tlb.pages = brbt_create(sizeof(struct tlb_page_node),
                                    offsetof(struct tlb_page_node, id),
                                    page_policy,
                                    NULL,
                                    (brbt_comparator)page_table_comparison);

  mobo->mmu.tlb.segments =
    brbt_create(sizeof(struct tlb_segment_node),
                offsetof(struct tlb_segment_node, id),
                segment_policy,
                NULL,
                (brbt_comparator)segment_table_comparison);

  mobo->mmu.hardware_io = brbt_create(sizeof(struct tenc32_hardware_io),
                                      offsetof(struct tenc32_hardware_io, id),
                                      brbt_create_default_policy(),
                                      NULL,
                                      (brbt_comparator)hardware_io_comparison);

  struct tenc32_hardware_io mmu_self_io;
  mmu_self_io.id = 0x0000;
  mmu_self_io.data = mobo;
  mmu_self_io.read = mmu_self_read;
  mmu_self_io.write = mmu_self_write;
  mmu_self_io.cleanup = NULL;

  tenc32_add_io_space(mobo, mmu_self_io);
}

void
tenc32_mmu_reset(struct tenc32_motherboard_t* mobo)
{
  /* dont clear the hardware io */
  brbt_clear(&mobo->mmu.tlb.pages);
  brbt_clear(&mobo->mmu.tlb.segments);

  mobo->mmu.segment_table_offset = 0xFFFFFFFF;
  mobo->mmu.staging_register = 0;

  /* set up the default segments */
  struct tlb_segment_node code_segment = {
    .id = TENC32_STARTUP_CODE_SEGMENT << 22,
    .flags.active = true,
    .flags.execute = true,
    .flags.protected = true,
    .flags.io = false,
    .flags.paged = false,
    .flags.write = false,
    .data.unpaged.offset = TENC32_STARTUP_CODE_SEGMENT_PHYSICAL_OFFSET,
    .data.unpaged.size = TENC32_STARTUP_CODE_SEGMENT_SIZE,
  };

  struct tlb_segment_node data_segment = {
    .id = TENC32_STARTUP_DATA_SEGMENT << 22,
    .flags.active = true,
    .flags.write = true,
    .flags.protected = true,
    .flags.execute = false,
    .flags.io = false,
    .flags.paged = false,
    .data.unpaged.offset = TENC32_STARTUP_DATA_SEGMENT_PHYSICAL_OFFSET,
    .data.unpaged.size = TENC32_STARTUP_DATA_SEGMENT_SIZE,
  };

  struct tlb_segment_node mmu_io_segment = {
    .id = 0x3FF << 22,
    .flags.active = true,
    .flags.write = true,
    .flags.protected = true,
    .flags.io = true,
    .flags.execute = false,
    .flags.paged = false,
    .data.io.hardware_io = 0x000,
  };

  brbt_insert(&mobo->mmu.tlb.segments, &code_segment, true);
  brbt_insert(&mobo->mmu.tlb.segments, &data_segment, true);
  brbt_insert(&mobo->mmu.tlb.segments, &mmu_io_segment, true);
}

bool
tenc32_add_io_space(tenc32_motherboard_t* mobo, struct tenc32_hardware_io io)
{
  if (brbt_find(&mobo->mmu.hardware_io, &io.id) != BRBT_NIL)
    return false;

  brbt_insert(&mobo->mmu.hardware_io, &io, true);

  return true;
}

/* must be aligned */
static uint32_t
word_at_phys_addr(tenc32_motherboard_t* mobo, uint32_t phys)
{
  if (phys % 4)
    return 0;

  return *(uint32_t*)&mobo->memory[phys];
}

static struct segment_table_entry
read_segment_entry_at_location(tenc32_motherboard_t* mobo, uint32_t phys)
{
  struct segment_table_entry out;

  out.flags = *(uint32_t*)&mobo->memory[phys];
  out.id = word_at_phys_addr(mobo, phys + 0x04);
  out.b = word_at_phys_addr(mobo, phys + 0x08);
  out.c = word_at_phys_addr(mobo, phys + 0x0c);

  return out;
}

static struct page_table_entry
read_page_entry_at_location(tenc32_motherboard_t* mobo, uint32_t phys)
{
  struct page_table_entry out;

  out.flags = *(uint32_t*)&mobo->memory[phys];
  out.offset = word_at_phys_addr(mobo, phys + sizeof(uint32_t) * 1);

  return out;
}

static bool
segment_walk_dump(tenc32_motherboard_t* mobo)
{
  assert(mobo);

  /* TODO: add an exception here */
  uint32_t phys_offset = mobo->mmu.segment_table_offset;
  uint32_t len = word_at_phys_addr(mobo, phys_offset);
  phys_offset += 0x10; // aligned

  printf("segment table offset: %#.6x\n", mobo->mmu.segment_table_offset);
  printf("segment table length: %#.6x\n", len);

  for (unsigned i = 0; i < len; i++) {
    struct segment_table_entry out =
      read_segment_entry_at_location(mobo, phys_offset);

    fprintf(stderr, "ID %#.6x\n", out.id);

    phys_offset += 0x10;
  }

  return false;
}

static bool

segment_walk(tenc32_motherboard_t* mobo,
             uint32_t addr,
             struct segment_table_entry* out)
{
  assert(mobo);
  assert(out);

  /* TODO: add an exception here */
  uint32_t req_seg = TENC32_GET_SEGMENT(addr);
  uint32_t phys_offset = mobo->mmu.segment_table_offset;
  uint32_t len = word_at_phys_addr(mobo, phys_offset);
  phys_offset += 0x10; // aligned

  for (unsigned i = 0; i < len; i++) {
    *out = read_segment_entry_at_location(mobo, phys_offset);
    if (out->id == req_seg)
      return true;

    phys_offset += 0x10;
  }

  return false;
}

static bool
page_walk(tenc32_motherboard_t* mobo,
          uint32_t addr,
          struct page_table_entry* out)
{
  assert(mobo);
  assert(out);

  uint32_t req_seg = TENC32_GET_SEGMENT(addr);

  uint32_t len = mobo->mmu.segment_table_offset;

  if (req_seg >= len)
    return false;

  /* each segment descriptor is 8 bytes */
  uint32_t size = len * 8;
  uint32_t offset = size + len;

  *out = read_page_entry_at_location(mobo, offset);

  return true;
}

/* returns the physical address of the requested memory location */
static bool
mmu_resolve_address(tenc32_motherboard_t* mobo,
                    uint32_t req,
                    struct resolve* out)
{
  struct tlb_segment_node* segment = tlb_segment_consult(mobo, req);

  /* TODO: throw a general segmentation fault */
  if (!segment || !segment->flags.active) {
    mobo->cpu.exception = TENC32_INTERRUPT_SEGMENTATION_FAULT;
    EDPRINTV("segment nonexistent or inactive %s %s",
             segment ? "true" : "false",
             segment ? segment->flags.active ? "true" : "false" : "");
    // dump_tlb(mobo);
    segment_walk_dump(mobo);
    return false;
  }

  /* user mode execution cannot access protected segments */
  if (segment->flags.protected && mobo->cpu.crs.cr0 & TENC32_CR0_MODE) {
    mobo->cpu.exception = TENC32_INTERRUPT_SEGMENTATION_FAULT;
    EDPRINT("segment protected and not in kernel mode");
    return false;
  }

  out->segment = segment;

  out->paged = segment->flags.paged;
  out->io_space = segment->flags.io;

  if (out->io_space) {
    out->mmio_space = segment->data.io.hardware_io;
    out->phys_addr = TENC32_GET_SEGMENT_OFFSET(req);
    return true;
  }

  if (segment->flags.paged) {
    struct tlb_page_node* page = tlb_page_consult(mobo, req);
    out->page = page;

    if (!page) {
      /* null is only returned if and only if the page doesnt exist */
      mobo->cpu.exception = TENC32_INTERRUPT_SEGMENTATION_FAULT;
      EDPRINT("page nonexistent in segment");
      return false;
    }

    if (!page->flags.active) {
      mobo->cpu.exception = TENC32_INTERRUPT_INACTIVE_PAGE;
      EDPRINT("page not active in segment");
      return false;
    }

    out->phys_addr = TENC32_GET_PAGE_OFFSET(req) + page->offset;
  } else {
    uint32_t off = TENC32_GET_SEGMENT_OFFSET(req);
    uint32_t loc = off + segment->data.unpaged.offset;

    if (loc >= segment->data.unpaged.offset + segment->data.unpaged.size) {
      mobo->cpu.exception = TENC32_INTERRUPT_SEGMENTATION_FAULT;
      EDPRINT("attempting to read past the segment end in unpaged segment");
      /* further checking should be performed by the consumer such that
       * the total bytes read of the address does not leave the bounds
       */
      return 0;
    }

    out->phys_addr = loc;
  }

  return true;
}

bool
tenc32_read_word(tenc32_motherboard_t* mobo, uint32_t addr, uint32_t* out)
{
  /* words must be read on a boundary */
  if (addr % 4 != 0) {
    mobo->cpu.exception = TENC32_INTERRUPT_UNALIGNED_ACCESS;
    EDPRINT("misaligned word read");
    return false;
  }

  struct resolve r;
  if (!mmu_resolve_address(mobo, addr, &r))
    return false;

  if (r.io_space) {
    brbt_node handler_node = brbt_find(&mobo->mmu.hardware_io, &r.mmio_space);
    if (handler_node == BRBT_NIL) {
      mobo->cpu.exception = TENC32_INTERRUPT_INVALID_IO_SPACE;
      EDPRINT("nonexistent io space read");
      return false;
    }

    struct tenc32_hardware_io* io =
      brbt_get(&mobo->mmu.hardware_io, handler_node);

    if (!io->read(io->data, addr, out)) {
      mobo->cpu.exception = TENC32_INTERRUPT_SEGMENTATION_FAULT;
      EDPRINT("invalid io space read");
      return false;
    }

    return true;
  }

  if (r.paged)
    r.page->flags.read = true;

  *out = *(uint32_t*)&mobo->memory[r.phys_addr];
  return true;
}

bool
tenc32_write_word(tenc32_motherboard_t* mobo, uint32_t addr, uint32_t in)
{
  /* words must be read on a boundary */
  if (addr % 4 != 0) {
    mobo->cpu.exception = TENC32_INTERRUPT_UNALIGNED_ACCESS;
    EDPRINT("misaligned word write");
    return false;
  }

  struct resolve r;

  if (!mmu_resolve_address(mobo, addr, &r))
    return false;

  if (r.io_space) {
    brbt_node handler_node = brbt_find(&mobo->mmu.hardware_io, &r.mmio_space);
    if (handler_node == BRBT_NIL) {
      mobo->cpu.exception = TENC32_INTERRUPT_INVALID_IO_SPACE;
      EDPRINT("nonexistent io space write");
      return false;
    }

    struct tenc32_hardware_io* io =
      brbt_get(&mobo->mmu.hardware_io, handler_node);

    if (!io->write(io->data, addr, in)) {
      mobo->cpu.exception = TENC32_INTERRUPT_SEGMENTATION_FAULT;
      EDPRINT("invalid io space write");
      return false;
    }

    return true;
  }

  if (r.paged)
    r.page->flags.read = true;

  *(uint32_t*)&mobo->memory[r.phys_addr] = in;
  return true;
}

bool
tenc32_read_mem(tenc32_motherboard_t* mobo,
                uint32_t addr,
                uint32_t length,
                char* buf)
{
  unsigned start = addr;
  unsigned end = addr + length;

  /* read/writes CANNOT cross segment boundaries */
  if ((start & TENC32_SEGMENT_MASK) != (end & TENC32_SEGMENT_MASK))
    return false;

  /* get the segment type */
  struct tlb_segment_node* segment = tlb_segment_consult(mobo, start);

  if (segment->flags.paged) {
    fprintf(stderr,
            "tenc32_read_mem does not currently support reading from pages\n");
    assert(false);
  } else {
    unsigned segment_end =
      segment->data.unpaged.offset + segment->data.unpaged.size;
    unsigned phys_begin =
      segment->data.unpaged.offset + TENC32_GET_SEGMENT_OFFSET(addr);
    unsigned phys_end = phys_begin + length;

    if (phys_end > segment_end)
      return false;

    memcpy(buf, &mobo->memory[phys_begin], length);
  }

  return true;
}

bool
tenc32_write_mem(tenc32_motherboard_t* mobo,
                 uint32_t addr,
                 uint32_t length,
                 char const* restrict data)
{
  unsigned start = addr;
  unsigned end = addr + length;

  /* read/writes CANNOT cross segment boundaries */
  if ((start & TENC32_SEGMENT_MASK) != (end & TENC32_SEGMENT_MASK))
    return false;

  /* get the segment type */
  struct tlb_segment_node* segment = tlb_segment_consult(mobo, start);

  if (segment->flags.paged) {
    fprintf(stderr,
            "tenc32_read_mem does not currently support reading from pages\n");
    assert(false);
  } else {
    unsigned segment_end =
      segment->data.unpaged.offset + segment->data.unpaged.size;
    unsigned phys_begin =
      segment->data.unpaged.offset + TENC32_GET_SEGMENT_OFFSET(addr);
    unsigned phys_end = phys_begin + length;

    if (phys_end > segment_end)
      return false;

    memcpy(&mobo->memory[phys_begin], data, length);
  }

  return true;
}

bool
tenc32_read_byte(tenc32_motherboard_t* mobo, uint32_t addr, uint32_t* out)
{
  struct resolve r;
  if (!mmu_resolve_address(mobo, addr, &r))
    return false;

  if (r.io_space) {
    brbt_node handler_node = brbt_find(&mobo->mmu.hardware_io, &r.mmio_space);
    if (handler_node == BRBT_NIL) {
      mobo->cpu.exception = TENC32_INTERRUPT_INVALID_IO_SPACE;
      EDPRINT("nonexistent io space read");
      return false;
    }

    struct tenc32_hardware_io* io =
      brbt_get(&mobo->mmu.hardware_io, handler_node);

    if (!io->read(io->data, addr, out)) {
      mobo->cpu.exception = TENC32_INTERRUPT_SEGMENTATION_FAULT;
      EDPRINT("invalid io space read");
      return false;
    }

    return true;
  }

  if (r.paged)
    r.page->flags.read = true;

  *out = mobo->memory[r.phys_addr];
  return true;
}

bool
tenc32_write_byte(tenc32_motherboard_t* mobo, uint32_t addr, uint32_t in)
{
  struct resolve r;

  if (!mmu_resolve_address(mobo, addr, &r))
    return false;

  if (r.io_space) {
    brbt_node handler_node = brbt_find(&mobo->mmu.hardware_io, &r.mmio_space);
    if (handler_node == BRBT_NIL) {
      mobo->cpu.exception = TENC32_INTERRUPT_INVALID_IO_SPACE;
      EDPRINT("nonexistent io space write");
      return false;
    }

    struct tenc32_hardware_io* io =
      brbt_get(&mobo->mmu.hardware_io, handler_node);

    if (!io->write(io->data, addr, in)) {
      mobo->cpu.exception = TENC32_INTERRUPT_SEGMENTATION_FAULT;
      EDPRINT("invalid io space write");
      return false;
    }

    return true;
  }

  if (r.paged)
    r.page->flags.read = true;

  *(uint8_t*)&mobo->memory[r.phys_addr] = in;

  return true;
}

struct tlb_page_node*
tlb_page_consult(tenc32_motherboard_t* mobo, uint32_t addr)
{
  /* include both the segment and the page id */
  addr &= (~0u << TENC32_PAGE_OFFSET_BITS);
  brbt_node node = brbt_find(&mobo->mmu.tlb.pages, &addr);

  /* add it to the tlb */
  if (node == BRBT_NIL) {
    struct page_table_entry entry;
    if (!page_walk(mobo, addr, &entry))
      return NULL;

    struct tlb_page_node new_node;
    new_node.id = addr;
    new_node.offset = entry.offset;
    new_node.flags.active = entry.flags & TENC32_PAGE_ACTIVE;
    new_node.flags.written = entry.flags & TENC32_PAGE_WRITTEN;
    new_node.flags.read = entry.flags & TENC32_PAGE_READ;

    brbt_insert(&mobo->mmu.tlb.pages, &new_node, true);
    node = brbt_find(&mobo->mmu.tlb.pages, &addr);
    if (node == BRBT_NIL)
      fprintf(
        stderr,
        "node not found in TLB page table despite being just inserted?\n"),
        abort();
  }

  return brbt_get(&mobo->mmu.tlb.pages, node);
}

struct tlb_segment_node*
tlb_segment_consult(tenc32_motherboard_t* mobo, uint32_t addr)
{
  addr &= (~0u << 22);
  brbt_node node = brbt_find(&mobo->mmu.tlb.segments, &addr);

  /* have to add it to the tlb */
  if (node == BRBT_NIL) {
    struct segment_table_entry entry;
    if (!segment_walk(mobo, addr, &entry))
      return NULL;

    struct tlb_segment_node new_node;
    new_node.id = addr;
    new_node.flags.active = entry.flags & TENC32_SEGMENT_DESCRIPTOR_ACTIVE;
    new_node.flags.write = entry.flags & TENC32_SEGMENT_DESCRIPTOR_WRITE;
    new_node.flags.execute = entry.flags & TENC32_SEGMENT_DESCRIPTOR_EXECUTE;
    new_node.flags.paged = entry.flags & TENC32_SEGMENT_DESCRIPTOR_PAGED;
    new_node.flags.io = entry.flags & TENC32_SEGMENT_DESCRIPTOR_IO;
    new_node.flags.protected =
      entry.flags & TENC32_SEGMENT_DESCRIPTOR_PROTECTED;

    /* segments cannot be paged and represent IO space at the same time */
    if (new_node.flags.paged && new_node.flags.io) {
      mobo->cpu.exception = TENC32_INTERRUPT_SEGMENTATION_FAULT;
      EDPRINT("segment is paged and io");
      return NULL;
    }

    if (new_node.flags.paged)
      new_node.data.paged.offset = entry.b;
    else
      new_node.data.unpaged.offset = entry.b,
      new_node.data.unpaged.size = entry.c;

    brbt_insert(&mobo->mmu.tlb.segments, &new_node, true);
    node = brbt_find(&mobo->mmu.tlb.segments, &addr);
    if (node == BRBT_NIL)
      fprintf(
        stderr,
        "node not found in TLB segment table despite being just inserted?\n"),
        exit(1);
  }

  return brbt_get(&mobo->mmu.tlb.segments, node);
}

[[maybe_unused]] static void
dump_tlb_visit(struct brbt* brbt, void* userdata, brbt_node val)
{
  (void)brbt;
  (void)userdata;
  (void)val;
#ifdef EN_EDPRINT
  (void)userdata;
  struct tlb_segment_node* entry = brbt_get(brbt, val);
  fprintf(stderr, "SEGMENT DUMP: ID %#.6x\n", entry->id);
#endif
}

[[maybe_unused]] static void
dump_tlb(tenc32_motherboard_t* mobo)
{
  (void)mobo;
#ifdef EN_EDPRINT
  brbt_iterate(&mobo->mmu.tlb.segments, dump_tlb_visit, mobo);
#endif
}
