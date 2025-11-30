#include "cpc.h"
#include "crowcpu.h"
#include "mobo.h"

static bool
cpcrd(tenc32_motherboard_t* mobo, unsigned id, unsigned* out)
{
  (void)mobo;
  (void)id;
  (void)out;
  return false;
}

static bool
cpcwr(tenc32_motherboard_t* mobo, unsigned addr, unsigned val)
{
  (void)val;
  addr &= 0xffff;

  switch (addr) {
    case 0:
      mobo->poweroff = true;
      break;

    default:
      return false;
  }

  return true;
}

void
tenc32_init_cpc(tenc32_motherboard_t* mobo)
{
  struct tenc32_hardware_io io;
  io.id = 0x01;
  io.read = (tenc32_hardware_read)cpcrd;
  io.write = (tenc32_hardware_write)cpcwr;
  io.cleanup = NULL;
  io.data = mobo;
  tenc32_add_io_space(mobo, io);
}
