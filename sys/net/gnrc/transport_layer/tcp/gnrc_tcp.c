/*
 * Copyright (C) 2015-2017 Simon Brummer
 *
 * This file is subject to the terms and conditions of the GNU Lesser
 * General Public License v2.1. See the file LICENSE in the top level
 * directory for more details.
 */

/**
 * @ingroup     net_gnrc
 * @{
 *
 * @file
 * @brief       GNRC TCP API implementation
 *
 * @author      Simon Brummer <simon.brummer@posteo.de>
 * @}
 */

#include <assert.h>
#include <errno.h>
#include <string.h>
#include <utlist.h>

#include "evtimer.h"
#include "evtimer_mbox.h"
#include "mbox.h"
#include "net/af.h"
#include "net/tcp.h"
#include "net/gnrc.h"
#include "net/gnrc/tcp.h"
#include "include/gnrc_tcp_common.h"
#include "include/gnrc_tcp_fsm.h"
#include "include/gnrc_tcp_pkt.h"
#include "include/gnrc_tcp_eventloop.h"
#include "include/gnrc_tcp_rcvbuf.h"

#ifdef MODULE_GNRC_IPV6
#include "net/gnrc/ipv6.h"
#endif

#define ENABLE_DEBUG 0
#include "debug.h"

#define TCP_MSG_QUEUE_SIZE (1 << CONFIG_GNRC_TCP_MSG_QUEUE_SIZE_EXP)


/**
 * @brief Central MBOX evtimer used by gnrc_tcp
 */
static evtimer_t _tcp_mbox_timer;

static void _sched_mbox(evtimer_mbox_event_t *event, uint32_t offset,
                        uint16_t type, mbox_t *mbox)
{
    TCP_DEBUG_ENTER;
    event->event.offset = offset;
    event->msg.type = type;
    evtimer_add_mbox(&_tcp_mbox_timer, event, mbox);
    TCP_DEBUG_LEAVE;
}

static void _sched_connection_timeout(evtimer_mbox_event_t *event, mbox_t *mbox)
{
    TCP_DEBUG_ENTER;
    _sched_mbox(event, CONFIG_GNRC_TCP_CONNECTION_TIMEOUT_DURATION_MS,
                MSG_TYPE_CONNECTION_TIMEOUT, mbox);
    TCP_DEBUG_LEAVE;
}

static void _unsched_mbox(evtimer_mbox_event_t *event)
{
    TCP_DEBUG_ENTER;
    evtimer_del(&_tcp_mbox_timer, (evtimer_event_t *)event);
    TCP_DEBUG_LEAVE;
}

/**
 * @brief   Establishes a new TCP connection
 *
 * @param[in,out] tcb           TCB holding the connection information.
 * @param[in]     target_addr   Target address to connect to, if this is a active connection.
 * @param[in]     target_port   Target port to connect to, if this is a active connection.
 * @param[in]     local_addr    Local address to bind on, if this is a passive connection.
 * @param[in]     local_port    Local port to bind on, if this is a passive connection.
 * @param[in]     passive       Flag to indicate if this is a active or passive open.
 *
 * @returns   Zero on success.
 *            -EISCONN if TCB is already connected.
 *            -ENOMEM if the receive buffer for the TCB could not be allocated.
 *            -EADDRINUSE if @p local_port is already in use.
 *            -ETIMEDOUT if the connection opening timed out.
 *            -ECONNREFUSED if the connection was reset by the peer.
 */
