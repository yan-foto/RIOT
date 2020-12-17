/*
 * Copyright (C) 2019 Tianyuan Yu
 *
 * This file is subject to the terms and conditions of the GNU Lesser
 * General Public License v2.1. See the file LICENSE in the top level
 * directory for more details.
 *
 * See AUTHORS.md for complete list of NDN IOT PKG authors and contributors.
 */

#ifndef NDN_L2_H_
#define NDN_L2_H_

#include <ndn-lite/forwarder/face.h>
#include <ndn-lite/encode/fragmentation-support.h>

#include <net/netopt.h>
#include <net/gnrc/netapi.h>
#include <net/gnrc/netif.h>
#include <net/gnrc/netif/hdr.h>
#include <net/gnrc/netreg.h>
#include <kernel_types.h>
#include <thread.h>
#include <timex.h>

#ifdef __cplusplus
extern "C" {
#endif

int ndn_l2_send_packet(netdev_t *netdev, uint8_t *src_addr, const uint8_t *packet, uint32_t size);

int ndn_l2_send_fragments(netdev_t *netdev, uint8_t *src_addr,
                                 const uint8_t *packet, uint32_t size, uint16_t mtu);
int ndn_l2_process_packet(ndn_face_intf_t *self, uint8_t *data, size_t length);

#ifdef __cplusplus
}
#endif

#endif /* NDN_NETIF_H_ */
/** @} */
