/*
 * Copyright (C) 2019 Tianyuan Yu
 *
 * This file is subject to the terms and conditions of the GNU Lesser
 * General Public License v2.1. See the file LICENSE in the top level
 * directory for more details.
 *
 * See AUTHORS.md for complete list of NDN IOT PKG authors and contributors.
 */

#include "netface/netface.h"
#include "l2/l2.h"

#include "netdev_tap_params.h"
#include "thread.h"

#include <ndn-lite/encode/fragmentation-support.h>
#include <ndn-lite/forwarder/forwarder.h>
#define ENABLE_NDN_LOG_ERROR 1
#define ENABLE_NDN_LOG_DEBUG 1
#define ENABLE_NDN_LOG_INFO 1
#include <ndn-lite/util/logger.h>
#include <ndn-lite/ndn-constants.h>

#include <net/ethertype.h>
#include <net/netopt.h>
#include <kernel_types.h>
#include <thread.h>
#include <timex.h>

#include <inttypes.h>

#define MAX_NET_QUEUE_SIZE 8
/**
 * @brief Max number of network interfaces
 */
#ifndef MAX_NETIFS
#define MAX_NETIFS (1)
#endif

#ifndef NETFACE_NETDEV_BUFLEN
#define NETFACE_NETDEV_BUFLEN      (ETHERNET_MAX_LEN)
#endif

#define NETFACE_NETDEV_STACKSIZE        (THREAD_STACKSIZE_DEFAULT)
#define NETFACE_NETDEV_PRIO             (THREAD_PRIORITY_MAIN - 1)
#define NETFACE_NETDEV_QUEUE_LEN        (8)
#define NETFACE_NETDEV_MSG_TYPE_EVENT   0x1236

static netdev_tap_t netdev_tap[NETDEV_TAP_MAX];
static kernel_pid_t _pid = KERNEL_PID_UNDEF;
static char _stack[NETFACE_NETDEV_STACKSIZE];
static msg_t _queue[NETFACE_NETDEV_QUEUE_LEN];
static uint8_t _recv_buf[NETFACE_NETDEV_BUFLEN];

static ndn_netface_t _netface_table[MAX_NETIFS];

static void _event_cb(netdev_t *dev, netdev_event_t event);
static void *_event_loop(void *arg);

int ndn_netface_up(struct ndn_face_intf *self)
{
    self->state = NDN_FACE_STATE_UP;
    return 0;
}

void ndn_netface_destroy(struct ndn_face_intf *self)
{
    self->state = NDN_FACE_STATE_DESTROYED;
    return;
}

int ndn_netface_down(struct ndn_face_intf *self)
{
    self->state = NDN_FACE_STATE_DOWN;
    return 0;
}

int ndn_netface_send(struct ndn_face_intf *self, const uint8_t *packet,
                     uint32_t size)
{
    ndn_netface_t *phyface = (ndn_netface_t *)self;

    if (phyface == NULL) {
        NDN_LOG_ERROR(
            "no such physical netface, forwarder face_id  = %d\n",
            self->face_id);
    }

    /* check mtu */
    if (size > phyface->mtu) {
        NDN_LOG_DEBUG("the packet will be fragmented\n");
        return ndn_l2_send_fragments(&netdev_tap[0].netdev, netdev_tap[0].addr,
                                     packet, size, phyface->mtu);
    }

    int len;

    len = ndn_l2_send_packet(&netdev_tap[0].netdev, netdev_tap[0].addr, packet,
                             size);

    return len;
}

void ndn_netface_receive(void *self, size_t param_length, void *param)
{
    int len = netdev_tap[0].netdev.driver->recv(&netdev_tap[0].netdev,
                                                _recv_buf, sizeof(_recv_buf),
                                                NULL);

    if (len == -1) {
        ndn_msgqueue_post(self, ndn_netface_receive, param_length, param);
        return;
    }
    assert(((unsigned)len) <= UINT16_MAX);

    NDN_LOG_DEBUG("message received\n");
    ndn_l2_process_packet(self, _recv_buf, sizeof(_recv_buf));

    ndn_msgqueue_post(self, ndn_netface_receive, param_length, param);
}