static int _gnrc_tcp_open(gnrc_tcp_tcb_t *tcb, const gnrc_tcp_ep_t *remote,
                          const uint8_t *local_addr, uint16_t local_port, int passive)
{
    TCP_DEBUG_ENTER;
    msg_t msg;
    msg_t msg_queue[TCP_MSG_QUEUE_SIZE];
    mbox_t mbox = MBOX_INIT(msg_queue, TCP_MSG_QUEUE_SIZE);
    int ret = 0;

    /* Lock the TCB for this function call */
    mutex_lock(&(tcb->function_lock));

    /* TCB is already connected: Return -EISCONN */
    if (tcb->state != FSM_STATE_CLOSED) {
        mutex_unlock(&(tcb->function_lock));
        TCP_DEBUG_ERROR("-EISCONN: TCB already connected.");
        TCP_DEBUG_LEAVE;
        return -EISCONN;
    }

    /* Setup messaging */
    _gnrc_tcp_fsm_set_mbox(tcb, &mbox);

    /* Setup passive connection */
    if (passive) {
        /* Mark connection as passive opend */
        tcb->status |= STATUS_PASSIVE;
#ifdef MODULE_GNRC_IPV6
        /* If local address is specified: Copy it into TCB */
        if (local_addr && tcb->address_family == AF_INET6) {
            /* Store given address in TCB */
            if (memcpy(tcb->local_addr, local_addr, sizeof(tcb->local_addr)) == NULL) {
                TCP_DEBUG_ERROR("-EINVAL: Invalid peer address.");
                TCP_DEBUG_LEAVE;
                return -EINVAL;
            }

            if (ipv6_addr_is_unspecified((ipv6_addr_t *) tcb->local_addr)) {
                tcb->status |= STATUS_ALLOW_ANY_ADDR;
            }
        }
#else
        /* Suppress Compiler Warnings */
        (void) remote;
        (void) local_addr;
#endif
        /* Set port number to listen on */
        tcb->local_port = local_port;
    }
    /* Setup active connection */
    else {
        assert(remote != NULL);

        /* Parse target address and port number into TCB */
 #ifdef MODULE_GNRC_IPV6
        if (tcb->address_family == AF_INET6) {

            /* Store Address information in TCB */
            if (memcpy(tcb->peer_addr, remote->addr.ipv6, sizeof(tcb->peer_addr)) == NULL) {
                TCP_DEBUG_ERROR("-EINVAL: Invalid peer address.");
                TCP_DEBUG_LEAVE;
                return -EINVAL;
            }
            tcb->ll_iface = remote->netif;
        }
 #endif

        /* Assign port numbers, verification happens in fsm */
        tcb->local_port = local_port;
        tcb->peer_port = remote->port;

        /* Setup connection timeout */
        _sched_connection_timeout(&tcb->event_misc, &mbox);
    }

    /* Call FSM with event: CALL_OPEN */
    ret = _gnrc_tcp_fsm(tcb, FSM_EVENT_CALL_OPEN, NULL, NULL, 0);
    if (ret == -ENOMEM) {
        TCP_DEBUG_ERROR("-ENOMEM: All receive buffers are in use.");
    }
    else if (ret == -EADDRINUSE) {
        TCP_DEBUG_ERROR("-EADDRINUSE: local_port is already in use.");
    }

    /* Wait until a connection was established or closed */
    while (ret >= 0 && tcb->state != FSM_STATE_CLOSED && tcb->state != FSM_STATE_ESTABLISHED &&
           tcb->state != FSM_STATE_CLOSE_WAIT) {
        mbox_get(&mbox, &msg);
        switch (msg.type) {
            case MSG_TYPE_NOTIFY_USER:
                TCP_DEBUG_INFO("Received MSG_TYPE_NOTIFY_USER.");

                /* Setup a timeout to be able to revert back to LISTEN state, in case the
                 * send SYN+ACK we received upon entering SYN_RCVD is never acknowledged
                 * by the peer. */
                if ((tcb->state == FSM_STATE_SYN_RCVD) && (tcb->status & STATUS_PASSIVE)) {
                    _unsched_mbox(&tcb->event_misc);
                    _sched_connection_timeout(&tcb->event_misc, &mbox);
                }
                break;

            case MSG_TYPE_CONNECTION_TIMEOUT:
                TCP_DEBUG_INFO("Received MSG_TYPE_CONNECTION_TIMEOUT.");

                /* The connection establishment attempt timed out:
                 * 1) Active connections return -ETIMEOUT.
                 * 2) Passive connections stop the ongoing retransmissions and repeat the
                 *    open call to wait for the next connection attempt. */
                if (tcb->status & STATUS_PASSIVE) {
                    _gnrc_tcp_fsm(tcb, FSM_EVENT_CLEAR_RETRANSMIT, NULL, NULL, 0);
                    _gnrc_tcp_fsm(tcb, FSM_EVENT_CALL_OPEN, NULL, NULL, 0);
                }
                else {
                    _gnrc_tcp_fsm(tcb, FSM_EVENT_TIMEOUT_CONNECTION, NULL, NULL, 0);
                    TCP_DEBUG_ERROR("-ETIMEDOUT: Connection timed out.");
                    ret = -ETIMEDOUT;
                }
                break;

            default:
                TCP_DEBUG_ERROR("Received unexpected message.");
        }
    }

    /* Cleanup */
    _gnrc_tcp_fsm_set_mbox(tcb, NULL);
    _unsched_mbox(&tcb->event_misc);
    if (tcb->state == FSM_STATE_CLOSED && ret == 0) {
        TCP_DEBUG_ERROR("-ECONNREFUSED: Connection refused by peer.");
        ret = -ECONNREFUSED;
    }
    mutex_unlock(&(tcb->function_lock));
    TCP_DEBUG_LEAVE;
    return ret;
}

