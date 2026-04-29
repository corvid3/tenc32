#include "crowcpu.h"
#include "mobo.h"
#include <threads.h>

static bool
picrd(tenc32_motherboard_t* mobo, unsigned addr, unsigned* out)
{
  (void)mobo;
  (void)addr;
  (void)out;
  return false;
}

static bool
picwr(tenc32_motherboard_t* mobo, unsigned addr, unsigned in)
{
  addr &= 0xFFFF;

  if (addr == 0) {
    mtx_lock(&mobo->pic.mutex);
    mobo->pic.incoming_flags &= ~(1U << in);
    mobo->pic.currently_handled &= ~(1U << in);
    mtx_unlock(&mobo->pic.mutex);
  } else
    return false;

  return true;
}

void
tenc32_init_pic(tenc32_motherboard_t* mobo)
{
  struct tenc32_hardware_io io;
  io.id = 0x02;
  io.data = mobo;
  io.read = (tenc32_hardware_read)picrd;
  io.write = (tenc32_hardware_write)picwr;
  io.cleanup = 0;
  if (!tenc32_add_io_space(mobo, io))
    fprintf(stderr, "tenc32 fatal error: unable to insert pic io space\n"),
      exit(1);
}