int ndn_netface_auto_construct(void)
{
    int res = 0;

    for (unsigned i = 0; i < NETDEV_TAP_MAX; i++) {
        const netdev_tap_params_t *p = &netdev_tap_params[i];

        NDN_LOG_DEBUG(
            "[auto_init_netif] initializing netdev_tap #%u on TAP %s\n",
            i, *(p->tap_name));

        netdev_tap_setup(&netdev_tap[i], p);
        /* start multiplexing thread (only one needed) */
        if (_pid <= KERNEL_PID_UNDEF) {
            _pid = thread_create(_stack, NETFACE_NETDEV_STACKSIZE,
                                 NETFACE_NETDEV_PRIO,
                                 THREAD_CREATE_STACKTEST, _event_loop, NULL,
                                 "netface_netdev_thread");

            if (_pid <= 0) {
                NDN_LOG_DEBUG("Failed to create thread\n");
            }

        }
        netdev_tap[i].netdev.driver->init(&netdev_tap[i].netdev);
        netdev_tap[i].netdev.event_callback = _event_cb;
        _netface_table[i].mtu = 1500;

        /* setting up forwarder face*/
        _netface_table[i].intf.state = NDN_FACE_STATE_DOWN;
        _netface_table[i].intf.face_id = NDN_INVALID_ID;
        _netface_table[i].intf.type = NDN_FACE_TYPE_NET;
        _netface_table[i].intf.up = ndn_netface_up;
        _netface_table[i].intf.send = ndn_netface_send;
        _netface_table[i].intf.down = ndn_netface_down;
        _netface_table[i].intf.destroy = ndn_netface_destroy;
        _netface_table[i].pid = _pid;

        /* registering netface to forwarder */
        ndn_forwarder_register_face(&_netface_table[i].intf);
        ndn_face_up(&_netface_table[i].intf);
        ndn_frag_assembler_init(&_netface_table[i].assembler,
                                _netface_table[i].frag_buffer,
                                sizeof(_netface_table[i].frag_buffer));

        /* posting a msgqueue netface receiving event */
        ndn_msgqueue_post(&_netface_table[i].intf, ndn_netface_receive,
                          0, NULL);
    }

    if (res < 0) {
        return res;
    }

    return res;
}

static void _event_cb(netdev_t *dev, netdev_event_t event)
{
    if (event == NETDEV_EVENT_ISR) {
        assert(_pid != KERNEL_PID_UNDEF);
        msg_t msg;

        msg.type = NETFACE_NETDEV_MSG_TYPE_EVENT;
        msg.content.ptr = dev;

        if (msg_send(&msg, _pid) <= 0) {
            NDN_LOG_DEBUG("netface_netdev: possibly lost interrupt.\n");
        }
    }
    else {
        switch (event) {
        case NETDEV_EVENT_RX_COMPLETE: {
            int len =
                dev->driver->recv(dev, _recv_buf, sizeof(_recv_buf), NULL);
            if (len < 0) {
                NDN_LOG_DEBUG("netface_netdev: error receiving packet\n");
                return;
            }

            size_t data_size = len - sizeof(ethernet_hdr_t);

            ethernet_hdr_t header;
            uint8_t *packet = malloc(data_size);

            memcpy(packet, _recv_buf + sizeof(ethernet_hdr_t), data_size);
            memcpy(&header, _recv_buf, sizeof(ethernet_hdr_t));

            if (header.type.u16 == ETHERTYPE_NDN) {
                ndn_l2_process_packet(&_netface_table[0].intf, packet,
                                      data_size);
            }
            free(packet);

            break;
        }
        default:
            NDN_LOG_DEBUG("netface_netdev: a different event occured: %d\n",
                          event);
            break;
        }
    }
}

static void *_event_loop(void *arg)
{
    NDN_LOG_DEBUG("THREAD start event loop\n");
    (void)arg;
    msg_init_queue(_queue, NETFACE_NETDEV_QUEUE_LEN);
    while (1) {
        msg_t msg;
        msg_receive(&msg);
        if (msg.type == NETFACE_NETDEV_MSG_TYPE_EVENT) {
            netdev_t *dev = msg.content.ptr;
            dev->driver->isr(dev);
        }
    }
    return NULL;
}

ndn_netface_t *ndn_netface_get_list(void)
{
    return _netface_table;
}

/** @} */