/* External GNRC TCP API */
int gnrc_tcp_ep_init(gnrc_tcp_ep_t *ep, int family, const uint8_t *addr, size_t addr_size,
                     uint16_t port, uint16_t netif)
{
    TCP_DEBUG_ENTER;
#ifdef MODULE_GNRC_IPV6
    if (family != AF_INET6) {
        TCP_DEBUG_ERROR("-EAFNOSUPPORT: Parameter family is not AF_INET6.")
        TCP_DEBUG_LEAVE;
        return -EAFNOSUPPORT;
    }

    if (addr == NULL && addr_size == 0) {
        ipv6_addr_set_unspecified((ipv6_addr_t *) ep->addr.ipv6);
    }
    else if (addr_size == sizeof(ipv6_addr_t)) {
        memcpy(ep->addr.ipv6, addr, sizeof(ipv6_addr_t));
    }
    else {
        TCP_DEBUG_ERROR("-EINVAL: Parameter addr is invalid.")
        TCP_DEBUG_LEAVE;
        return -EINVAL;
    }
#else
    /* Suppress Compiler Warnings */
    (void) addr;
    (void) addr_size;
    TCP_DEBUG_ERROR("-EAFNOSUPPORT: No network layer configured.")
    TCP_DEBUG_LEAVE;
    return -EAFNOSUPPORT;
#endif

    ep->family = family;
    ep->port = port;
    ep->netif = netif;
    TCP_DEBUG_LEAVE;
    return 0;
}

int gnrc_tcp_ep_from_str(gnrc_tcp_ep_t *ep, const char *str)
{
    TCP_DEBUG_ENTER;
    assert(str);

    unsigned port = 0;
    unsigned netif = 0;

    /* Examine given string */
    char *addr_begin = strchr(str, '[');
    char *addr_end = strchr(str, ']');

    /* 1) Ensure that str contains a single pair of brackets */
    if (!addr_begin || !addr_end || strchr(addr_begin + 1, '[') || strchr(addr_end + 1, ']')) {
        TCP_DEBUG_ERROR("-EINVAL: Invalid address string.");
        TCP_DEBUG_LEAVE;
        return -EINVAL;
    }
    /* 2) Ensure that the first character is the opening bracket */
    else if (addr_begin != str) {
        TCP_DEBUG_ERROR("-EINVAL: Invalid address string.");
        TCP_DEBUG_LEAVE;
        return -EINVAL;
    }

    /* 3) Examine optional port number */
    char *port_begin = strchr(addr_end, ':');
    if (port_begin) {
        /* 3.1) Ensure that there are characters left to parse after ':'. */
        if (*(++port_begin) == '\0') {
            TCP_DEBUG_ERROR("-EINVAL: Invalid address string.");
            TCP_DEBUG_LEAVE;
            return -EINVAL;
        }

        /* 3.2) Ensure that port is a number (atol, does not report errors) */
        for (char *ptr = port_begin; *ptr; ++ptr) {
            if ((*ptr < '0') || ('9' < *ptr)) {
                TCP_DEBUG_ERROR("-EINVAL: Invalid address string.");
                TCP_DEBUG_LEAVE;
                return -EINVAL;
            }
        }

        /* 3.3) Read and verify that given number port is within range */
        port = atol(port_begin);
        if (port > 0xFFFF) {
            TCP_DEBUG_ERROR("-EINVAL: Invalid address string.");
            TCP_DEBUG_LEAVE;
            return -EINVAL;
        }
    }

    /* 4) Examine optional interface identifier. */
    char *if_begin = strchr(str, '%');
    if (if_begin) {
        /* 4.1) Ensure that the identifier is not empty and within brackets. */
        if (addr_end <= (++if_begin)) {
            TCP_DEBUG_ERROR("-EINVAL: Invalid address string.");
            TCP_DEBUG_LEAVE;
            return -EINVAL;
        }

        /* 4.2) Ensure that the identifier is a number (atol, does not report errors) */
        for (char *ptr = if_begin; ptr != addr_end; ++ptr) {
            if ((*ptr < '0') || ('9' < *ptr)) {
                TCP_DEBUG_ERROR("-EINVAL: Invalid address string.");
                TCP_DEBUG_LEAVE;
                return -EINVAL;
            }
        }

        /* 4.3) Read and replace addr_end with if_begin. */
        netif = atol(if_begin);
        addr_end = if_begin - 1;
    }

#ifdef MODULE_GNRC_IPV6
    /* 5) Try to parse IP Address. Construct Endpoint on after success. */
    char tmp[IPV6_ADDR_MAX_STR_LEN];

    /* 5.1) Verify address length and copy address into temporary buffer.
     *      This is required to preserve constness of input.
     */
    int len = addr_end - (++addr_begin);

    if (0 <= len && len < (int) sizeof(tmp)) {
        memcpy(tmp, addr_begin, len);
        tmp[len] = '\0';
    }
    else {
        TCP_DEBUG_ERROR("-EINVAL: Invalid address string.");
        TCP_DEBUG_LEAVE;
        return -EINVAL;
    }

    /* 5.2) Try to read address into endpoint. */
    if (ipv6_addr_from_str((ipv6_addr_t *) ep->addr.ipv6, tmp) == NULL) {
        TCP_DEBUG_ERROR("-EINVAL: Invalid address string.");
        TCP_DEBUG_LEAVE;
        return -EINVAL;
    }
    ep->family = AF_INET6;
#else
    /* Suppress Compiler Warnings */
    (void) port;
    (void) netif;
    TCP_DEBUG_ERROR("-EINVAL: Invalid address string.");
    TCP_DEBUG_LEAVE;
    return -EINVAL;
#endif

    ep->port = (uint16_t) port;
    ep->netif = (uint16_t) netif;
    TCP_DEBUG_LEAVE;
    return 0;
}

