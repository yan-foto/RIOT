/*
 * Copyright (C) 2014 Freie Universität Berlin
 *
 * This file is subject to the terms and conditions of the GNU Lesser
 * General Public License v2.1. See the file LICENSE in the top level
 * directory for more details.
 */

/**
 * @ingroup     examples
 * @{
 *
 * @file
 * @brief       ndn-lite producer application
 *
 * @}
 */

#include <stdio.h>

#include "thread.h"
#include "timex.h"
#include "xtimer.h"

#include "ndn-lite/encode/data.h"
#include "ndn-lite/encode/interest.h"
#include "ndn-lite/encode/name.h"
#include "ndn-lite/forwarder/forwarder.h"
#include "ndn-lite/util/uniform-time.h"

#include "ndn-lite.h"
#include "netface/netface.h"

#ifndef THREAD_STACKSIZE_NDN_LITE
#define THREAD_STACKSIZE_NDN_LITE THREAD_STACKSIZE_MAIN * 4
#endif

#ifndef THREAD_STACKSIZE_REGISTRATION
#define THREAD_STACKSIZE_REGISTRATION THREAD_STACKSIZE_MAIN * 2
#endif

/* initialize stack sizes */
char registration_stack[THREAD_STACKSIZE_REGISTRATION];
char ndn_lite_stack[THREAD_STACKSIZE_NDN_LITE];

/* define pid's for the threads */
kernel_pid_t registration_pid, ndn_lite_pid;

/* indicator to synchronize ndn-lite thread with other threads */
static bool ndn_lite_running;

int on_interest(const uint8_t *interest, uint32_t interest_size, void *userdata)
{
    /* variables for preparing the sent data */
    ndn_encoder_t encoder;
    uint8_t content[50];
    uint8_t buffer[250] = { 0 };
    ndn_data_t data;
    ndn_interest_t incoming;
    int ret;

    /* filling content with rising numbers */
    for (uint8_t i = 0; i < sizeof(content); i++) {
        content[i] = i;
    }

    ndn_interest_from_block(&incoming, interest, interest_size);

    printf("On interest: ");
    ndn_name_print(&(incoming.name));

    printf("This is the pointer to the userdata: %p\n", userdata);

    /* set metainfo of the data packet */
    ret = ndn_name_from_string(&data.name, "/intf/test/01",
                               strlen("/intf/test/01"));
    if (ret != 0) {
        puts("Adding name to data packet failed");
        return ret;
    }

    ret = ndn_data_set_content(&data, content, sizeof(content));
    if (ret != 0) {
        puts("Setting content to data packet failed");
        return ret;
    }

    ndn_metainfo_init(&data.metainfo);
    ndn_metainfo_set_content_type(&data.metainfo, NDN_CONTENT_TYPE_BLOB);

    /* encode the data for forwarder */
    encoder_init(&encoder, buffer, sizeof(buffer));
    ret = ndn_data_tlv_encode_digest_sign(&encoder, &data);
    if (ret != 0) {
        puts("Sign data packet failed");
        return ret;
    }

    ret = ndn_forwarder_put_data(encoder.output_value, encoder.offset);
    if (ret != 0) {
        puts("Produce data packet failed");
        return ret;
    }

    return ret;
}

void register_interest(const char *prefix, size_t length)
{
    ndn_name_t name;

    /* register prefix to forwarder by name */
    ndn_name_from_string(&name, prefix, length);
    ndn_name_print(&name);
    ndn_forwarder_register_name_prefix(
        &name, (ndn_on_interest_func)on_interest, NULL);
}

void *registration_thread(void *arg)
{
    (void)arg;
    puts("THREAD registration start");

    /* set thread to sleep while ndn-lite is not up yet */
    while (ndn_lite_running == false) {
        thread_sleep();
    }

    /* register interest with string */
    register_interest("/intf/test", strlen("/intf/test"));

    puts("THREAD registration end");
    return NULL;
}

void *ndn_lite_thread(void *arg)
{
    (void)arg;
    puts("THREAD ndn-lite start");

    /* call functions to initialize ndn-lite */
    ndn_lite_startup();

    /* synchronize other threads */
    ndn_lite_running = true;
    thread_wakeup(registration_pid);

    /* loop for processing incoming data */
    while (ndn_lite_running) {
        ndn_forwarder_process();
        xtimer_sleep(1);
    }

    puts("THREAD ndn-lite end");
    return NULL;
}

int main(void)
{
    printf("You are running RIOT on a(n) %s board.\n", RIOT_BOARD);
    printf("This board features a(n) %s MCU.\n", RIOT_MCU);

    /* delay to ensure a successful connecting over wifi or esp-now */
    xtimer_sleep(10);

    /* ndn-lite thread */
    ndn_lite_pid = thread_create(ndn_lite_stack, sizeof(ndn_lite_stack),
                                 THREAD_PRIORITY_MAIN - 1,
                                 THREAD_CREATE_WOUT_YIELD |
                                 THREAD_CREATE_STACKTEST,
                                 ndn_lite_thread, NULL, "ndn-lite thread");

    /* registration thread */
    registration_pid = thread_create(registration_stack,
                                     sizeof(registration_stack),
                                     THREAD_PRIORITY_MAIN - 1,
                                     THREAD_CREATE_WOUT_YIELD | THREAD_CREATE_STACKTEST,
                                     registration_thread, NULL,
                                     "registration thread");

    return 0;
}
