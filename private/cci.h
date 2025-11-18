#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
  bool enabled;
  int socket;

  bool phys_addr_set;
  uint32_t phys_addr_base;

  struct pollfd* this_pollfd;
} cci_dev;

/* crow component interface
 * virtual component interface over the network
 */
typedef struct
{
  int listening_socket;
  cci_dev* devices;
  uint32_t devices_cap;

  uint32_t last_connection_tick;
  uint32_t last_update_tick;

  /* used by the emulator
   * size = devices_cap
   */
  struct pollfd* pollfds;
} cci_t;

typedef struct
{
  bool write;
  uint32_t bytesize;
  void* opt_payload;
  void (*handler)(void* data);
} cci_msg;