int gnrc_tcp_init(void)
{
    TCP_DEBUG_ENTER;
    /* Initialize receive buffers */
    _gnrc_tcp_rcvbuf_init();

    /* Initialize timers */
    evtimer_init_mbox(&_tcp_mbox_timer);

    /* Start TCP processing thread */
    kernel_pid_t pid = _gnrc_tcp_eventloop_init();
    TCP_DEBUG_LEAVE;
    return pid;
}

void gnrc_tcp_tcb_init(gnrc_tcp_tcb_t *tcb)
{
    TCP_DEBUG_ENTER;
    memset(tcb, 0, sizeof(gnrc_tcp_tcb_t));
#ifdef MODULE_GNRC_IPV6
    tcb->address_family = AF_INET6;
#else
    TCP_DEBUG_ERROR("Missing network layer. Add module to makefile.");
#endif
    tcb->rtt_var = RTO_UNINITIALIZED;
    tcb->srtt = RTO_UNINITIALIZED;
    tcb->rto = RTO_UNINITIALIZED;
    mutex_init(&(tcb->fsm_lock));
    mutex_init(&(tcb->function_lock));
    TCP_DEBUG_LEAVE;
}

int gnrc_tcp_open_active(gnrc_tcp_tcb_t *tcb, const gnrc_tcp_ep_t *remote, uint16_t local_port)
{
    TCP_DEBUG_ENTER;
    assert(tcb != NULL);
    assert(remote != NULL);
    assert(remote->port != PORT_UNSPEC);

    /* Check if given AF-Family in remote is supported */
#ifdef MODULE_GNRC_IPV6
    if (remote->family != AF_INET6) {
        TCP_DEBUG_ERROR("-EAFNOSUPPORT: remote AF-Family not supported.");
        TCP_DEBUG_LEAVE;
        return -EAFNOSUPPORT;
    }
#else
    TCP_DEBUG_ERROR("-EAFNOSUPPORT: AF-Family not supported.");
    TCP_DEBUG_LEAVE;
    return -EAFNOSUPPORT;
#endif

    /* Check if AF-Family for target address matches internally used AF-Family */
    if (remote->family != tcb->address_family) {
        TCP_DEBUG_ERROR("-EINVAL: local and remote AF-Family don't match.");
        TCP_DEBUG_LEAVE;
        return -EINVAL;
    }

    /* Proceed with connection opening */
    int res = _gnrc_tcp_open(tcb, remote, NULL, local_port, 0);
    TCP_DEBUG_LEAVE;
    return res;
}

