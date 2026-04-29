#pragma once

#include "crowcpu.h"

/* corvid chipset controller
 * manages information about the core motherboard of the systemn */
struct ccc_t
{
  uint32_t answer_register;
};

void
tenc32_init_ccc(tenc32_motherboard_t*);
