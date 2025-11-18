#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "mobo.h"
void
crowcpu_motherboard_add_cci(crowcpu_motherboard_t* mobo,
                            short port,
                            short max_components)
{
  if (mobo->cci.devices != NULL)
    return fprintf(stderr,
                   "attempting to attach more than one cci bus to a cpu\n"),
           (void)0;

  mobo->cci.listening_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (mobo->cci.listening_socket == -1)
    fprintf(stderr, "unable to create socket\n");

  if (fcntl(mobo->cci.listening_socket,
            F_SETFL,
            fcntl(mobo->cci.listening_socket, F_GETFL) | O_NONBLOCK) < 0)
    fprintf(stderr, "unable to set listening cci socket as nonblocking\n");

  int val = 1;
  setsockopt(
    mobo->cci.listening_socket, SOL_SOCKET, SO_REUSEADDR, &val, sizeof val);

  struct sockaddr_in addr;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_family = AF_INET;
  memset(addr.sin_zero, 0, sizeof addr.sin_zero);

  if (bind(mobo->cci.listening_socket, (struct sockaddr*)&addr, sizeof addr) ==
      -1)
    return fprintf(stderr, "unable to bind cci bus to socket\n"), (void)0;

  if (listen(mobo->cci.listening_socket, max_components) == -1)
    return fprintf(stderr, "unable to listen cci socket\n"), (void)0;

  mobo->cci.devices_cap = max_components;
  mobo->cci.devices = malloc(sizeof *mobo->cci.devices * mobo->cci.devices_cap);
  mobo->cci.pollfds = malloc(sizeof(struct pollfd) * mobo->cci.devices_cap);

  for (uint32_t i = 0; i < mobo->cci.devices_cap; i++) {
    mobo->cci.devices[i].enabled = false;
    mobo->cci.devices[i].this_pollfd = &mobo->cci.pollfds[i];
    mobo->cci.pollfds[i].fd = -1;
  }
}

static bool
cci_interact(cci_dev* dev, cci_msg* msgs, uint32_t num)
{
  for (uint32_t i = 0; i < num; i++) {
    cci_msg msg = msgs[i];

    if (msg.bytesize > 16)
      return false;

    if (!msg.write) {
      assert(msg.handler != NULL);
      void* val = __builtin_alloca(msg.bytesize);
      uint32_t bytes_read = recv(dev->socket, val, msg.bytesize, 0);
      if (bytes_read != msg.bytesize)
        return false;
      msg.handler(val);
    } else {
      assert(msg.opt_payload != NULL);
      uint32_t bytes_written =
        send(dev->socket, msg.opt_payload, msg.bytesize, 0);
      if (bytes_written != msg.bytesize)
        return false;
    }
  }

  return true;
}

static void
cci_disconnect(cci_dev* dev)
{
  close(dev->socket);
  dev->enabled = false;
  dev->this_pollfd->fd = -1;
}

/* returns index of free device
 * returns -1 if unable to find one
 */
static int
cci_find_open_dev(crowcpu_motherboard_t* mobo)
{
  for (uint32_t i = 0; i < mobo->cci.devices_cap; i++) {
    cci_dev* dev = &mobo->cci.devices[i];
    if (dev->enabled)
      return i;
  }

  return -1;
}

static uint32_t cci_register_device_id;
static uint32_t cci_register_vendor_id;
static uint32_t cci_register_required_space;

static void
cci_accept_stage0(void* data)
{
  uint32_t unwrap[3];
  memcpy(unwrap, data, sizeof unwrap);
  cci_register_device_id = ntohl(unwrap[0]);
  cci_register_vendor_id = ntohl(unwrap[1]);
  cci_register_required_space = ntohl(unwrap[2]);
}

static void
cci_tick(crowcpu_motherboard_t* mobo)
{
  uint32_t current_timeval = clock();
  uint32_t current_timeval_ms =
    div(current_timeval, CLOCKS_PER_SEC / 1000).quot;

  uint32_t connection_must_achieve =
    mobo->cci.last_connection_tick + mobo->conf.cci_con_ms;
  uint32_t update_must_achieve =
    mobo->cci.last_update_tick + mobo->conf.cci_update_ms;

  if (current_timeval_ms >= connection_must_achieve) {
    int dev_idx = cci_find_open_dev(mobo);

    if (dev_idx == -1)
      goto skip_connection_handler;

    cci_dev* dev = &mobo->cci.devices[dev_idx];

    int nfd;
    if ((nfd = accept(mobo->cci.listening_socket, NULL, NULL)) < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        goto skip_connection_handler;

      fprintf(stderr, "accept returned an error in cci handler\n");
    }

    dev->socket = nfd;
    dev->this_pollfd->fd = dev->socket;

    struct timespec recv_timeout;
    recv_timeout.tv_sec = 0;
    recv_timeout.tv_nsec = 1000000;

    setsockopt(
      dev->socket, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout, sizeof recv_timeout);

    if (fcntl(dev->socket, F_SETFL, fcntl(dev->socket, F_GETFL) | O_NONBLOCK) <
        0)
      fprintf(stderr, "unable to set cci device socket as nonblocking\n");

    /* connection protocol */
    uint32_t accept_response = htonl(1);
    cci_msg msgs[] = {
      (cci_msg){ .bytesize = 12, .write = false, .handler = cci_accept_stage0 },
      (cci_msg){ .bytesize = sizeof accept_response,
                 .write = true,
                 .opt_payload = &accept_response }
    };

    if (!cci_interact(dev, msgs, 1))
      cci_disconnect(dev);
  }

skip_connection_handler:
  if (current_timeval_ms >= update_must_achieve) {
    /* TODO */
    if (poll(mobo->cci.pollfds, mobo->cci.devices_cap, 0) == -1)
      fprintf(stderr, "poll returned error code\n");

    for (uint32_t i = 0; i < mobo->cci.devices_cap; i++) {
      cci_dev* dev = &mobo->cci.devices[i];

      if (dev->this_pollfd->revents & POLLIN) {
        uint32_t buf[2];

        for (;;) {
          int resp = recv(dev->socket, buf, sizeof buf, MSG_WAITALL);

          if (resp == -1) {
            if (errno == EWOULDBLOCK || errno == EAGAIN)
              break;
            else
              fprintf(stderr, "failed to read from cci device socket\n");
          }

          buf[0] = ntohl(buf[0]);
          buf[1] = ntohl(buf[1]);

          crowcpu_mmu_set_word(, , )
        }
      }

      if (dev->this_pollfd->revents)
    }
  }
}