int gnrc_tcp_open_passive(gnrc_tcp_tcb_t *tcb, const gnrc_tcp_ep_t *local)
{
    TCP_DEBUG_ENTER;
    assert(tcb != NULL);
    assert(local != NULL);
    assert(local->port != PORT_UNSPEC);

    /* Check if given AF-Family in local is supported */
#ifdef MODULE_GNRC_IPV6
    if (local->family != AF_INET6) {
        TCP_DEBUG_ERROR("-EAFNOSUPPORT: AF-Family not supported.");
        TCP_DEBUG_LEAVE;
        return -EAFNOSUPPORT;
    }

    /* Check if AF-Family matches internally used AF-Family */
    if (local->family != tcb->address_family) {
        TCP_DEBUG_ERROR("-EINVAL: AF-Family doesn't match.");
        TCP_DEBUG_LEAVE;
        return -EINVAL;
    }

    /* Proceed with connection opening */
    int res = _gnrc_tcp_open(tcb, NULL, local->addr.ipv6, local->port, 1);
    TCP_DEBUG_LEAVE;
    return res;
#else
    TCP_DEBUG_ERROR("-EAFNOSUPPORT: AF-Family not supported.");
    TCP_DEBUG_LEAVE;
    return -EAFNOSUPPORT;
#endif
}

