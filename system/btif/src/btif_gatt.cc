/******************************************************************************
 *
 *  Copyright 2009-2013 Broadcom Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

/*******************************************************************************
 *
 *  Filename:      btif_gatt.c
 *
 *  Description:   GATT Profile Bluetooth Interface
 *
 ******************************************************************************/

#define LOG_TAG "bt_btif_gatt"

#include "btif_gatt.h"

#include <com_android_bluetooth_flags.h>
#include <hardware/bluetooth.h>
#include <hardware/bt_gatt.h>
#include <stdlib.h>
#include <string.h>

#include "bta/include/bta_gatt_api.h"
#include "btif/include/btif_common.h"
#include "main/shim/distance_measurement_manager.h"
#include "main/shim/le_advertising_manager.h"

// TODO(b/369381361) Enfore -Wmissing-prototypes
#pragma GCC diagnostic ignored "-Wmissing-prototypes"

const btgatt_callbacks_t* bt_gatt_callbacks = NULL;

/*******************************************************************************
 *
 * Function         btif_gatt_init
 *
 * Description      Initializes the GATT interface
 *
 * Returns          bt_status_t
 *
 ******************************************************************************/
static bt_status_t btif_gatt_init(const btgatt_callbacks_t* callbacks) {
  bt_gatt_callbacks = callbacks;
  BTA_GATTS_InitBonded();
  return BT_STATUS_SUCCESS;
}

/*******************************************************************************
 *
 * Function         btif_gatt_cleanup
 *
 * Description      Closes the GATT interface
 *
 * Returns          void
 *
 ******************************************************************************/
static void btif_gatt_cleanup(void) {
  if (bt_gatt_callbacks) {
    bt_gatt_callbacks = NULL;
  }

  BTA_GATTC_Disable();
  BTA_GATTS_Disable();
}

static btgatt_interface_t btgattInterface = {
        .size = sizeof(btgattInterface),

        .init = btif_gatt_init,
        .cleanup = btif_gatt_cleanup,

        .client = &btgattClientInterface,
        .server = &btgattServerInterface,
        .scanner = nullptr,    // filled in btif_gatt_get_interface
        .advertiser = nullptr  // filled in btif_gatt_get_interface
};

/*******************************************************************************
 *
 * Function         btif_gatt_get_interface
 *
 * Description      Get the gatt callback interface
 *
 * Returns          btgatt_interface_t
 *
 ******************************************************************************/
const btgatt_interface_t* btif_gatt_get_interface() {
  // TODO(jpawlowski) right now initializing advertiser field in static
  // structure cause explosion of dependencies. It must be initialized here
  // until those dependencies are properly abstracted for tests.
  btgattInterface.scanner = get_ble_scanner_instance();
  btgattInterface.advertiser = bluetooth::shim::get_ble_advertiser_instance();
  btgattInterface.distance_measurement_manager =
          bluetooth::shim::get_distance_measurement_instance();
  return &btgattInterface;
}
