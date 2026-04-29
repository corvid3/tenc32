#include <assert.h>
#include <crow.crowcpu_arch/crowcpu_arch.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "crowcpu.h"
#include "mmu.h"
#include "mobo.h"

#define CLOCK_EMPTY ((unsigned)-1)
#define MAX_HARDWARE_IO 32

static void
dump_tlb(tenc32_motherboard_t* mobo);
static struct tlb_segment_node*
tlb_segment_insert(mmu_t* mmu, uint32_t segment);

static bool
mmu_resolve_address(tenc32_motherboard_t* mobo,
                    uint32_t req,
                    struct resolve* out);

static int
hardware_io_comparison(struct brbt const* tree [[maybe_unused]],
                       uint32_t const* lhs,
                       uint32_t const* rhs)
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

static bool
mmu_self_read(void* data, uint32_t addr, uint32_t* out)
{
  tenc32_motherboard_t* mobo = data;

  addr &= UINT16_MAX;
  if (addr != 0)
    return false;

  *out = mobo->mmu.staging_register;
  return true;
}

static bool
mmu_self_write(void* data, uint32_t addr, uint32_t val)
{
  tenc32_motherboard_t* mobo = data;

  addr &= UINT16_MAX;

  if (addr == 0) {
    mobo->mmu.staging_register = val;
    return true;
  }

  if (addr == 1) {
    if (val == 0) {
      /* update the table physical location */
      mobo->mmu.segment_table_offset = mobo->mmu.staging_register;
      return true;
    }

    if (val == 1) {
      /* invalidate the TLB */
      mobo->mmu.tlb.num_pages = 0;
      mobo->mmu.tlb.num_segments = 0;
      for (unsigned i = 0; i < TLB_SEGMENT_CAP; i++)
        mobo->mmu.tlb.segments[i].id = -1U;
      return true;
    }

    if (val == 2) {
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
    }
  }

  return false;
}

static struct brbt_policy default_policy;
[[gnu::constructor]] static void
init_default_policy()
{
  default_policy = brbt_create_default_policy();
}

void
tenc32_mmu_init(struct tenc32_motherboard_t* mobo)
{
  mobo->mmu.segment_table_offset = 0;

  static struct brbt_type const hardware_io_type = {
    .membs = sizeof(struct tenc32_hardware_io),
    .keyoff = offsetof(struct tenc32_hardware_io, id),
    .deleter = 0,
    .cmp = (brbt_comparator)hardware_io_comparison,
  };

  for (unsigned i = 0; i < TLB_SEGMENT_CAP; i++)
    mobo->mmu.tlb.segments[i].id = -1U;
  mobo->mmu.tlb.num_segments = 0;

  extern void tlb_segment_dump(tenc32_motherboard_t * mobo);

  mobo->mmu.hardware_io = brbt_create(&hardware_io_type, &default_policy, mobo);

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
  for (unsigned i = 0; i < TLB_SEGMENT_CAP; i++)
    mobo->mmu.tlb.segments[i].id = -1U;

  mobo->mmu.segment_table_offset = 0xFFFFFFFF;
  mobo->mmu.staging_register = 0;

  /* set up the default segments */
  struct tlb_segment_node code_segment = {
    .id = TENC32_STARTUP_CODE_SEGMENT,
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
    .id = TENC32_STARTUP_DATA_SEGMENT,
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
    .id = 0x3FF,
    .flags.active = true,
    .flags.write = true,
    .flags.protected = true,
    .flags.io = true,
    .flags.execute = false,
    .flags.paged = false,
    .data.io.hardware_io = 0x000,
  };

  *tlb_segment_insert(&mobo->mmu, TENC32_STARTUP_CODE_SEGMENT) = code_segment;
  *tlb_segment_insert(&mobo->mmu, TENC32_STARTUP_DATA_SEGMENT) = data_segment;
  *tlb_segment_insert(&mobo->mmu, 0x3FF) = mmu_io_segment;
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

extern bool
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

    fprintf(
      stderr, "ID %#.6x OFFSET: %#.6x SIZE: %#.6x\n", out.id, out.b, out.c);

    phys_offset += 0x10;
  }

  return false;
}

extern void
tlb_segment_dump(tenc32_motherboard_t* mobo)
{
  mmu_t* mmu = &mobo->mmu;

  for (unsigned i = 0; i < TLB_SEGMENT_CAP; i++) {
    struct tlb_segment_node* seg = &mmu->tlb.segments[i];
    if (seg->id == -1U)
      continue;
    fprintf(stderr, "TLB ID: %#.6x\n", seg->id);
  }
}

static bool
segment_walk(tenc32_motherboard_t* mobo,
             uint32_t req_seg,
             struct segment_table_entry* out)
{
  assert(mobo);
  assert(out);

  /* TODO: add an exception here */
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

[[maybe_unused]]
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
tenc32_read_word_ex(tenc32_motherboard_t* mobo,
                    uint32_t addr,
                    uint32_t* out,
                    struct resolve* rout)
{
  /* words must be read on a boundary */
  if (addr % 4 != 0) {
    mobo->cpu.exception = TENC32_INTERRUPT_UNALIGNED_ACCESS;
    EDPRINT("misaligned word read");
    return false;
  }

  if (!mmu_resolve_address(mobo, addr, rout))
    return false;

