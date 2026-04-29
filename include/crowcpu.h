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
  TENC32_CCI_DC_FLAG_ACTIVE = 0b0001,
} tenc32_cci_device_configuration_flags;

typedef struct tenc32_motherboard_t tenc32_motherboard_t;

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
} tenc32_configuration_t;

/* in IO space, addresses are not required to congruent.
 * that is, a word write at address 0x00 may or may not cause
 *   the data at address 0x01 to be affected.
 * it is entirely up to the implementation of the hardware.
 */

/* return false to trigger a segmentation fault */
typedef bool (*tenc32_hardware_read)(void* data, uint32_t addr, uint32_t* out);
/* return false to trigger a segmentation fault */
typedef bool (*tenc32_hardware_write)(void* data, uint32_t addr, uint32_t val);
typedef void (*tenc32_hardware_cleanup)(void* data);

struct tenc32_hardware_io
{
  uint32_t id;
  void* data;
  tenc32_hardware_read read;
  tenc32_hardware_write write;
  tenc32_hardware_cleanup cleanup;
};

enum tenc32_step_val
{
  /* no changes in execution state */
  TENC32_STEP_OK,

  /* halting state (NOT poweroff!) */
  TENC32_STEP_HALT,

  TENC32_STEP_POWEROFF,

  /* crashing halt, double exception */
  TENC32_STEP_CRASH,

  /* hit a breakpoint */
  TENC32_STEP_BREAK,
};

void
tenc32_default_configuration(tenc32_configuration_t*);

/* make sure to invoke tenc32_restart
 */
tenc32_motherboard_t*
tenc32_motherboard_create(tenc32_configuration_t*, size_t ram_size);

/* attaches a cci (crow component interface) bus to the motherboard
 * if you want the CPU to be able to talk to the universe, you want this
 */
void
tenc32_motherboard_add_cci(tenc32_motherboard_t* mobo,
                           short port,
                           short max_components);

/* only used for hardware interrupt requests
 * software interrupts occur through machine executable codl
 */
// void
// tenc32_motherboard_send_irq(tenc32_motherboard_t* mobo, unsigned short);

void
tenc32_motherboard_destroy(tenc32_motherboard_t*);

/* function that gets ran on an exception
 * for debugging os/bios etc
 */
void
tenc32_insert_exception_callback(tenc32_motherboard_t*,
                                 void (*)(tenc32_motherboard_t*, unsigned));

/* performs a logical restart of the cpu
 *  - loads the bios segments into memory
 *  - sets the default segment table
 *  - sets protection mode to 0
 */
void
tenc32_restart(tenc32_motherboard_t* mobo, char const (*bios)[TENC32_ROM_SIZE]);

enum tenc32_step_val
tenc32_step(tenc32_motherboard_t* cpu);

/* works with logical segmentation addresses,
 * i.e. addr is NOT physical.
 */
bool
tenc32_read_word(tenc32_motherboard_t*, uint32_t addr, uint32_t* out);

/* works with logical segmentation addresses,
 * i.e. addr is NOT physical.
 */
bool
tenc32_write_word(tenc32_motherboard_t*, uint32_t addr, uint32_t in);

/* works with logical segmentation addresses,
 * i.e. addr is NOT physical.
 */
bool
tenc32_read_byte(tenc32_motherboard_t*, uint32_t addr, uint32_t* out);

/* works with logical segmentation addresses,
 * i.e. addr is NOT physical.
 */
bool
tenc32_write_byte(tenc32_motherboard_t*, uint32_t addr, uint32_t in);

bool
tenc32_read_mem(tenc32_motherboard_t*,
                uint32_t addr,
                uint32_t length,
                char* buffer);

bool
tenc32_write_mem(tenc32_motherboard_t*,
                 uint32_t addr,
                 uint32_t length,
                 char const* data);

/* thread-safe, fine to call asynchronously */
void
tenc32_trigger_hardware_interrupt(tenc32_motherboard_t*, unsigned irq);

/* thread-safe, fine to call asynchronously
 * triggers the non-maskable interrupt
 * passes a single value on the stack (parameter value)
 * useful for stuff like shutting down
 */
void
tenc32_trigger_nonmaskable_interrupt(tenc32_motherboard_t*, unsigned value);

/* returns false if IO space is already registered
 * 0x3FF is forcibly bound to the MMU's internals
 */
bool
tenc32_add_io_space(tenc32_motherboard_t*, struct tenc32_hardware_io);

void
tenc32_dump_registers(tenc32_motherboard_t*);

/* allows a thread to sleep until a motherboard gets an interrupt */
void
tenc32_halt_sleep(tenc32_motherboard_t*);

/* wakes up a halt-waiting thread. useful if you need
 * to wake a thread up to shut down a motherboard from an interrupt */
void
tenc32_awake_mobo(tenc32_motherboard_t*);
