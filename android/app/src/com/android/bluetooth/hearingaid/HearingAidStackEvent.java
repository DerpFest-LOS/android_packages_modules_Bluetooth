/*
 * Copyright 2018 The Android Open Source Project
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

package com.android.bluetooth.hearingaid;

import android.bluetooth.BluetoothDevice;

/**
 * Stack event sent via a callback from JNI to Java, or generated internally by the Hearing Aid
 * State Machine.
 */
class HearingAidStackEvent {
    // Event types for STACK_EVENT message (coming from native)
    private static final int EVENT_TYPE_NONE = 0;
    public static final int EVENT_TYPE_CONNECTION_STATE_CHANGED = 1;
    public static final int EVENT_TYPE_DEVICE_AVAILABLE = 2;

    public int type;
    public BluetoothDevice device;
    public int valueInt1;
    public long valueLong2;

    HearingAidStackEvent(int type) {
        this.type = type;
    }

    @Override
    public String toString() {
        // event dump
        StringBuilder result = new StringBuilder();
        result.append("HearingAidStackEvent {type:").append(eventTypeToString(type));
        result.append(", device:").append(device);
        result.append(", value1:").append(valueInt1);
        result.append(", value2:").append(valueLong2);
        result.append("}");
        return result.toString();
    }

    private static String eventTypeToString(int type) {
        switch (type) {
            case EVENT_TYPE_NONE:
                return "EVENT_TYPE_NONE";
            case EVENT_TYPE_CONNECTION_STATE_CHANGED:
                return "EVENT_TYPE_CONNECTION_STATE_CHANGED";
            case EVENT_TYPE_DEVICE_AVAILABLE:
                return "EVENT_TYPE_DEVICE_AVAILABLE";
            default:
                return "EVENT_TYPE_UNKNOWN:" + type;
        }
    }
}
