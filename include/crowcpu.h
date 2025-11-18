#pragma once

#include <crow.crowcpu_arch/crowcpu_arch.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* cpu comes with an MMU baked in, non-interchangable
 * booting involves the segment tables & interrupt tables being cleared,
 * the segment table is set with two segments,
 * & the rom is loaded into the two segments.
 *
 * boot segments:
 *   * segment 0: executable segment, r/x, 4kb length @ physical address 0x5000
 *   * segment 1: data segment, r/w, 4kb length @ physical address 0x6000
 *
 * the bios rom must contain a POFF executable (prt)
 *   * section ''
 *   * section 'text': loaded into segment 0
 *   * section 'data': loaded into segment 1
 *
 */

/* CCI status register
 * constains information/state constantly updated by
 * the CCI controller on the motherboard
 *
 * 4u : index to which device triggered the current interrupt
 * 4u : number of CCI slots available (static)
 *
 */
#define CCI_STATUS_OFFSET 0x5000

/* CCI device configuration space
 * each available slot in the CCI bus has a related header here
 * the device configuration header provides an interface for
 *  initializing peripherals on the CCI bus
 *
 * 4u  r  : device ID register
 * 4u  r  : vendor
 * 4u r/w : offset
 *        :   write to this register to change the memory mapped location
 * 4u  r  : size
 * 4u  r  : flags
 * 4u     : reserved
 * 4u     : reserved
 * 4u     : reserved
 */
#define CCI_DEVICE_CONFIGURATION_OFFSET 0x5100

typedef enum
{
  /* is there a peripheral installed in this slot? */
  CROWCPU_CCI_DC_FLAG_ACTIVE = 0b0001,
} crowcpu_cci_device_configuration_flags;

typedef struct crowcpu_motherboard_t crowcpu_motherboard_t;

typedef struct
{
  /* how much time must pass, in ms, before the CCI adapter
   * looks for newly connected devices
   */
  uint32_t cci_con_ms;

  /* how much time must pass, in ms, before the CCI adapter
   * will look for & potentially perform state changes and
   * trigger an interrupt request
   */
  uint32_t cci_update_ms;
} crowcpu_configuration_t;

/* in IO space, addresses are not required to congruent.
 * that is, a word write at address 0x00 may or may not cause
 *   the data at address 0x01 to be affected.
 * it is entirely up to the implementation of the hardware.
 */

/* return false to trigger a segmentation fault */
typedef bool (*crowcpu_hardware_read)(void* data, uint32_t addr, uint32_t* out);
/* return false to trigger a segmentation fault */
typedef bool (*crowcpu_hardware_write)(void* data, uint32_t addr, uint32_t val);
typedef void (*crowcpu_hardware_cleanup)(void* data);

struct crowcpu_hardware_io
{
  uint32_t id;
  void* data;
  crowcpu_hardware_read read;
  crowcpu_hardware_write write;
  crowcpu_hardware_cleanup cleanup;
};

enum crowcpu_step_val
{
  /* no changes in execution state */
  CROWCPU_STEP_OK,

  /* halting state */
  CROWCPU_STEP_HALT,

  /* hit a breakpoint */
  CROWCPU_STEP_BREAK,
};

void
crowcpu_default_configuration(crowcpu_configuration_t*);

/* make sure to invoke crowcpu_restart
 */
crowcpu_motherboard_t*
crowcpu_motherboard_create(crowcpu_configuration_t*, size_t ram_size);

/* attaches a cci (crow component interface) bus to the motherboard
 * if you want the CPU to be able to talk to the universe, you want this
 */
void
crowcpu_motherboard_add_cci(crowcpu_motherboard_t* mobo,
                            short port,
                            short max_components);

void
crowcpu_motherboard_send_irq(crowcpu_motherboard_t* mobo, unsigned short);

void
crowcpu_motherboard_destroy(crowcpu_motherboard_t*);

/* performs a logical restart of the cpu
 *  - loads the bios segments into memory
 *  - sets the default segment table
 *  - sets protection mode to 0
 */
void
crowcpu_restart(crowcpu_motherboard_t* cpu,
                char const (*bios)[CROWCPU_ROM_SIZE]);

enum crowcpu_step_val
crowcpu_step(crowcpu_motherboard_t* cpu);

/* works with logical segmentation addresses,
 * i.e. addr is NOT physical.
 */
bool
crowcpu_read_word(crowcpu_motherboard_t*, uint32_t addr, uint32_t* out);

/* works with logical segmentation addresses,
 * i.e. addr is NOT physical.
 */
bool
crowcpu_write_word(crowcpu_motherboard_t*, uint32_t addr, uint32_t in);

/* works with logical segmentation addresses,
 * i.e. addr is NOT physical.
 */
bool
crowcpu_read_byte(crowcpu_motherboard_t*, uint32_t addr, uint32_t* out);

/* works with logical segmentation addresses,
 * i.e. addr is NOT physical.
 */
bool
crowcpu_write_byte(crowcpu_motherboard_t*, uint32_t addr, uint32_t in);

/* returns false if IO space is already registered
 * 0x3FF is forcibly bound to the MMU's internals
 */
bool
crowcpu_add_io_space(crowcpu_motherboard_t*, struct crowcpu_hardware_io);

void
crowcpu_dump_registers(crowcpu_motherboard_t*);
