/*
 * Copyright (c) 2019-2024, Jacques Gagnon
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <esp_timer.h>
#include <esp_system.h>
#include "adapter/config.h"
#include "bluetooth/host.h"
#include "ps3.h"

static const uint8_t bt_init_magic[] = {
    0x42, 0x03, 0x00, 0x00
};

static const uint8_t ps3_config[] = {
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x02, 0xff, 0x27, 0x10, 0x00, 0x32, 0xff,
    0x27, 0x10, 0x00, 0x32, 0xff, 0x27, 0x10, 0x00,
    0x32, 0xff, 0x27, 0x10, 0x00, 0x32, 0x00, 0x00,
    0x00, 0x00, 0x00
};

static void bt_hid_cmd_ps3_bt_init(struct bt_dev *device) {
    struct bt_hidp_ps3_bt_init *bt_init = (struct bt_hidp_ps3_bt_init *)bt_hci_pkt_tmp.hidp_data;

    memcpy((void *)bt_init, bt_init_magic, sizeof(*bt_init));

    bt_hid_cmd(device->acl_handle, device->ctrl_chan.dcid, BT_HIDP_SET_FE, BT_HIDP_PS3_BT_INIT, sizeof(*bt_init));
}

static void bt_hid_ps3_init_callback(void *arg) {
    struct bt_dev *device = (struct bt_dev *)arg;
    struct bt_data *bt_data = &bt_adapter.data[device->ids.id];

    printf("# %s\n", __FUNCTION__);

    esp_timer_delete(device->timer_hdl);
    device->timer_hdl = NULL;
    bt_hid_cmd_ps3_set_conf(device, bt_data->base.output);

    atomic_set_bit(&device->flags, BT_DEV_HID_INIT_DONE);
}

void bt_hid_cmd_ps3_set_conf(struct bt_dev *device, void *report) {
    struct bt_hidp_ps3_set_conf *set_conf = (struct bt_hidp_ps3_set_conf *)bt_hci_pkt_tmp.hidp_data;
    memcpy((void *)set_conf, report, sizeof(*set_conf));
    //Added:
    uint8_t target_led_idx = (config.in_cfg[0].bt_subdev_id == 1) ? 1 : 0;
    set_conf->leds = (bt_hid_led_dev_id_map[target_led_idx] << 1);
    //^^^Added:
    bt_hid_cmd(device->acl_handle, device->ctrl_chan.dcid, BT_HIDP_SET_OUT, BT_HIDP_PS3_SET_CONF, sizeof(*set_conf));
}

void bt_hid_ps3_init(struct bt_dev *device) {
    struct bt_data *bt_data = &bt_adapter.data[device->ids.id];
    struct bt_hidp_ps3_set_conf *set_conf = (struct bt_hidp_ps3_set_conf *)bt_data->base.output;
    printf("# %s\n", __FUNCTION__);

    memcpy((void *)set_conf, ps3_config, sizeof(ps3_config));
    //set_conf->leds = (bt_hid_led_dev_id_map[device->ids.out_idx] << 1); //Original Line Commented out
    
    // ==========================================
    // OVERRIDE: Read saved LED preference instead of physical port
    // ==========================================
    uint8_t target_led_idx = (config.in_cfg[0].bt_subdev_id == 1) ? 1 : 0;
    set_conf->leds = (bt_hid_led_dev_id_map[target_led_idx] << 1);
    //^^^ADDED for Testing

    /* PS3 ctrl not yet ready to RX config, delay 20ms */
    const esp_timer_create_args_t ps3_timer_args = {
        .callback = &bt_hid_ps3_init_callback,
        .arg = (void *)device,
        .name = "ps3_init_timer"
    };
    esp_timer_create(&ps3_timer_args, (esp_timer_handle_t *)&device->timer_hdl);
    esp_timer_start_once(device->timer_hdl, 1000000);

    bt_hid_cmd_ps3_bt_init(device);
}

void bt_hid_ps3_hdlr(struct bt_dev *device, struct bt_hci_pkt *bt_hci_acl_pkt, uint32_t len) {
    uint32_t hidp_data_len = len - (BT_HCI_H4_HDR_SIZE + BT_HCI_ACL_HDR_SIZE
                                    + sizeof(struct bt_l2cap_hdr) + sizeof(struct bt_hidp_hdr));

    switch (bt_hci_acl_pkt->sig_hdr.code) {
        case BT_HIDP_DATA_IN:
            switch (bt_hci_acl_pkt->hidp_hdr.protocol) {
                case BT_HIDP_PS3_STATUS:
#ifdef CONFIG_BLUERETRO_ADAPTER_RUMBLE_TEST
                    struct bt_hidp_ps3_set_conf rumble;
                    memcpy(&rumble, ps3_config, sizeof(rumble));
                    if (bt_hci_acl_pkt->hidp_data[17] || bt_hci_acl_pkt->hidp_data[18]) {
                        rumble.r_rumble_pow = 0x01;
                    }
                    if (bt_hci_acl_pkt->hidp_data[17]) {
                        rumble.l_rumble_pow = bt_hci_acl_pkt->hidp_data[17];
                        rumble.l_rumble_len = 0xFF;
                    }
                    if (bt_hci_acl_pkt->hidp_data[18]) {
                        rumble.r_rumble_len = 0xFF;
                    }
                    bt_hid_cmd_ps3_set_conf(device, &rumble);
#else
                    /*
                    if (bt_hci_acl_pkt->hidp_data[0] != 0xFF) {
                        bt_host_bridge(device, bt_hci_acl_pkt->hidp_hdr.protocol, bt_hci_acl_pkt->hidp_data, hidp_data_len);
                    }
                    */

                    // ==========================================
                    // THE HOTKEY INTERCEPT (Dynamic LED Update)
                    // Select (0x01) + L3 (0x02) + R3 (0x04) + Start (0x08)
                    // ==========================================
                    static uint8_t swap_triggered = 0; 
                    
                    if ((bt_hci_acl_pkt->hidp_data[2] & 0x07) == 0x07) {
                        struct bt_hidp_ps3_set_conf blink;
                        memcpy(&blink, ps3_config, sizeof(ps3_config));
                        blink.leds = (0x08 << 1); 
                        bt_hid_cmd_ps3_set_conf(device, &blink);
                        if (!swap_triggered) {
                            swap_triggered = 1; // Lock trigger
                            printf("# Custom Combo: Toggling PS3 LED directly...\n");

                            // 1. Toggle our hijacked visual state variable
                            config.in_cfg[0].bt_subdev_id = (config.in_cfg[0].bt_subdev_id == 0) ? 1 : 0;

                            // 2. Save visual preference to physical flash memory
                            config_update(config_get_src());

                            // 3. Force the adapter to push an update immediately.
                            // The choke point at the top of the file will automatically catch this 
                            // and inject the correct LED byte!
                            struct bt_data *bt_data = &bt_adapter.data[device->ids.id];
                            bt_hid_cmd_ps3_set_conf(device, bt_data->base.output);
                        }
                    } else {
                        // The user let go of the buttons. Reset the lock.
                        swap_triggered = 0; 
                    }

                    // Proceed with normal gameplay input routing
                    if (bt_hci_acl_pkt->hidp_data[0] != 0xFF) {
                        bt_host_bridge(device, bt_hci_acl_pkt->hidp_hdr.protocol, bt_hci_acl_pkt->hidp_data, hidp_data_len);
                    }
                    //^^^Added for Testing
#endif
                    break;
            }
            break;
    }
}
