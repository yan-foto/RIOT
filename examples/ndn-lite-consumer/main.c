/*
 * Copyright (C) 2019 Tianyuan Yu
 *
 * This file is subject to the terms and conditions of the GNU Lesser
 * General Public License v2.1. See the file LICENSE in the top level
 * directory for more details.
 *
 * See AUTHORS.md for complete list of NDN IOT PKG authors and contributors.
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

#ifndef THREAD_STACKSIZE_SAMPLE_CONTENT
#define THREAD_STACKSIZE_SAMPLE_CONTENT THREAD_STACKSIZE_MAIN * 2
#endif

/* initialize stack sizes */
char sample_content_stack[THREAD_STACKSIZE_SAMPLE_CONTENT];
char ndn_lite_stack[THREAD_STACKSIZE_NDN_LITE];

/* define pid's for the threads */
kernel_pid_t sample_content_pid, ndn_lite_pid;

/* indicator to synchronize some threads with each other */
static bool ndn_lite_running;

void on_data(const uint8_t *rawdata, uint32_t data_size, void *userdata)
{
	ndn_data_t data;

	puts("On data");
	printf("This is the pointer to the userdata: %p\n", userdata);

	/* decode received data */
	if (ndn_data_tlv_decode_digest_verify(&data, rawdata, data_size)) {
		puts("Decoding failed");
	}

	/* as the content has 50 entries read the values */
	for (int i = 0; i < 50; i++)
		printf("It says: %d\n", *(data.content_value + i));
}

void on_timeout(void *userdata)
{
	puts("On timeout");
	printf("This is the userdata: %p\n", userdata);
}

void add_interface_to_forwarder(const char *prefix, size_t length)
{
	/* create face */
	ndn_netface_t *netface = ndn_netface_get_list();
	ndn_face_intf_t *face_ptr = &netface[0].intf;

	/* add created face to forwarder */
	ndn_forwarder_add_route_by_str(face_ptr, prefix, length);
}

void advertise_interest(const char *prefix, size_t length)
{
	/* initialize and set interest name */
	ndn_interest_t interest;
	ndn_interest_init(&interest);
	ndn_name_from_string(&interest.name, prefix, length);
	ndn_name_print(&interest.name);

	/* set interest metainfo */
	ndn_interest_set_MustBeFresh(&interest, true);
	ndn_interest_set_CanBePrefix(&interest, true);
	ndn_rng((uint8_t *)&interest.nonce, sizeof(interest.nonce));
	interest.lifetime = 5000;

	/* variables for preparing the sent data */
	ndn_encoder_t encoder;
	uint8_t buffer[250] = { 0 };

	/* encode the buffer and express interest to forwarder */
	encoder_init(&encoder, buffer, sizeof(buffer));
	int ret = ndn_interest_tlv_encode(&encoder, &interest);
	if (ret == 0) {
		puts("interest encoding success");
	}

	ndn_forwarder_express_interest(encoder.output_value, encoder.offset,
				       (ndn_on_data_func)on_data,
				       (ndn_on_timeout_func)on_timeout, NULL);
}

void *sample_content_thread(void *arg)
{
	(void)arg;
	puts("THREAD sample content start");

	/* set thread to sleep while ndn-lite is not up yet */
	while (ndn_lite_running == false) {
		thread_sleep();
	}

	/* add interest string to forwarder */
	add_interface_to_forwarder("/intf", strlen("/intf"));

	/* advertise interest string */
	advertise_interest("/intf/test", strlen("/intf/test"));

	puts("THREAD sample content end");
	return NULL;
}

void *ndn_lite_thread(void *arg)
{
	(void)arg;
	puts("THREAD ndn-lite start");

	/* functions to initialize ndn-lite */
	ndn_lite_startup();

	/* synchronize other threads */
	ndn_lite_running = true;
	thread_wakeup(sample_content_pid);

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

	/* sample-content thread */
	sample_content_pid = thread_create(
		sample_content_stack, sizeof(sample_content_stack),
		THREAD_PRIORITY_MAIN - 1,
		THREAD_CREATE_WOUT_YIELD | THREAD_CREATE_STACKTEST,
		sample_content_thread, NULL, "sample-content thread");

	return 0;
}