  if (rout->io_space) {
    brbt_node handler_node =
      brbt_find(&mobo->mmu.hardware_io, &rout->mmio_space);
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

  if (rout->paged)
    rout->page->flags.read = true;

  *out = *(uint32_t*)&mobo->memory[rout->phys_addr];
  return true;
}

bool
tenc32_read_word(tenc32_motherboard_t* mobo, uint32_t addr, uint32_t* out)
{
  struct resolve r;
  return tenc32_read_word_ex(mobo, addr, out, &r);
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

  mobo->memory[r.phys_addr] = in;

  return true;
}

struct tlb_page_node*
tlb_page_consult(tenc32_motherboard_t* mobo, uint32_t addr)
{
  /* unimplemented for now */
  (void)mobo;
  (void)addr;
  assert(false);
  // /* include both the segment and the page id */
  // addr &= (~0U << TENC32_PAGE_OFFSET_BITS);
  // brbt_node node = brbt_find(&mobo->mmu.tlb.pages, &addr);

  // /* add it to the tlb */
  // if (node == BRBT_NIL) {
  //   struct page_table_entry entry;
  //   if (!page_walk(mobo, addr, &entry))
  //     return NULL;

  //   struct tlb_page_node new_node;
  //   new_node.id = addr;
  //   new_node.offset = entry.offset;
  //   new_node.flags.active = entry.flags & TENC32_PAGE_ACTIVE;
  //   new_node.flags.written = entry.flags & TENC32_PAGE_WRITTEN;
  //   new_node.flags.read = entry.flags & TENC32_PAGE_READ;

  //   brbt_insert(&mobo->mmu.tlb.pages, &new_node, true);
  //   node = brbt_find(&mobo->mmu.tlb.pages, &addr);
  //   if (node == BRBT_NIL)
  //     fprintf(
  //       stderr,
  //       "node not found in TLB page table despite being just inserted?\n"),
  //       abort();
  // }

  // return brbt_get(&mobo->mmu.tlb.pages, node);
}

enum
{
  HASHMUL = 31,
};

static unsigned
hash_segment(uint32_t segment)
{
  unsigned out = segment * HASHMUL;
  if (out == -1U)
    out += 1;
  return out;
}

static unsigned
segment_iter(unsigned in)
{
  unsigned const out = in + 1;
  if (out >= TLB_SEGMENT_CAP)
    return 0;
  return out;
}

static struct tlb_segment_node*
segment_at(mmu_t* mmu, uint32_t idx)
{
  struct tlb_segment_node* node = &mmu->tlb.segments[idx];
  if (node->id == -1U)
    return 0;
  return node;
}

static struct tlb_segment_node*
tlb_segment_search(mmu_t* mmu, uint32_t segment)
{
  unsigned const hash = hash_segment(segment);
  unsigned idx = hash % TLB_SEGMENT_CAP;

  struct tlb_segment_node* node = 0;

  for (;;) {
    node = segment_at(mmu, idx);

    if (node == NULL)
      break;

    if (node->id == segment)
      break;

    idx = segment_iter(idx);
  }

  return node;
}

/* returns the segment slot to insert at */
static struct tlb_segment_node*
tlb_segment_insert(mmu_t* mmu, uint32_t segment)
{
  unsigned const hash = hash_segment(segment);
  unsigned idx = hash % TLB_SEGMENT_CAP;

  if (mmu->tlb.num_segments >= TLB_SEGMENT_MAXSIZE) {
    /* replacement policy is just to... remove the first
     * one in the list where the hash index is.
     * this is lazy, and it would probably be better
     * to remove with at least a little bit of randomness
     * but it works for now. */
    return &mmu->tlb.segments[idx];
  }

  struct tlb_segment_node* node = 0;

  for (;;) {
    node = &mmu->tlb.segments[idx];
    if (node->id == -1U || node->id == segment)
      break;
    idx = segment_iter(idx);
  }

  return node;
}

static bool
segment_entry_to_node(tenc32_motherboard_t* mobo,
                      struct segment_table_entry entry,
                      struct tlb_segment_node* out)
{
  out->id = entry.id;
  out->flags.active = entry.flags & TENC32_SEGMENT_DESCRIPTOR_ACTIVE;
  out->flags.write = entry.flags & TENC32_SEGMENT_DESCRIPTOR_WRITE;
  out->flags.execute = entry.flags & TENC32_SEGMENT_DESCRIPTOR_EXECUTE;
  out->flags.paged = entry.flags & TENC32_SEGMENT_DESCRIPTOR_PAGED;
  out->flags.io = entry.flags & TENC32_SEGMENT_DESCRIPTOR_IO;
  out->flags.protected = entry.flags & TENC32_SEGMENT_DESCRIPTOR_PROTECTED;

  /* segments cannot be paged and represent IO space at the same time */
  if (out->flags.paged && out->flags.io) {
    mobo->cpu.exception = TENC32_INTERRUPT_SEGMENTATION_FAULT;
    EDPRINT("segment is paged and io");
    return false;
  }

  if (out->flags.paged)
    out->data.paged.offset = entry.b;
  else
    out->data.unpaged.offset = entry.b, out->data.unpaged.size = entry.c;

  return true;
}

struct tlb_segment_node*
tlb_segment_consult(tenc32_motherboard_t* mobo, uint32_t addr)
{
  addr = addr >> TENC32_SEGMENT_SELECTOR_OFFSET;
  struct tlb_segment_node* node = tlb_segment_search(&mobo->mmu, addr);
  if (node)
    return node;

  struct segment_table_entry entry;
  if (!segment_walk(mobo, addr, &entry)) {
    printf("failed to find segment while walking table\n");
    return 0;
  }

  struct tlb_segment_node* out = tlb_segment_insert(&mobo->mmu, addr);
  assert(out);

  if (!segment_entry_to_node(mobo, entry, out)) {
    printf("failed to convert segment entry to node\n");
    return 0;
  }

  mobo->mmu.tlb.num_segments++;
  return out;
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