ssize_t gnrc_tcp_send(gnrc_tcp_tcb_t *tcb, const void *data, const size_t len,
                      const uint32_t timeout_duration_ms)
{
    TCP_DEBUG_ENTER;
    assert(tcb != NULL);
    assert(data != NULL);

    msg_t msg;
    msg_t msg_queue[TCP_MSG_QUEUE_SIZE];
    mbox_t mbox = MBOX_INIT(msg_queue, TCP_MSG_QUEUE_SIZE);
    evtimer_mbox_event_t event_user_timeout;
    evtimer_mbox_event_t event_probe_timeout;
    uint32_t probe_timeout_duration_ms = 0;
    ssize_t ret = 0;
    bool probing_mode = false;

    /* Lock the TCB for this function call */
    mutex_lock(&(tcb->function_lock));

    /* Check if connection is in a valid state */
    if (tcb->state != FSM_STATE_ESTABLISHED && tcb->state != FSM_STATE_CLOSE_WAIT) {
        mutex_unlock(&(tcb->function_lock));
        TCP_DEBUG_ERROR("-ENOTCONN: TCB is not connected.");
        TCP_DEBUG_LEAVE;
        return -ENOTCONN;
    }

    /* Setup messaging */
    _gnrc_tcp_fsm_set_mbox(tcb, &mbox);

    /* Setup connection timeout */
    _sched_connection_timeout(&tcb->event_misc, &mbox);

    if (timeout_duration_ms > 0) {
        _sched_mbox(&event_user_timeout, timeout_duration_ms,
                    MSG_TYPE_USER_SPEC_TIMEOUT, &mbox);
    }

    /* Loop until something was sent and acked */
    while (ret == 0 || tcb->pkt_retransmit != NULL) {
        /* Check if the connections state is closed. If so, a reset was received */
        if (tcb->state == FSM_STATE_CLOSED) {
            TCP_DEBUG_ERROR("-ECONNRESET: Connection was reset by peer.");
            ret = -ECONNRESET;
            break;
        }

        /* If the send window is closed: Setup Probing */
        if (tcb->snd_wnd <= 0) {
            /* If this is the first probe: Setup probing duration */
            if (!probing_mode) {
                probing_mode = true;
                probe_timeout_duration_ms = tcb->rto;
            }
            /* Setup probe timeout */
            _unsched_mbox(&event_probe_timeout);
            _sched_mbox(&event_probe_timeout, probe_timeout_duration_ms,
                        MSG_TYPE_PROBE_TIMEOUT, &mbox);
        }

        /* Try to send data in case there nothing has been sent and we are not probing */
        if (ret == 0 && !probing_mode) {
            ret = _gnrc_tcp_fsm(tcb, FSM_EVENT_CALL_SEND, NULL, (void *) data, len);
        }

        /* Wait for responses */
        mbox_get(&mbox, &msg);
        switch (msg.type) {
            case MSG_TYPE_CONNECTION_TIMEOUT:
                TCP_DEBUG_INFO("Received MSG_TYPE_CONNECTION_TIMEOUT.");
                _gnrc_tcp_fsm(tcb, FSM_EVENT_TIMEOUT_CONNECTION, NULL, NULL, 0);
                TCP_DEBUG_ERROR("-ECONNABORTED: Connection timed out.");
                ret = -ECONNABORTED;
                break;

            case MSG_TYPE_USER_SPEC_TIMEOUT:
                TCP_DEBUG_INFO("Received MSG_TYPE_USER_SPEC_TIMEOUT.");
                _gnrc_tcp_fsm(tcb, FSM_EVENT_CLEAR_RETRANSMIT, NULL, NULL, 0);
                TCP_DEBUG_ERROR("-ETIMEDOUT: User specified timeout expired.");
                ret = -ETIMEDOUT;
                break;

            case MSG_TYPE_PROBE_TIMEOUT:
                TCP_DEBUG_INFO("Received MSG_TYPE_PROBE_TIMEOUT.");
                /* Send probe */
                _gnrc_tcp_fsm(tcb, FSM_EVENT_SEND_PROBE, NULL, NULL, 0);
                probe_timeout_duration_ms += probe_timeout_duration_ms;

                /* Boundary check for time interval between probes */
                if (probe_timeout_duration_ms < CONFIG_GNRC_TCP_PROBE_LOWER_BOUND_MS) {
                    probe_timeout_duration_ms = CONFIG_GNRC_TCP_PROBE_LOWER_BOUND_MS;
                }
                else if (probe_timeout_duration_ms > CONFIG_GNRC_TCP_PROBE_UPPER_BOUND_MS) {
                    probe_timeout_duration_ms = CONFIG_GNRC_TCP_PROBE_UPPER_BOUND_MS;
                }
                break;

            case MSG_TYPE_NOTIFY_USER:
                TCP_DEBUG_INFO("Received MSG_TYPE_NOTIFY_USER.");

                /* Connection is alive: Reset Connection Timeout */
                _unsched_mbox(&tcb->event_misc);
                _sched_connection_timeout(&tcb->event_misc, &mbox);

                /* If the window re-opened and we are probing: Stop it */
                if (tcb->snd_wnd > 0 && probing_mode) {
                    probing_mode = false;
                    _unsched_mbox(&event_probe_timeout);
                }
                break;

            default:
                TCP_DEBUG_ERROR("Received unexpected message.");
        }
    }

    /* Cleanup */
    _gnrc_tcp_fsm_set_mbox(tcb, NULL);
    _unsched_mbox(&tcb->event_misc);
    _unsched_mbox(&event_probe_timeout);
    _unsched_mbox(&event_user_timeout);
    mutex_unlock(&(tcb->function_lock));
    TCP_DEBUG_LEAVE;
    return ret;
}

