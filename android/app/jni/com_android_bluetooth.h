/*
 * Copyright (c) 2014 The Android Open Source Project
 * Copyright (C) 2012 The Android Open Source Project
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

#pragma once

#include <bluetooth/log.h>

#include "hardware/bluetooth.h"
#include "hardware/hardware.h"
#include "jni.h"
#include "nativehelper/ScopedLocalRef.h"

namespace log = bluetooth::log;

namespace android {

JNIEnv* getCallbackEnv();
bool isCallbackThread();

class CallbackEnv {
public:
  CallbackEnv(const char* methodName) : mName(methodName) { mCallbackEnv = getCallbackEnv(); }

  ~CallbackEnv() {
    if (mCallbackEnv && mCallbackEnv->ExceptionCheck()) {
      log::error("An exception was thrown by callback '{}'.", mName);
      jniLogException(mCallbackEnv, ANDROID_LOG_ERROR, LOG_TAG);
      mCallbackEnv->ExceptionClear();
    }
  }

  bool valid() const {
    if (!mCallbackEnv || !isCallbackThread()) {
      log::error("{}: Callback env fail", mName);
      return false;
    }
    return true;
  }

  // stolen from art/runtime/jni/check_jni.cc
  bool isValidUtf(const char* bytes) const {
    while (*bytes != '\0') {
      const uint8_t* utf8 = reinterpret_cast<const uint8_t*>(bytes++);
      // Switch on the high four bits.
      switch (*utf8 >> 4) {
        case 0x00:
        case 0x01:
        case 0x02:
        case 0x03:
        case 0x04:
        case 0x05:
        case 0x06:
        case 0x07:
          // Bit pattern 0xxx. No need for any extra bytes.
          break;
        case 0x08:
        case 0x09:
        case 0x0a:
        case 0x0b:
          // Bit patterns 10xx, which are illegal start bytes.
          return false;
        case 0x0f:
          // Bit pattern 1111, which might be the start of a 4 byte sequence.
          if ((*utf8 & 0x08) == 0) {
            // Bit pattern 1111 0xxx, which is the start of a 4 byte sequence.
            // We consume one continuation byte here, and fall through to
            // consume two more.
            utf8 = reinterpret_cast<const uint8_t*>(bytes++);
            if ((*utf8 & 0xc0) != 0x80) {
              return false;
            }
          } else {
            return false;
          }
          // Fall through to the cases below to consume two more continuation
          // bytes.
          [[fallthrough]];
        case 0x0e:
          // Bit pattern 1110, so there are two additional bytes.
          utf8 = reinterpret_cast<const uint8_t*>(bytes++);
          if ((*utf8 & 0xc0) != 0x80) {
            return false;
          }
          // Fall through to consume one more continuation byte.
          [[fallthrough]];
        case 0x0c:
        case 0x0d:
          // Bit pattern 110x, so there is one additional byte.
          utf8 = reinterpret_cast<const uint8_t*>(bytes++);
          if ((*utf8 & 0xc0) != 0x80) {
            return false;
          }
          break;
      }
    }
    return true;
  }

  JNIEnv* operator->() const { return mCallbackEnv; }

  JNIEnv* get() const { return mCallbackEnv; }

private:
  JNIEnv* mCallbackEnv;
  const char* mName;

  CallbackEnv(const CallbackEnv&) = delete;
  void operator=(const CallbackEnv&) = delete;
};

const bt_interface_t* getBluetoothInterface();

int register_com_android_bluetooth_hfp(JNIEnv* env);

int register_com_android_bluetooth_hfpclient(JNIEnv* env);

int register_com_android_bluetooth_a2dp(JNIEnv* env);

int register_com_android_bluetooth_a2dp_sink(JNIEnv* env);

int register_com_android_bluetooth_avrcp(JNIEnv* env);

int register_com_android_bluetooth_avrcp_target(JNIEnv* env);

int register_com_android_bluetooth_avrcp_controller(JNIEnv* env);

int register_com_android_bluetooth_hid_host(JNIEnv* env);

int register_com_android_bluetooth_hid_device(JNIEnv* env);

int register_com_android_bluetooth_pan(JNIEnv* env);

int register_com_android_bluetooth_gatt(JNIEnv* env);

int register_com_android_bluetooth_sdp(JNIEnv* env);

int register_com_android_bluetooth_hearing_aid(JNIEnv* env);

int register_com_android_bluetooth_hap_client(JNIEnv* env);

int register_com_android_bluetooth_btservice_BluetoothKeystore(JNIEnv* env);

int register_com_android_bluetooth_le_audio(JNIEnv* env);

int register_com_android_bluetooth_vc(JNIEnv* env);

int register_com_android_bluetooth_csip_set_coordinator(JNIEnv* env);

int register_com_android_bluetooth_btservice_BluetoothQualityReport(JNIEnv* env);

int register_com_android_bluetooth_btservice_BluetoothHciVendorSpecific(JNIEnv* env);

struct JNIJavaMethod {
  const char* name;
  const char* signature;
  jmethodID* id;
  bool is_static{false};
};

void jniGetMethodsOrDie(JNIEnv* env, const char* className, const JNIJavaMethod* methods,
                        int nMethods);

#define REGISTER_NATIVE_METHODS(env, classname, methodsArray) \
  jniRegisterNativeMethods(env, classname, methodsArray, NELEM(methodsArray))

#define GET_JAVA_METHODS(env, classname, methodsArray) \
  jniGetMethodsOrDie(env, classname, methodsArray, NELEM(methodsArray))

}  // namespace android
