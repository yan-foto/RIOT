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

#include <ndn-lite/encode/fragmentation-support.h>
#define ENABLE_NDN_LOG_ERROR 1
#define ENABLE_NDN_LOG_DEBUG 1
#define ENABLE_NDN_LOG_INFO 1
#include <ndn-lite/util/logger.h>
#include <ndn-lite/forwarder/forwarder.h>
#include <ndn-lite/ndn-constants.h>
#include <ndn-lite/security/ndn-lite-rng.h>

#include <net/netdev.h>
#include <utlist.h>
#include <kernel_types.h>
#include <thread.h>
#include <timex.h>

#include <stdio.h>
#include <inttypes.h>

#define MAX_NET_QUEUE_SIZE 16

int ndn_l2_send_packet(netdev_t *netdev, uint8_t *src_addr, const uint8_t *packet, uint32_t size)
{

    ethernet_hdr_t hdr;
    hdr.dst[0] = 0xff;
    hdr.dst[1] = 0xff;
    hdr.dst[2] = 0xff;
    hdr.dst[3] = 0xff;
    hdr.dst[4] = 0xff;
    hdr.dst[5] = 0xff;

    ethernet_next_t hdr_pkt;
    hdr_pkt.data = &hdr;
    hdr_pkt.size = sizeof(hdr);
    NDN_LOG_DEBUG("send: hdr size: %d\n", hdr_pkt.size);

    memcpy(hdr.src, src_addr, ETHERNET_ADDR_LEN);

    ethernet_next_t* pkt = malloc(sizeof(ethernet_next_t));

    pkt->data = packet;
    pkt->size = size;
    NDN_LOG_DEBUG("send: pkt size: %d\n", pkt->size);

    LL_PREPEND(pkt, &hdr_pkt);

    return netdev->driver->send(netdev, (iolist_t*) pkt);
}

int ndn_l2_send_fragments(netdev_t *netdev, uint8_t *src_addr,
                                const uint8_t *packet, uint32_t packet_size, uint16_t mtu)
{
    if (mtu <= NDN_FRAG_HDR_LEN) {
        NDN_LOG_ERROR(
            "MTU smaller than L2 fragmentation header size\n");
        return -1;
    }

    int total_frags = packet_size / (mtu - NDN_FRAG_HDR_LEN) + 1;
    if (total_frags > 32) {
        NDN_LOG_ERROR(
            "ndn: too many fragments to send\n");
        return -1;
    }

    ndn_fragmenter_t fragmenter;
    uint8_t fragmented[mtu];

    uint16_t identifier = 0;
    ndn_rng((uint8_t *)&identifier, sizeof(identifier));
    ndn_fragmenter_init(&fragmenter, packet, packet_size, mtu, identifier);

    while (fragmenter.counter < fragmenter.total_frag_num) {
        uint32_t size =
            (fragmenter.counter == fragmenter.total_frag_num - 1) ?
            (fragmenter.original_size - fragmenter.offset +
             3) :
            mtu;
        ndn_fragmenter_fragment(&fragmenter, fragmented);

        if (ndn_l2_send_packet(netdev, src_addr, fragmented, size) < 0) {
            NDN_LOG_ERROR("fragment: error sending packet\n");
            return -1;
        }

        NDN_LOG_DEBUG("sent fragment (SEQ=%d, ID=%02X, "
                      "size=%d\n",
                      (int)fragmenter.counter,
                      fragmenter.frag_identifier, (int)size);
    }
    NDN_LOG_DEBUG("forwarder sending: %llu ms\n", ndn_time_now_ms());
    return 0;
}

int ndn_l2_process_packet(ndn_face_intf_t *self, uint8_t *data, size_t length)
{
    NDN_LOG_DEBUG("forwarder receiving: %llu ms\n",
                      ndn_time_now_ms());

    int ret = ndn_forwarder_receive(self, data, length);

    NDN_LOG_DEBUG("ndn_l2_process_packet: return value from forwarder_receive: %d\n", ret);

    return 0;
}
/** @} */