ssize_t gnrc_tcp_recv(gnrc_tcp_tcb_t *tcb, void *data, const size_t max_len,
                      const uint32_t timeout_duration_ms)
{
    TCP_DEBUG_ENTER;
    assert(tcb != NULL);
    assert(data != NULL);

    msg_t msg;
    msg_t msg_queue[TCP_MSG_QUEUE_SIZE];
    mbox_t mbox = MBOX_INIT(msg_queue, TCP_MSG_QUEUE_SIZE);
    evtimer_mbox_event_t event_user_timeout;
    ssize_t ret = 0;

    /* Lock the TCB for this function call */
    mutex_lock(&(tcb->function_lock));

    /* Check if connection is in a valid state */
    if (tcb->state != FSM_STATE_ESTABLISHED && tcb->state != FSM_STATE_FIN_WAIT_1 &&
        tcb->state != FSM_STATE_FIN_WAIT_2 && tcb->state != FSM_STATE_CLOSE_WAIT) {
        mutex_unlock(&(tcb->function_lock));
        TCP_DEBUG_ERROR("-ENOTCONN: TCB is not connected.");
        TCP_DEBUG_LEAVE;
        return -ENOTCONN;
    }

    /* If FIN was received (CLOSE_WAIT), no further data can be received. */
    /* Copy received data into given buffer and return number of bytes. Can be zero. */
    if (tcb->state == FSM_STATE_CLOSE_WAIT) {
        ret = _gnrc_tcp_fsm(tcb, FSM_EVENT_CALL_RECV, NULL, data, max_len);
        mutex_unlock(&(tcb->function_lock));
        TCP_DEBUG_LEAVE;
        return ret;
    }

    /* If this call is non-blocking (timeout_duration_ms == 0): Try to read data and return */
    if (timeout_duration_ms == 0) {
        ret = _gnrc_tcp_fsm(tcb, FSM_EVENT_CALL_RECV, NULL, data, max_len);
        if (ret == 0) {
            TCP_DEBUG_ERROR("-EAGAIN: Not data available, try later again.");
            ret = -EAGAIN;
        }
        mutex_unlock(&(tcb->function_lock));
        TCP_DEBUG_LEAVE;
        return ret;
    }

    /* Setup messaging */
    _gnrc_tcp_fsm_set_mbox(tcb, &mbox);

    /* Setup connection timeout */
    _sched_connection_timeout(&tcb->event_misc, &mbox);

    if (timeout_duration_ms > 0) {
        _sched_mbox(&event_user_timeout, timeout_duration_ms,
                    MSG_TYPE_USER_SPEC_TIMEOUT, &mbox);
    }

    /* Processing loop */
    while (ret == 0) {
        /* Check if the connections state is closed. If so, a reset was received */
        if (tcb->state == FSM_STATE_CLOSED) {
            TCP_DEBUG_ERROR("-ECONNRESET: Connection was reset by peer.");
            ret = -ECONNRESET;
            break;
        }

        /* Try to read available data */
        ret = _gnrc_tcp_fsm(tcb, FSM_EVENT_CALL_RECV, NULL, data, max_len);

        /* If FIN was received (CLOSE_WAIT), no further data can be received. Leave event loop */
        if (tcb->state == FSM_STATE_CLOSE_WAIT) {
            break;
        }

        /* If there was no data: Wait for next packet or until the timeout fires */
        if (ret <= 0) {
            mbox_get(&mbox, &msg);
            switch (msg.type) {
                case MSG_TYPE_CONNECTION_TIMEOUT:
                    TCP_DEBUG_INFO("Received MSG_TYPE_CONNECTION_TIMEOUT.");
                    _gnrc_tcp_fsm(tcb, FSM_EVENT_TIMEOUT_CONNECTION, NULL, NULL, 0);
                    TCP_DEBUG_ERROR("-ECONNABORTED: Connection timed out.");
                    ret = -ECONNABORTED;
                    break;

                case MSG_TYPE_USER_SPEC_TIMEOUT:
                    TCP_DEBUG_INFO("Received MSG_TYPE_USER_SPEC_TIMEOUT.");
                    _gnrc_tcp_fsm(tcb, FSM_EVENT_CLEAR_RETRANSMIT, NULL, NULL, 0);
                    TCP_DEBUG_ERROR("-ETIMEDOUT: User specified timeout expired.");
                    ret = -ETIMEDOUT;
                    break;

                case MSG_TYPE_NOTIFY_USER:
                    TCP_DEBUG_INFO("Received MSG_TYPE_NOTIFY_USER.");
                    break;

                default:
                    TCP_DEBUG_ERROR("Received unexpected message.");
            }
        }
    }

    /* Cleanup */
    _gnrc_tcp_fsm_set_mbox(tcb, NULL);
    _unsched_mbox(&tcb->event_misc);
    _unsched_mbox(&event_user_timeout);
    mutex_unlock(&(tcb->function_lock));
    TCP_DEBUG_LEAVE;
    return ret;
}

