/*
 * Copyright (C) 2019 Tianyuan Yu
 *
 * This file is subject to the terms and conditions of the GNU Lesser
 * General Public License v2.1. See the file LICENSE in the top level
 * directory for more details.
 *
 * See AUTHORS.md for complete list of NDN IOT PKG authors and contributors.
 */

#ifndef NETFACE_H
#define NETFACE_H

#include <ndn-lite/forwarder/face.h>
#include <ndn-lite/encode/fragmentation-support.h>

#include "iolist.h"
#include "net/ethernet.h"

#include <kernel_types.h>
#include <thread.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief   Length of the temporary copying buffer for receival.
 * @note    It should be as long as the maximum packet length of all the netdev you use.
 */
#ifndef NETFACE_NETDEV_BUFLEN
#define NETFACE_NETDEV_BUFLEN      (ETHERNET_MAX_LEN)
#endif

/**
 * Link layer face.
 */
typedef struct ndn_netface {
    /**
     * The inherited interface.
     */
    ndn_face_intf_t intf;
    /**
     * Link layer MTU.
     */
    uint16_t mtu;
    /**
     * Re-assembly buffer.
     */
    uint8_t frag_buffer[500];
    /**
     * Assembler help the re-assembly.
     */
    ndn_frag_assembler_t assembler;
    /**
     * Corresponding link layer PID.
     */
    kernel_pid_t pid;
} ndn_netface_t;

typedef struct ethernet_next {
    struct ethernet_next *next;

    const void *data;                     

    size_t size;
} ethernet_next_t;

/*
 * Initializes the netif table and try to add existing
 * network devices into the netif and face tables.
 * @return the initialized netfaces.
 */
int ndn_netface_auto_construct(void);

#ifdef __cplusplus
}
#endif

ndn_netface_t *ndn_netface_get_list(void);

#endif /* NETFACE_H */
/** @} */
