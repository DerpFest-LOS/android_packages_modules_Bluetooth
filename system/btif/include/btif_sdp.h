/******************************************************************************
 *
 *  Copyright 2014 The Android Open Source Project
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
 *  Filename:      btif_sdp.h
 *
 *  Description:   Bluetooth SDP search Interface
 *
 ******************************************************************************/

#ifndef BTIF_SDP_H
#define BTIF_SDP_H

#include <hardware/bt_sdp.h>

const btsdp_interface_t* btif_sdp_get_interface();
bt_status_t btif_sdp_execute_service(bool b_enable);

bt_status_t sdp_server_init();
void sdp_server_cleanup();

#endif