void gnrc_tcp_close(gnrc_tcp_tcb_t *tcb)
{
    TCP_DEBUG_ENTER;
    assert(tcb != NULL);

    msg_t msg;
    msg_t msg_queue[TCP_MSG_QUEUE_SIZE];
    mbox_t mbox = MBOX_INIT(msg_queue, TCP_MSG_QUEUE_SIZE);

    /* Lock the TCB for this function call */
    mutex_lock(&(tcb->function_lock));

    /* Return if connection is closed */
    if (tcb->state == FSM_STATE_CLOSED) {
        mutex_unlock(&(tcb->function_lock));
        TCP_DEBUG_LEAVE;
        return;
    }

    /* Setup messaging */
    _gnrc_tcp_fsm_set_mbox(tcb, &mbox);

    /* Setup connection timeout */
    _sched_connection_timeout(&tcb->event_misc, &mbox);

    /* Start connection teardown sequence */
    _gnrc_tcp_fsm(tcb, FSM_EVENT_CALL_CLOSE, NULL, NULL, 0);

    /* Loop until the connection has been closed */
    while (tcb->state != FSM_STATE_CLOSED) {
        mbox_get(&mbox, &msg);
        switch (msg.type) {
            case MSG_TYPE_CONNECTION_TIMEOUT:
                TCP_DEBUG_INFO("Received MSG_TYPE_CONNECTION_TIMEOUT.");
                _gnrc_tcp_fsm(tcb, FSM_EVENT_TIMEOUT_CONNECTION, NULL, NULL, 0);
                break;

            case MSG_TYPE_NOTIFY_USER:
                TCP_DEBUG_INFO("Received MSG_TYPE_NOTIFY_USER.");
                break;

            default:
                TCP_DEBUG_ERROR("Received unexpected message.");
        }
    }

    /* Cleanup */
    _gnrc_tcp_fsm_set_mbox(tcb, NULL);
    _unsched_mbox(&tcb->event_misc);
    mutex_unlock(&(tcb->function_lock));
    TCP_DEBUG_LEAVE;
}

void gnrc_tcp_abort(gnrc_tcp_tcb_t *tcb)
{
    TCP_DEBUG_ENTER;
    assert(tcb != NULL);

    /* Lock the TCB for this function call */
    mutex_lock(&(tcb->function_lock));
    if (tcb->state != FSM_STATE_CLOSED) {
        /* Call FSM ABORT event */
        _gnrc_tcp_fsm(tcb, FSM_EVENT_CALL_ABORT, NULL, NULL, 0);
    }
    mutex_unlock(&(tcb->function_lock));
    TCP_DEBUG_LEAVE;
}

int gnrc_tcp_calc_csum(const gnrc_pktsnip_t *hdr, const gnrc_pktsnip_t *pseudo_hdr)
{
    TCP_DEBUG_ENTER;
    uint16_t csum;

    if ((hdr == NULL) || (pseudo_hdr == NULL)) {
        TCP_DEBUG_ERROR("-EFAULT: hdr or pseudo_hdr is NULL.");
        TCP_DEBUG_LEAVE;
        return -EFAULT;
    }
    if (hdr->type != GNRC_NETTYPE_TCP) {
        TCP_DEBUG_ERROR("-EBADMSG: Variable hdr is no TCP header.");
        TCP_DEBUG_LEAVE;
        return -EBADMSG;
    }

    csum = _gnrc_tcp_pkt_calc_csum(hdr, pseudo_hdr, hdr->next);
    if (csum == 0) {
        TCP_DEBUG_ERROR("-ENOENT");
        TCP_DEBUG_LEAVE;
        return -ENOENT;
    }
    ((tcp_hdr_t *)hdr->data)->checksum = byteorder_htons(csum);

    TCP_DEBUG_LEAVE;
    return 0;
}

gnrc_pktsnip_t *gnrc_tcp_hdr_build(gnrc_pktsnip_t *payload, uint16_t src, uint16_t dst)
{
    TCP_DEBUG_ENTER;
    gnrc_pktsnip_t *res;
    tcp_hdr_t *hdr;

    /* Allocate header */
    res = gnrc_pktbuf_add(payload, NULL, sizeof(tcp_hdr_t), GNRC_NETTYPE_TCP);
    if (res == NULL) {
        TCP_DEBUG_ERROR("pktbuf is full.");
        TCP_DEBUG_LEAVE;
        return NULL;
    }
    hdr = (tcp_hdr_t *) res->data;

    /* Clear Header */
    memset(hdr, 0, sizeof(tcp_hdr_t));

    /* Initialize header with sane defaults */
    hdr->src_port = byteorder_htons(src);
    hdr->dst_port = byteorder_htons(dst);
    hdr->checksum = byteorder_htons(0);
    hdr->off_ctl = byteorder_htons(TCP_HDR_OFFSET_MIN);

    TCP_DEBUG_LEAVE;
    return res;
}
