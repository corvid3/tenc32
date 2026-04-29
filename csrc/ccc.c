#include "ccc.h"
#include "crowcpu.h"
#include "mobo.h"

enum
{
  command_register = 0x00,
  answer_register = 0x10,
};

static bool
cpcrd(tenc32_motherboard_t* mobo, unsigned addr, unsigned* out)
{
  addr &= UINT16_MAX;

  if (addr == answer_register) {
    *out = mobo->ccc.answer_register;
    return true;
  }

  return false;
}

static bool
cpcwr(tenc32_motherboard_t* mobo, unsigned addr, unsigned val)
{
  (void)val;
  addr &= UINT16_MAX;

  if (addr == command_register) {
    switch (val) {
      case 0:
        mobo->poweroff = true;
        break;

      case 1:
        mobo->ccc.answer_register = mobo->memory_size;
        break;
      default:
        return false;
    }
  } else {
    return false;
  }

  return true;
}

void
tenc32_init_ccc(tenc32_motherboard_t* mobo)
{
  struct tenc32_hardware_io io;
  io.id = 0x01;
  io.read = (tenc32_hardware_read)cpcrd;
  io.write = (tenc32_hardware_write)cpcwr;
  io.cleanup = NULL;
  io.data = mobo;

  if (!tenc32_add_io_space(mobo, io))
    fprintf(stderr, "tenc32 fatal error: unable to insert cpc io space\n"),
      exit(1);
}
