/*
 * Copyright 2024 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package bluetooth.constants;

/**
 * See Bluetooth SIG Assigned Numbers 6.12.2 Audio Input Type Definitions
 * {@hide}
 */
@JavaDerive(toString = true)
@Backing(type="int")
enum AudioInputType {
    UNSPECIFIED = 0x00,
    BLUETOOTH = 0x01,
    MICROPHONE = 0x02,
    ANALOG = 0x03,
    DIGITAL = 0x04,
    RADIO = 0x05,
    STREAMING = 0x06,
    AMBIENT = 0x07,
}
