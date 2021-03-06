// Copyright (c) 2017 Daniel L. Robertson
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#define MAX_EVENTS 10

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <time.h>
#include <sys/timerfd.h>
#include <sys/types.h>

#include <unistd.h>

#include <cmds.h>
#include <errors.h>
#include <sender.h>
#include <sender_map.h>

struct rx_event {
  struct packet_command *cmd;
  int fd;
};

GENERIC_LIST(struct rx_event *, rx_events);

// Create a timer and register it to the epoll fd
struct rx_events_item *add_packet_timer(struct packet_command *cmd,
                                        struct rx_events *evs, int efd) {
  struct itimerspec ts;
  struct epoll_event ev;
  struct rx_event *rx_event = malloc(sizeof(struct rx_event));
  struct rx_events_item *rx_item = NULL;
  if (rx_event) {
    rx_event->cmd = cmd;
  } else {
    return NULL;
  }
  rx_event->fd = timerfd_create(1, 0);
  if (rx_event->fd < 0) {
    free(rx_event);
    return NULL;
  } else {
    rx_item = rx_events_add(evs, rx_event);
    if (!rx_item) {
      free(rx_event);
      return NULL;
    }
    if (cmd->repeat == 0) {
      // We assume a repeat value of 0 was meant to be 1.
      ts.it_interval.tv_sec = 0;
      ts.it_interval.tv_nsec = 0;
      ts.it_value.tv_sec = (cmd->timeout >= 0) ? cmd->timeout : 0;
      ts.it_value.tv_nsec = 1;
    } else if (cmd->repeat == 1) {
      // repeat is 1. Do not set an interval to fire at and decrement
      // repeat.
      ts.it_interval.tv_sec = 0;
      ts.it_interval.tv_nsec = 0;
      // We assume a timeout set when repeat is 0 means you want a
      // delay before we send the packet
      ts.it_value.tv_sec = (cmd->timeout >= 0) ? cmd->timeout : 0;
      ts.it_value.tv_nsec = 1;
      // Decrement repeat value to ensure it is only sent once
      --cmd->repeat;
    } else {
      // If repeat has a value, set it_interval.tv_sec because we want
      // this timer to fire at an interval. If the timeout value is less
      // than or equal to zero, set this to a few nanoseconds
      if (cmd->timeout <= 0) {
        ts.it_interval.tv_sec = 0;
        ts.it_interval.tv_nsec = 10000;
      } else {
        ts.it_interval.tv_sec = cmd->timeout;
        ts.it_interval.tv_nsec = 0;
      }
      // Set tv_nsec to 1 so that it is immediately sent
      ts.it_value.tv_sec = 0;
      ts.it_value.tv_nsec = 1;
      if (cmd->repeat > 0) {
        // repeat is not infinate. Decrement it for the first time.
        --cmd->repeat;
      }
    }
    if (timerfd_settime(rx_event->fd, 0, &ts, NULL) < 0) {
      free(rx_event);
      return NULL;
    }

    // Register the timerfd to the epoll fd
    ev.events = EPOLLIN;
    ev.data.ptr = rx_item;
    if (epoll_ctl(efd, EPOLL_CTL_ADD, rx_event->fd, &ev) < 0) {
      free(rx_event);
      return NULL;
    }
  }
  return rx_item;
}

void rx_events_free_all(struct rx_events *evs) {
  struct rx_events_item *rx_item = NULL;
  for (rx_item = evs->head; rx_item; rx_item = rx_item->next) {
    if (rx_item->value) {
      close(rx_item->value->fd);
      free(rx_item->value);
    }
  }
  rx_events_free(evs);
}

int tx_main(const struct pkt_cmd_list *lst, struct sylkie_sender_map *map,
            int ipc) {
  int ret, i, nfds, exiting = 0, closing = 0;
  int efd = epoll_create1(0);
  enum sylkie_error err = SYLKIE_SUCCESS;
  struct epoll_event ev, events[MAX_EVENTS];
  uint64_t res = 0;
  struct pkt_cmd_list_item *cmd_item = NULL;
  struct rx_events_item *rx_item = NULL;
  struct rx_event *rx_event = NULL;
  struct rx_events *evs = rx_events_init();

  if (!evs) {
    return -1;
  }

  for (cmd_item = lst->head; cmd_item; cmd_item = cmd_item->next) {
    rx_item = add_packet_timer(cmd_item->value, evs, efd);
    if (!rx_item) {
      close(efd);
      rx_events_free_all(evs);
      return -1;
    }
  }

  // Change this to while (commands still exist)
  while (evs->head && !exiting) {
    nfds = epoll_wait(efd, events, MAX_EVENTS, -1);
    if (nfds < 0) {
      // TODO(dlrobertson): cleanup
      exiting = 1;
      break;
    }
    for (i = 0; i < nfds; ++i) {
      ev = events[i];
      if ((rx_item = ev.data.ptr) && (rx_event = rx_item->value)) {
        if (rx_event->cmd) {
          sylkie_sender_send_packet(rx_event->cmd->sender, rx_event->cmd->pkt,
                                    0, &err);
          if (err) {
            exiting = 1;
            break;
          }
          if (rx_event->cmd->repeat > 0) {
            --rx_event->cmd->repeat;
          } else if (rx_event->cmd->repeat == 0) {
            // We cannot count on rx_item being alive past this point
            rx_events_rm(evs, rx_item);
            if (epoll_ctl(efd, EPOLL_CTL_DEL, rx_event->fd, NULL) < 0) {
              exiting = 1;
              break;
            }
            closing = 1;
          }
        }
        ret = read(rx_event->fd, &res, sizeof(res));
        if (ret < 0) {
          exiting = 1;
          break;
        }
        if (closing) {
          close(rx_event->fd);
          free(rx_event);
          closing = 0;
        }
      } else {
        exiting = 1;
        break;
      }
    }
  }

  close(efd);
  rx_events_free_all(evs);

  return exiting;
}
