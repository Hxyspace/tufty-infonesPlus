/* Bootstrap adapted from Pico SDK btstack_cyw43.c.
 * Copyright (c) 2023 Raspberry Pi (Trading) Ltd.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "btstack_memory.h"
#include "btstack_tlv.h"
#include "btstack_tlv_none.h"
#include "hci.h"
#include "pico/btstack_hci_transport_cyw43.h"
#include "pico/btstack_run_loop_async_context.h"
#include "pico/btstack_cyw43.h"

// SDK setup_tlv() reserves Flash banks at the end of Flash, overlapping /state.
// Keep transport/run-loop integration, with a RAM LE device DB and no Flash TLV.
bool __wrap_btstack_cyw43_init(async_context_t *context) {
    btstack_memory_init();
    btstack_run_loop_init(btstack_run_loop_async_context_get_instance(context));
    hci_init(hci_transport_cyw43_instance(),NULL);
    btstack_tlv_set_instance(btstack_tlv_none_init_instance(),NULL);
    return true;
}
