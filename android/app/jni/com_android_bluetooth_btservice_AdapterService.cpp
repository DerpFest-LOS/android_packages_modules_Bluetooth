/*
 * Copyright (C) 2016-2017 The Linux Foundation
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

#define LOG_TAG "BluetoothServiceJni"

#include <android/log.h>
#include <bluetooth/log.h>
#include <jni.h>
#include <nativehelper/JNIHelp.h>
#include <nativehelper/JNIPlatformHelp.h>
#include <nativehelper/scoped_local_ref.h>
#include <pthread.h>
#include <sys/prctl.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

#include "com_android_bluetooth.h"
#include "hardware/bluetooth.h"
#include "hardware/bt_sock.h"
#include "types/bluetooth/uuid.h"
#include "types/bt_transport.h"
#include "types/raw_address.h"

// TODO(b/369381361) Enfore -Wmissing-prototypes
#pragma GCC diagnostic ignored "-Wmissing-prototypes"

using bluetooth::Uuid;
extern bt_interface_t bluetoothInterface;

namespace std {
template <>
struct formatter<bt_state_t> : enum_formatter<bt_state_t> {};
template <>
struct formatter<bt_discovery_state_t> : enum_formatter<bt_discovery_state_t> {};
}  // namespace std

static Uuid from_java_uuid(jlong uuid_msb, jlong uuid_lsb) {
  std::array<uint8_t, Uuid::kNumBytes128> uu;
  for (int i = 0; i < 8; i++) {
    uu[7 - i] = (uuid_msb >> (8 * i)) & 0xFF;
    uu[15 - i] = (uuid_lsb >> (8 * i)) & 0xFF;
  }
  return Uuid::From128BitBE(uu);
}

namespace {
tBT_TRANSPORT to_bt_transport(jint val) {
  switch (val) {
    case 0:
      return BT_TRANSPORT_AUTO;
    case 1:
      return BT_TRANSPORT_BR_EDR;
    case 2:
      return BT_TRANSPORT_LE;
    default:
      break;
  }
  log::warn("Passed unexpected transport value:{}", val);
  return BT_TRANSPORT_AUTO;
}

}  // namespace

namespace android {

#define BLE_ADDR_PUBLIC 0x00
#define BLE_ADDR_RANDOM 0x01

const jint INVALID_FD = -1;
const jint INVALID_CID = -1;

static jmethodID method_oobDataReceivedCallback;
static jmethodID method_stateChangeCallback;
static jmethodID method_adapterPropertyChangedCallback;
static jmethodID method_devicePropertyChangedCallback;
static jmethodID method_deviceFoundCallback;
static jmethodID method_pinRequestCallback;
static jmethodID method_sspRequestCallback;
static jmethodID method_bondStateChangeCallback;
static jmethodID method_addressConsolidateCallback;
static jmethodID method_leAddressAssociateCallback;
static jmethodID method_aclStateChangeCallback;
static jmethodID method_discoveryStateChangeCallback;
static jmethodID method_linkQualityReportCallback;
static jmethodID method_switchBufferSizeCallback;
static jmethodID method_switchCodecCallback;
static jmethodID method_acquireWakeLock;
static jmethodID method_releaseWakeLock;
static jmethodID method_energyInfo;
static jmethodID method_keyMissingCallback;
static jmethodID method_encryptionChangeCallback;

static struct {
  jclass clazz;
  jmethodID constructor;
} android_bluetooth_UidTraffic;

static const bt_interface_t* sBluetoothInterface = NULL;
static const btsock_interface_t* sBluetoothSocketInterface = NULL;
static JavaVM* vm = NULL;
static JNIEnv* callbackEnv = NULL;
static pthread_t sCallbackThread;
static bool sHaveCallbackThread;

static jobject sJniAdapterServiceObj;
static jobject sJniCallbacksObj;
static std::shared_timed_mutex jniObjMutex;
static jfieldID sJniCallbacksField;

const bt_interface_t* getBluetoothInterface() { return sBluetoothInterface; }

JNIEnv* getCallbackEnv() { return callbackEnv; }

bool isCallbackThread() {
  pthread_t curThread = pthread_self();
  bool isValid = sHaveCallbackThread && pthread_equal(sCallbackThread, curThread);
  if (!isValid) {
    log::error("Failed! sHaveCallbackThread={}, pthread_self()={}, sCallbackThread={}",
               sHaveCallbackThread, curThread, sCallbackThread);
  }
  return isValid;
}

static void adapter_state_change_callback(bt_state_t status) {
  std::shared_lock<std::shared_timed_mutex> lock(jniObjMutex);
  if (!sJniCallbacksObj) {
    log::error("JNI obj is null. Failed to call JNI callback");
    return;
  }

  CallbackEnv sCallbackEnv(__func__);
  if (!sCallbackEnv.valid()) {
    return;
  }
  log::verbose("Status is: {}", status);

  sCallbackEnv->CallVoidMethod(sJniCallbacksObj, method_stateChangeCallback, (jint)status);
}

static int get_properties(int num_properties, bt_property_t* properties, jintArray* types,
                          jobjectArray* props) {
  for (int i = 0; i < num_properties; i++) {
    ScopedLocalRef<jbyteArray> propVal(callbackEnv, callbackEnv->NewByteArray(properties[i].len));
    if (!propVal.get()) {
      log::error("Error while allocation of array");
      return -1;
    }

    callbackEnv->SetByteArrayRegion(propVal.get(), 0, properties[i].len,
                                    reinterpret_cast<jbyte*>(properties[i].val));
    callbackEnv->SetObjectArrayElement(*props, i, propVal.get());
    callbackEnv->SetIntArrayRegion(*types, i, 1, reinterpret_cast<jint*>(&properties[i].type));
  }
  return 0;
}

static void adapter_properties_callback(bt_status_t status, int num_properties,
                                        bt_property_t* properties) {
  std::shared_lock<std::shared_timed_mutex> lock(jniObjMutex);
  if (!sJniCallbacksObj) {
    log::error("JNI obj is null. Failed to call JNI callback");
    return;
  }

  CallbackEnv sCallbackEnv(__func__);
  if (!sCallbackEnv.valid()) {
    return;
  }

  log::verbose("Status is: {}, Properties: {}", bt_status_text(status), num_properties);

  if (status != BT_STATUS_SUCCESS) {
    log::error("Status {} is incorrect", bt_status_text(status));
    return;
  }

  ScopedLocalRef<jbyteArray> val(sCallbackEnv.get(),
                                 (jbyteArray)sCallbackEnv->NewByteArray(num_properties));
  if (!val.get()) {
    log::error("Error allocating byteArray");
    return;
  }

  ScopedLocalRef<jclass> mclass(sCallbackEnv.get(), sCallbackEnv->GetObjectClass(val.get()));

  /* (BT) Initialize the jobjectArray and jintArray here itself and send the
   initialized array pointers alone to get_properties */

  ScopedLocalRef<jobjectArray> props(
          sCallbackEnv.get(), sCallbackEnv->NewObjectArray(num_properties, mclass.get(), NULL));
  if (!props.get()) {
    log::error("Error allocating object Array for properties");
    return;
  }

  ScopedLocalRef<jintArray> types(sCallbackEnv.get(),
                                  (jintArray)sCallbackEnv->NewIntArray(num_properties));
  if (!types.get()) {
    log::error("Error allocating int Array for values");
    return;
  }

  jintArray typesPtr = types.get();
  jobjectArray propsPtr = props.get();
  if (get_properties(num_properties, properties, &typesPtr, &propsPtr) < 0) {
    return;
  }

  sCallbackEnv->CallVoidMethod(sJniCallbacksObj, method_adapterPropertyChangedCallback, types.get(),
                               props.get());
}

static void remote_device_properties_callback(bt_status_t status, RawAddress* bd_addr,
                                              int num_properties, bt_property_t* properties) {
  std::shared_lock<std::shared_timed_mutex> lock(jniObjMutex);
  if (!sJniCallbacksObj) {
    log::error("JNI obj is null. Failed to call JNI callback");
    return;
  }

  CallbackEnv sCallbackEnv(__func__);
  if (!sCallbackEnv.valid()) {
    return;
  }

  log::verbose("Device: {}, Status: {}, Properties: {}", *bd_addr, bt_status_text(status),
               num_properties);

  if (status != BT_STATUS_SUCCESS) {
    log::error("Status {} is incorrect", bt_status_text(status));
    return;
  }

  ScopedLocalRef<jbyteArray> val(sCallbackEnv.get(),
                                 (jbyteArray)sCallbackEnv->NewByteArray(num_properties));
  if (!val.get()) {
    log::error("Error allocating byteArray");
    return;
  }

  ScopedLocalRef<jclass> mclass(sCallbackEnv.get(), sCallbackEnv->GetObjectClass(val.get()));

  /* Initialize the jobjectArray and jintArray here itself and send the
   initialized array pointers alone to get_properties */

  ScopedLocalRef<jobjectArray> props(
          sCallbackEnv.get(), sCallbackEnv->NewObjectArray(num_properties, mclass.get(), NULL));
  if (!props.get()) {
    log::error("Error allocating object Array for properties");
    return;
  }

  ScopedLocalRef<jintArray> types(sCallbackEnv.get(),
                                  (jintArray)sCallbackEnv->NewIntArray(num_properties));
  if (!types.get()) {
    log::error("Error allocating int Array for values");
    return;
  }

  ScopedLocalRef<jbyteArray> addr(sCallbackEnv.get(),
                                  sCallbackEnv->NewByteArray(sizeof(RawAddress)));
  if (!addr.get()) {
    log::error("Error while allocation byte array");
    return;
  }

  sCallbackEnv->SetByteArrayRegion(addr.get(), 0, sizeof(RawAddress),
                                   reinterpret_cast<jbyte*>(bd_addr));

  jintArray typesPtr = types.get();
  jobjectArray propsPtr = props.get();
  if (get_properties(num_properties, properties, &typesPtr, &propsPtr) < 0) {
    return;
  }

  sCallbackEnv->CallVoidMethod(sJniCallbacksObj, method_devicePropertyChangedCallback, addr.get(),
                               types.get(), props.get());
}

static void device_found_callback(int num_properties, bt_property_t* properties) {
  std::shared_lock<std::shared_timed_mutex> lock(jniObjMutex);
  if (!sJniCallbacksObj) {
    log::error("JNI obj is null. Failed to call JNI callback");
    return;
  }

  CallbackEnv sCallbackEnv(__func__);
  if (!sCallbackEnv.valid()) {
    return;
  }

  ScopedLocalRef<jbyteArray> addr(sCallbackEnv.get(), NULL);
  int addr_index;
  for (int i = 0; i < num_properties; i++) {
    if (properties[i].type == BT_PROPERTY_BDADDR) {
      addr.reset(sCallbackEnv->NewByteArray(properties[i].len));
      if (!addr.get()) {
        log::error("Address is NULL (unable to allocate)");
        return;
      }
      sCallbackEnv->SetByteArrayRegion(addr.get(), 0, properties[i].len,
                                       reinterpret_cast<jbyte*>(properties[i].val));
      addr_index = i;
    }
  }
  if (!addr.get()) {
    log::error("Address is NULL");
    return;
  }

  log::verbose("Properties: {}, Address: {}", num_properties,
               *reinterpret_cast<RawAddress*>(properties[addr_index].val));

  remote_device_properties_callback(BT_STATUS_SUCCESS,
                                    reinterpret_cast<RawAddress*>(properties[addr_index].val),
                                    num_properties, properties);

  sCallbackEnv->CallVoidMethod(sJniCallbacksObj, method_deviceFoundCallback, addr.get());
}

static void bond_state_changed_callback(bt_status_t status, RawAddress* bd_addr,
                                        bt_bond_state_t state, int fail_reason) {
  std::shared_lock<std::shared_timed_mutex> lock(jniObjMutex);
  if (!sJniCallbacksObj) {
    log::error("JNI obj is null. Failed to call JNI callback");
    return;
  }

  CallbackEnv sCallbackEnv(__func__);
  if (!sCallbackEnv.valid()) {
    return;
  }

  if (!bd_addr) {
    log::error("Address is null");
    return;
  }

  ScopedLocalRef<jbyteArray> addr(sCallbackEnv.get(),
                                  sCallbackEnv->NewByteArray(sizeof(RawAddress)));
  if (!addr.get()) {
    log::error("Address allocation failed");
    return;
  }
  sCallbackEnv->SetByteArrayRegion(addr.get(), 0, sizeof(RawAddress),
                                   reinterpret_cast<jbyte*>(bd_addr));

  sCallbackEnv->CallVoidMethod(sJniCallbacksObj, method_bondStateChangeCallback, (jint)status,
                               addr.get(), (jint)state, (jint)fail_reason);
}

static void address_consolidate_callback(RawAddress* main_bd_addr, RawAddress* secondary_bd_addr) {
  std::shared_lock<std::shared_timed_mutex> lock(jniObjMutex);
  if (!sJniCallbacksObj) {
    log::error("JNI obj is null. Failed to call JNI callback");
    return;
  }

  CallbackEnv sCallbackEnv(__func__);

  ScopedLocalRef<jbyteArray> main_addr(sCallbackEnv.get(),
                                       sCallbackEnv->NewByteArray(sizeof(RawAddress)));
  if (!main_addr.get()) {
    log::error("Address allocation failed");
    return;
  }
  sCallbackEnv->SetByteArrayRegion(main_addr.get(), 0, sizeof(RawAddress),
                                   reinterpret_cast<jbyte*>(main_bd_addr));

  ScopedLocalRef<jbyteArray> secondary_addr(sCallbackEnv.get(),
                                            sCallbackEnv->NewByteArray(sizeof(RawAddress)));
  if (!secondary_addr.get()) {
    log::error("Address allocation failed");
    return;
  }

  sCallbackEnv->SetByteArrayRegion(secondary_addr.get(), 0, sizeof(RawAddress),
                                   reinterpret_cast<jbyte*>(secondary_bd_addr));

  sCallbackEnv->CallVoidMethod(sJniCallbacksObj, method_addressConsolidateCallback, main_addr.get(),
                               secondary_addr.get());
}

static void le_address_associate_callback(RawAddress* main_bd_addr, RawAddress* secondary_bd_addr,
                                          uint8_t identity_address_type) {
  std::shared_lock<std::shared_timed_mutex> lock(jniObjMutex);
  if (!sJniCallbacksObj) {
    log::error("JNI obj is null. Failed to call JNI callback");
    return;
  }

  CallbackEnv sCallbackEnv(__func__);

  ScopedLocalRef<jbyteArray> main_addr(sCallbackEnv.get(),
                                       sCallbackEnv->NewByteArray(sizeof(RawAddress)));
  if (!main_addr.get()) {
    log::error("Address allocation failed");
    return;
  }
  sCallbackEnv->SetByteArrayRegion(main_addr.get(), 0, sizeof(RawAddress),
                                   reinterpret_cast<jbyte*>(main_bd_addr));

  ScopedLocalRef<jbyteArray> secondary_addr(sCallbackEnv.get(),
                                            sCallbackEnv->NewByteArray(sizeof(RawAddress)));
  if (!secondary_addr.get()) {
    log::error("Address allocation failed");
    return;
  }

  sCallbackEnv->SetByteArrayRegion(secondary_addr.get(), 0, sizeof(RawAddress),
                                   reinterpret_cast<jbyte*>(secondary_bd_addr));

  sCallbackEnv->CallVoidMethod(sJniCallbacksObj, method_leAddressAssociateCallback, main_addr.get(),
                               secondary_addr.get(), (jint)identity_address_type);
}

static void acl_state_changed_callback(bt_status_t status, RawAddress* bd_addr,
                                       bt_acl_state_t state, int transport_link_type,
                                       bt_hci_error_code_t hci_reason,
                                       bt_conn_direction_t /* direction */, uint16_t acl_handle) {
  if (!bd_addr) {
    log::error("Address is null");
    return;
  }

  std::shared_lock<std::shared_timed_mutex> lock(jniObjMutex);
  if (!sJniCallbacksObj) {
    log::error("JNI obj is null. Failed to call JNI callback");
    return;
  }

  CallbackEnv sCallbackEnv(__func__);
  if (!sCallbackEnv.valid()) {
    return;
  }

  ScopedLocalRef<jbyteArray> addr(sCallbackEnv.get(),
                                  sCallbackEnv->NewByteArray(sizeof(RawAddress)));
  if (!addr.get()) {
    log::error("Address allocation failed");
    return;
  }
  sCallbackEnv->SetByteArrayRegion(addr.get(), 0, sizeof(RawAddress),
                                   reinterpret_cast<jbyte*>(bd_addr));

  sCallbackEnv->CallVoidMethod(sJniCallbacksObj, method_aclStateChangeCallback, (jint)status,
                               addr.get(), (jint)state, (jint)transport_link_type, (jint)hci_reason,
                               (jint)acl_handle);
}

static void discovery_state_changed_callback(bt_discovery_state_t state) {
  std::shared_lock<std::shared_timed_mutex> lock(jniObjMutex);
  if (!sJniCallbacksObj) {
    log::error("JNI obj is null. Failed to call JNI callback");
    return;
  }

  CallbackEnv sCallbackEnv(__func__);
  if (!sCallbackEnv.valid()) {
    return;
  }

  log::verbose("DiscoveryState:{}", state);

  sCallbackEnv->CallVoidMethod(sJniCallbacksObj, method_discoveryStateChangeCallback, (jint)state);
}

static void pin_request_callback(RawAddress* bd_addr, bt_bdname_t* bdname, uint32_t cod,
                                 bool min_16_digits) {
  if (!bd_addr) {
    log::error("Address is null");
    return;
  }

  std::shared_lock<std::shared_timed_mutex> lock(jniObjMutex);
  if (!sJniCallbacksObj) {
    log::error("JNI obj is null. Failed to call JNI callback");
    return;
  }

  CallbackEnv sCallbackEnv(__func__);
  if (!sCallbackEnv.valid()) {
    return;
  }

  ScopedLocalRef<jbyteArray> addr(sCallbackEnv.get(),
                                  sCallbackEnv->NewByteArray(sizeof(RawAddress)));
  if (!addr.get()) {
    log::error("Error while allocating");
    return;
  }

  sCallbackEnv->SetByteArrayRegion(addr.get(), 0, sizeof(RawAddress),
                                   reinterpret_cast<jbyte*>(bd_addr));

  ScopedLocalRef<jbyteArray> devname(sCallbackEnv.get(),
                                     sCallbackEnv->NewByteArray(sizeof(bt_bdname_t)));
  if (!devname.get()) {
    log::error("Error while allocating");
    return;
  }

  sCallbackEnv->SetByteArrayRegion(devname.get(), 0, sizeof(bt_bdname_t),
                                   reinterpret_cast<jbyte*>(bdname));

  sCallbackEnv->CallVoidMethod(sJniCallbacksObj, method_pinRequestCallback, addr.get(),
                               devname.get(), cod, min_16_digits);
}

static void ssp_request_callback(RawAddress* bd_addr, bt_ssp_variant_t pairing_variant,
                                 uint32_t pass_key) {
  if (!bd_addr) {
    log::error("Address is null");
    return;
  }

  std::shared_lock<std::shared_timed_mutex> lock(jniObjMutex);
  if (!sJniCallbacksObj) {
    log::error("JNI obj is null. Failed to call JNI callback");
    return;
  }

  CallbackEnv sCallbackEnv(__func__);
  if (!sCallbackEnv.valid()) {
    return;
  }

  ScopedLocalRef<jbyteArray> addr(sCallbackEnv.get(),
                                  sCallbackEnv->NewByteArray(sizeof(RawAddress)));
  if (!addr.get()) {
    log::error("Error while allocating");
    return;
  }

  sCallbackEnv->SetByteArrayRegion(addr.get(), 0, sizeof(RawAddress),
                                   reinterpret_cast<jbyte*>(bd_addr));

  sCallbackEnv->CallVoidMethod(sJniCallbacksObj, method_sspRequestCallback, addr.get(),
                               (jint)pairing_variant, pass_key);
}

static jobject createClassicOobDataObject(JNIEnv* env, bt_oob_data_t oob_data) {
  log::verbose("");
  jmethodID classicBuilderConstructor;
  jmethodID setRMethod;
  jmethodID setNameMethod;
  jmethodID buildMethod;

  const JNIJavaMethod javaMethods[] = {
          {"<init>", "([B[B[B)V", &classicBuilderConstructor},
          {"setRandomizerHash", "([B)Landroid/bluetooth/OobData$ClassicBuilder;", &setRMethod},
          {"setDeviceName", "([B)Landroid/bluetooth/OobData$ClassicBuilder;", &setNameMethod},
          {"build", "()Landroid/bluetooth/OobData;", &buildMethod},
  };
  GET_JAVA_METHODS(env, "android/bluetooth/OobData$ClassicBuilder", javaMethods);

  jbyteArray confirmationHash = env->NewByteArray(OOB_C_SIZE);
  env->SetByteArrayRegion(confirmationHash, 0, OOB_C_SIZE, reinterpret_cast<jbyte*>(oob_data.c));

  jbyteArray oobDataLength = env->NewByteArray(OOB_DATA_LEN_SIZE);
  env->SetByteArrayRegion(oobDataLength, 0, OOB_DATA_LEN_SIZE,
                          reinterpret_cast<jbyte*>(oob_data.oob_data_length));

  jbyteArray address = env->NewByteArray(OOB_ADDRESS_SIZE);
  env->SetByteArrayRegion(address, 0, OOB_ADDRESS_SIZE, reinterpret_cast<jbyte*>(oob_data.address));

  jclass classicBuilderClass = env->FindClass("android/bluetooth/OobData$ClassicBuilder");

  jobject oobDataClassicBuilder = env->NewObject(classicBuilderClass, classicBuilderConstructor,
                                                 confirmationHash, oobDataLength, address);

  env->DeleteLocalRef(classicBuilderClass);

  jbyteArray randomizerHash = env->NewByteArray(OOB_R_SIZE);
  env->SetByteArrayRegion(randomizerHash, 0, OOB_R_SIZE, reinterpret_cast<jbyte*>(oob_data.r));

  oobDataClassicBuilder = env->CallObjectMethod(oobDataClassicBuilder, setRMethod, randomizerHash);

  int name_char_count = 0;
  for (int i = 0; i < OOB_NAME_MAX_SIZE; i++) {
    if (oob_data.device_name[i] == 0) {
      name_char_count = i;
      break;
    }
  }

  jbyteArray deviceName = env->NewByteArray(name_char_count);
  env->SetByteArrayRegion(deviceName, 0, name_char_count,
                          reinterpret_cast<jbyte*>(oob_data.device_name));

  oobDataClassicBuilder = env->CallObjectMethod(oobDataClassicBuilder, setNameMethod, deviceName);

  return env->CallObjectMethod(oobDataClassicBuilder, buildMethod);
}

static jobject createLeOobDataObject(JNIEnv* env, bt_oob_data_t oob_data) {
  log::verbose("");

  jmethodID leBuilderConstructor;
  jmethodID setRMethod;
  jmethodID setNameMethod;
  jmethodID buildMethod;

  const JNIJavaMethod javaMethods[] = {
          {"<init>", "([B[BI)V", &leBuilderConstructor},
          {"setRandomizerHash", "([B)Landroid/bluetooth/OobData$LeBuilder;", &setRMethod},
          {"setDeviceName", "([B)Landroid/bluetooth/OobData$LeBuilder;", &setNameMethod},
          {"build", "()Landroid/bluetooth/OobData;", &buildMethod},
  };
  GET_JAVA_METHODS(env, "android/bluetooth/OobData$LeBuilder", javaMethods);

  jbyteArray confirmationHash = env->NewByteArray(OOB_C_SIZE);
  env->SetByteArrayRegion(confirmationHash, 0, OOB_C_SIZE, reinterpret_cast<jbyte*>(oob_data.c));

  jbyteArray address = env->NewByteArray(OOB_ADDRESS_SIZE);
  env->SetByteArrayRegion(address, 0, OOB_ADDRESS_SIZE, reinterpret_cast<jbyte*>(oob_data.address));

  jint le_role = (jint)oob_data.le_device_role;

  jclass leBuilderClass = env->FindClass("android/bluetooth/OobData$LeBuilder");

  jobject oobDataLeBuilder =
          env->NewObject(leBuilderClass, leBuilderConstructor, confirmationHash, address, le_role);

  env->DeleteLocalRef(leBuilderClass);

  jbyteArray randomizerHash = env->NewByteArray(OOB_R_SIZE);
  env->SetByteArrayRegion(randomizerHash, 0, OOB_R_SIZE, reinterpret_cast<jbyte*>(oob_data.r));

  oobDataLeBuilder = env->CallObjectMethod(oobDataLeBuilder, setRMethod, randomizerHash);

  int name_char_count = 0;
  for (int i = 0; i < OOB_NAME_MAX_SIZE; i++) {
    if (oob_data.device_name[i] == 0) {
      name_char_count = i;
      break;
    }
  }

  jbyteArray deviceName = env->NewByteArray(name_char_count);
  env->SetByteArrayRegion(deviceName, 0, name_char_count,
                          reinterpret_cast<jbyte*>(oob_data.device_name));

  oobDataLeBuilder = env->CallObjectMethod(oobDataLeBuilder, setNameMethod, deviceName);

  return env->CallObjectMethod(oobDataLeBuilder, buildMethod);
}

static void generate_local_oob_data_callback(tBT_TRANSPORT transport, bt_oob_data_t oob_data) {
  log::verbose("");

  std::shared_lock<std::shared_timed_mutex> lock(jniObjMutex);
  if (!sJniCallbacksObj) {
    log::error("JNI obj is null. Failed to call JNI callback");
    return;
  }

  CallbackEnv sCallbackEnv(__func__);
  if (!sCallbackEnv.valid()) {
    return;
  }

  if (transport == BT_TRANSPORT_BR_EDR) {
    sCallbackEnv->CallVoidMethod(
            sJniCallbacksObj, method_oobDataReceivedCallback, (jint)transport,
            ((oob_data.is_valid) ? createClassicOobDataObject(sCallbackEnv.get(), oob_data)
                                 : nullptr));
  } else if (transport == BT_TRANSPORT_LE) {
    sCallbackEnv->CallVoidMethod(
            sJniCallbacksObj, method_oobDataReceivedCallback, (jint)transport,
            ((oob_data.is_valid) ? createLeOobDataObject(sCallbackEnv.get(), oob_data) : nullptr));
  } else {
    // TRANSPORT_AUTO is a concept, however, the host stack doesn't fully
    // implement it So passing it from the java layer is currently useless until
    // the implementation and concept of TRANSPORT_AUTO is fleshed out.
    log::error("TRANSPORT: {} not implemented", transport);
    sCallbackEnv->CallVoidMethod(sJniCallbacksObj, method_oobDataReceivedCallback, (jint)transport,
                                 nullptr);
  }
}

static void link_quality_report_callback(uint64_t timestamp, int report_id, int rssi, int snr,
                                         int retransmission_count, int packets_not_receive_count,
                                         int negative_acknowledgement_count) {
  std::shared_lock<std::shared_timed_mutex> lock(jniObjMutex);
  if (!sJniCallbacksObj) {
    log::error("JNI obj is null. Failed to call JNI callback");
    return;
  }

  CallbackEnv sCallbackEnv(__func__);
  if (!sCallbackEnv.valid()) {
    return;
  }

  log::verbose("LinkQualityReportCallback: {} {} {} {} {} {}", report_id, rssi, snr,
               retransmission_count, packets_not_receive_count, negative_acknowledgement_count);

  sCallbackEnv->CallVoidMethod(sJniCallbacksObj, method_linkQualityReportCallback, (jlong)timestamp,
                               (jint)report_id, (jint)rssi, (jint)snr, (jint)retransmission_count,
                               (jint)packets_not_receive_count,
                               (jint)negative_acknowledgement_count);
}

static void switch_buffer_size_callback(bool is_low_latency_buffer_size) {
  std::shared_lock<std::shared_timed_mutex> lock(jniObjMutex);
  if (!sJniCallbacksObj) {
    log::error("JNI obj is null. Failed to call JNI callback");
    return;
  }

  CallbackEnv sCallbackEnv(__func__);
  if (!sCallbackEnv.valid()) {
    return;
  }

  log::verbose("SwitchBufferSizeCallback: {}", is_low_latency_buffer_size);

  sCallbackEnv->CallVoidMethod(sJniCallbacksObj, method_switchBufferSizeCallback,
                               (jboolean)is_low_latency_buffer_size);
}

static void switch_codec_callback(bool is_low_latency_buffer_size) {
  std::shared_lock<std::shared_timed_mutex> lock(jniObjMutex);
  if (!sJniCallbacksObj) {
    log::error("JNI obj is null. Failed to call JNI callback");
    return;
  }

  CallbackEnv sCallbackEnv(__func__);
  if (!sCallbackEnv.valid()) {
    return;
  }

  log::verbose("SwitchCodecCallback: {}", is_low_latency_buffer_size);

  sCallbackEnv->CallVoidMethod(sJniCallbacksObj, method_switchCodecCallback,
                               (jboolean)is_low_latency_buffer_size);
}

static void le_rand_callback(uint64_t /* random */) {
  // Android doesn't support the LeRand API.
}

static void key_missing_callback(const RawAddress bd_addr) {
  std::shared_lock<std::shared_timed_mutex> lock(jniObjMutex);
  if (!sJniCallbacksObj) {
    log::error("JNI obj is null. Failed to call JNI callback");
    return;
  }

  CallbackEnv sCallbackEnv(__func__);
  if (!sCallbackEnv.valid()) {
    return;
  }

  ScopedLocalRef<jbyteArray> addr(sCallbackEnv.get(),
                                  sCallbackEnv->NewByteArray(sizeof(RawAddress)));
  if (!addr.get()) {
    log::error("Address allocation failed");
    return;
  }
  sCallbackEnv->SetByteArrayRegion(addr.get(), 0, sizeof(RawAddress),
                                   reinterpret_cast<jbyte*>(const_cast<RawAddress*>(&bd_addr)));

  sCallbackEnv->CallVoidMethod(sJniCallbacksObj, method_keyMissingCallback, addr.get());
}

static void encryption_change_callback(const bt_encryption_change_evt encryption_change) {
  std::shared_lock<std::shared_timed_mutex> lock(jniObjMutex);
  if (!sJniCallbacksObj) {
    log::error("JNI obj is null. Failed to call JNI callback");
    return;
  }

  CallbackEnv sCallbackEnv(__func__);
  if (!sCallbackEnv.valid()) {
    return;
  }

  ScopedLocalRef<jbyteArray> addr(sCallbackEnv.get(),
                                  sCallbackEnv->NewByteArray(sizeof(RawAddress)));
  if (!addr.get()) {
    log::error("Address allocation failed");
    return;
  }
  sCallbackEnv->SetByteArrayRegion(
          addr.get(), 0, sizeof(RawAddress),
          reinterpret_cast<jbyte*>(const_cast<RawAddress*>(&encryption_change.bd_addr)));

  sCallbackEnv->CallVoidMethod(sJniCallbacksObj, method_encryptionChangeCallback, addr.get(),
                               encryption_change.status, encryption_change.encr_enable,
                               encryption_change.transport, encryption_change.secure_connections,
                               encryption_change.key_size);
}

static void callback_thread_event(bt_cb_thread_evt event) {
  if (event == ASSOCIATE_JVM) {
    JavaVMAttachArgs args;
    char name[] = "BT Service Callback Thread";
    args.version = JNI_VERSION_1_6;
    args.name = name;
    args.group = NULL;
    vm->AttachCurrentThread(&callbackEnv, &args);
    sHaveCallbackThread = true;
    sCallbackThread = pthread_self();
    log::verbose("Callback thread attached: {}", std::format_ptr(callbackEnv));
  } else if (event == DISASSOCIATE_JVM) {
    if (!isCallbackThread()) {
      log::error("Callback: '' is not called on the correct thread");
      return;
    }
    vm->DetachCurrentThread();
    sHaveCallbackThread = false;
    callbackEnv = NULL;
  }
}

static void dut_mode_recv_callback(uint16_t /* opcode */, uint8_t* /* buf */, uint8_t /* len */) {}

static void le_test_mode_recv_callback(bt_status_t status, uint16_t packet_count) {
  log::verbose("status:{} packet_count:{}", bt_status_text(status), packet_count);
}

static void energy_info_recv_callback(bt_activity_energy_info* p_energy_info,
                                      bt_uid_traffic_t* uid_data) {
  std::shared_lock<std::shared_timed_mutex> lock(jniObjMutex);
  if (!sJniAdapterServiceObj) {
    log::error("JNI obj is null. Failed to call JNI callback");
    return;
  }

  CallbackEnv sCallbackEnv(__func__);
  if (!sCallbackEnv.valid()) {
    return;
  }

  jsize len = 0;
  for (bt_uid_traffic_t* data = uid_data; data->app_uid != -1; data++) {
    len++;
  }

  ScopedLocalRef<jobjectArray> array(
          sCallbackEnv.get(),
          sCallbackEnv->NewObjectArray(len, android_bluetooth_UidTraffic.clazz, NULL));
  jsize i = 0;
  for (bt_uid_traffic_t* data = uid_data; data->app_uid != -1; data++) {
    ScopedLocalRef<jobject> uidObj(
            sCallbackEnv.get(),
            sCallbackEnv->NewObject(android_bluetooth_UidTraffic.clazz,
                                    android_bluetooth_UidTraffic.constructor, (jint)data->app_uid,
                                    (jlong)data->rx_bytes, (jlong)data->tx_bytes));
    sCallbackEnv->SetObjectArrayElement(array.get(), i++, uidObj.get());
  }

  sCallbackEnv->CallVoidMethod(sJniCallbacksObj, method_energyInfo, p_energy_info->status,
                               p_energy_info->ctrl_state, p_energy_info->tx_time,
                               p_energy_info->rx_time, p_energy_info->idle_time,
                               p_energy_info->energy_used, array.get());
}

static bt_callbacks_t sBluetoothCallbacks = {
        sizeof(sBluetoothCallbacks),
        adapter_state_change_callback,
        adapter_properties_callback,
        remote_device_properties_callback,
        device_found_callback,
        discovery_state_changed_callback,
        pin_request_callback,
        ssp_request_callback,
        bond_state_changed_callback,
        address_consolidate_callback,
        le_address_associate_callback,
        acl_state_changed_callback,
        callback_thread_event,
        dut_mode_recv_callback,
        le_test_mode_recv_callback,
        energy_info_recv_callback,
        link_quality_report_callback,
        generate_local_oob_data_callback,
        switch_buffer_size_callback,
        switch_codec_callback,
        le_rand_callback,
        key_missing_callback,
        encryption_change_callback,
};

class JNIThreadAttacher {
public:
  explicit JNIThreadAttacher(JavaVM* vm) : vm_(vm), env_(nullptr) {
    status_ = vm_->GetEnv(reinterpret_cast<void**>(&env_), JNI_VERSION_1_6);

    if (status_ != JNI_OK && status_ != JNI_EDETACHED) {
      log::error(
              "JNIThreadAttacher: unable to get environment for JNI CALL, status: "
              "{}",
              status_);
      env_ = nullptr;
      return;
    }

    if (status_ == JNI_EDETACHED) {
      char name[17] = {0};
      if (prctl(PR_GET_NAME, (unsigned long)name) != 0) {  // NOLINT: prctl take a long
        log::error("JNIThreadAttacher: unable to grab previous thread name, error: {}",
                   strerror(errno));
        env_ = nullptr;
        return;
      }

      JavaVMAttachArgs args = {.version = JNI_VERSION_1_6, .name = name, .group = nullptr};
      if (vm_->AttachCurrentThread(&env_, &args) != 0) {
        log::error("JNIThreadAttacher: unable to attach thread to VM");
        env_ = nullptr;
        return;
      }
    }
  }

  ~JNIThreadAttacher() {
    if (status_ == JNI_EDETACHED) {
      vm_->DetachCurrentThread();
    }
  }

  JNIEnv* getEnv() { return env_; }

private:
  JavaVM* vm_;
  JNIEnv* env_;
  jint status_;
};

static int acquire_wake_lock_callout(const char* lock_name) {
  std::shared_lock<std::shared_timed_mutex> lock(jniObjMutex);
  if (!sJniAdapterServiceObj) {
    log::error("JNI obj is null. Failed to call JNI callback");
    return BT_STATUS_NOT_READY;
  }

  JNIThreadAttacher attacher(vm);
  JNIEnv* env = attacher.getEnv();

  if (env == nullptr) {
    log::error("Unable to get JNI Env");
    return BT_STATUS_JNI_THREAD_ATTACH_ERROR;
  }

  jint ret = BT_STATUS_SUCCESS;
  {
    ScopedLocalRef<jstring> lock_name_jni(env, env->NewStringUTF(lock_name));
    if (lock_name_jni.get()) {
      bool acquired =
              env->CallBooleanMethod(sJniCallbacksObj, method_acquireWakeLock, lock_name_jni.get());
      if (!acquired) {
        ret = BT_STATUS_WAKELOCK_ERROR;
      }
    } else {
      log::error("unable to allocate string: {}", lock_name);
      ret = BT_STATUS_NOMEM;
    }
  }

  return ret;
}

static int release_wake_lock_callout(const char* lock_name) {
  std::shared_lock<std::shared_timed_mutex> lock(jniObjMutex);
  if (!sJniAdapterServiceObj) {
    log::error("JNI obj is null. Failed to call JNI callback");
    return BT_STATUS_NOT_READY;
  }

  JNIThreadAttacher attacher(vm);
  JNIEnv* env = attacher.getEnv();

  if (env == nullptr) {
    log::error("Unable to get JNI Env");
    return BT_STATUS_JNI_THREAD_ATTACH_ERROR;
  }

  jint ret = BT_STATUS_SUCCESS;
  {
    ScopedLocalRef<jstring> lock_name_jni(env, env->NewStringUTF(lock_name));
    if (lock_name_jni.get()) {
      bool released =
              env->CallBooleanMethod(sJniCallbacksObj, method_releaseWakeLock, lock_name_jni.get());
      if (!released) {
        ret = BT_STATUS_WAKELOCK_ERROR;
      }
    } else {
      log::error("unable to allocate string: {}", lock_name);
      ret = BT_STATUS_NOMEM;
    }
  }

  return ret;
}

static bt_os_callouts_t sBluetoothOsCallouts = {
        sizeof(sBluetoothOsCallouts),
        acquire_wake_lock_callout,
        release_wake_lock_callout,
};

int hal_util_load_bt_library(const bt_interface_t** interface) {
  *interface = &bluetoothInterface;
  return 0;
}

static bool initNative(JNIEnv* env, jobject obj, jboolean isGuest, jboolean isCommonCriteriaMode,
                       int configCompareResult, jboolean isAtvDevice) {
  std::unique_lock<std::shared_timed_mutex> lock(jniObjMutex);

  log::verbose("");

  android_bluetooth_UidTraffic.clazz =
          (jclass)env->NewGlobalRef(env->FindClass("android/bluetooth/UidTraffic"));

  sJniAdapterServiceObj = env->NewGlobalRef(obj);
  sJniCallbacksObj = env->NewGlobalRef(env->GetObjectField(obj, sJniCallbacksField));

  if (!sBluetoothInterface) {
    return JNI_FALSE;
  }

  int ret =
          sBluetoothInterface->init(&sBluetoothCallbacks, isGuest == JNI_TRUE ? 1 : 0,
                                    isCommonCriteriaMode == JNI_TRUE ? 1 : 0, configCompareResult,
                                    isAtvDevice == JNI_TRUE ? 1 : 0);

  if (ret != BT_STATUS_SUCCESS) {
    log::error("Error while setting the callbacks: {}", ret);
    sBluetoothInterface = NULL;
    return JNI_FALSE;
  }
  ret = sBluetoothInterface->set_os_callouts(&sBluetoothOsCallouts);
  if (ret != BT_STATUS_SUCCESS) {
    log::error("Error while setting Bluetooth callouts: {}", ret);
    sBluetoothInterface->cleanup();
    sBluetoothInterface = NULL;
    return JNI_FALSE;
  }

  sBluetoothSocketInterface = reinterpret_cast<const btsock_interface_t*>(
          sBluetoothInterface->get_profile_interface(BT_PROFILE_SOCKETS_ID));
  if (sBluetoothSocketInterface == NULL) {
    log::error("Error getting socket interface");
  }

  return JNI_TRUE;
}

static bool cleanupNative(JNIEnv* env, jobject /* obj */) {
  std::unique_lock<std::shared_timed_mutex> lock(jniObjMutex);

  log::verbose("");

  if (!sBluetoothInterface) {
    return JNI_FALSE;
  }

  sBluetoothInterface->cleanup();
  log::info("return from cleanup");

  if (sJniCallbacksObj) {
    env->DeleteGlobalRef(sJniCallbacksObj);
    sJniCallbacksObj = NULL;
  }

  if (sJniAdapterServiceObj) {
    env->DeleteGlobalRef(sJniAdapterServiceObj);
    sJniAdapterServiceObj = NULL;
  }

  if (android_bluetooth_UidTraffic.clazz) {
    env->DeleteGlobalRef(android_bluetooth_UidTraffic.clazz);
    android_bluetooth_UidTraffic.clazz = NULL;
  }
  return JNI_TRUE;
}

static jboolean enableNative(JNIEnv* /* env */, jobject /* obj */) {
  log::verbose("");

  if (!sBluetoothInterface) {
    return JNI_FALSE;
  }
  int ret = sBluetoothInterface->enable();
  return (ret == BT_STATUS_SUCCESS || ret == BT_STATUS_DONE) ? JNI_TRUE : JNI_FALSE;
}

static jboolean disableNative(JNIEnv* /* env */, jobject /* obj */) {
  log::verbose("");

  if (!sBluetoothInterface) {
    return JNI_FALSE;
  }

  int ret = sBluetoothInterface->disable();
  /* Retrun JNI_FALSE only when BTIF explicitly reports
     BT_STATUS_FAIL. It is fine for the BT_STATUS_NOT_READY
     case which indicates that stack had not been enabled.
  */
  return (ret == BT_STATUS_FAIL) ? JNI_FALSE : JNI_TRUE;
}

static jboolean startDiscoveryNative(JNIEnv* /* env */, jobject /* obj */) {
  log::verbose("");

  if (!sBluetoothInterface) {
    return JNI_FALSE;
  }

  int ret = sBluetoothInterface->start_discovery();
  return (ret == BT_STATUS_SUCCESS) ? JNI_TRUE : JNI_FALSE;
}

static jboolean cancelDiscoveryNative(JNIEnv* /* env */, jobject /* obj */) {
  log::verbose("");

  if (!sBluetoothInterface) {
    return JNI_FALSE;
  }

  int ret = sBluetoothInterface->cancel_discovery();
  return (ret == BT_STATUS_SUCCESS) ? JNI_TRUE : JNI_FALSE;
}

static jboolean createBondNative(JNIEnv* env, jobject /* obj */, jbyteArray address, jint addrType,
                                 jint transport) {
  log::verbose("");

  if (!sBluetoothInterface) {
    return JNI_FALSE;
  }

  jbyte* addr = env->GetByteArrayElements(address, NULL);
  if (addr == NULL) {
    jniThrowIOException(env, EINVAL);
    return JNI_FALSE;
  }

  uint8_t addr_type = (uint8_t)addrType;
  int ret = BT_STATUS_SUCCESS;
  if (addr_type == BLE_ADDR_RANDOM) {
    ret = sBluetoothInterface->create_bond_le(reinterpret_cast<RawAddress*>(addr), addr_type);
  } else {
    ret = sBluetoothInterface->create_bond(reinterpret_cast<RawAddress*>(addr), transport);
  }

  if (ret != BT_STATUS_SUCCESS) {
    log::warn("Failed to initiate bonding. Status = {}", ret);
  }

  env->ReleaseByteArrayElements(address, addr, 0);
  return (ret == BT_STATUS_SUCCESS) ? JNI_TRUE : JNI_FALSE;
}

static jbyteArray callByteArrayGetter(JNIEnv* env, jobject object, const char* className,
                                      const char* methodName) {
  jclass myClass = env->FindClass(className);
  jmethodID myMethod = env->GetMethodID(myClass, methodName, "()[B");
  env->DeleteLocalRef(myClass);
  return (jbyteArray)env->CallObjectMethod(object, myMethod);
}

static jint callIntGetter(JNIEnv* env, jobject object, const char* className,
                          const char* methodName) {
  jclass myClass = env->FindClass(className);
  jmethodID myMethod = env->GetMethodID(myClass, methodName, "()I");
  env->DeleteLocalRef(myClass);
  return env->CallIntMethod(object, myMethod);
}

static jboolean set_data(JNIEnv* env, jobject oobData, jint transport, bt_oob_data_t* oob_data) {
  // Need both arguments to be non NULL
  if (oobData == NULL) {
    log::error("oobData is null! Nothing to do.");
    return JNI_FALSE;
  }

  log::assert_that(oob_data != nullptr, "oob_data is never null");

  jbyteArray address = callByteArrayGetter(env, oobData, "android/bluetooth/OobData",
                                           "getDeviceAddressWithType");

  // Check the data
  int len = env->GetArrayLength(address);
  if (len != OOB_ADDRESS_SIZE) {
    log::error("addressBytes must be 7 bytes in length (address plus type) 6+1!");
    jniThrowIOException(env, EINVAL);
    return JNI_FALSE;
  }

  // Convert the address from byte[]
  {
    jbyte* addressBytes = env->GetByteArrayElements(address, NULL);
    if (addressBytes == NULL) {
      log::error("addressBytes cannot be null!");
      jniThrowIOException(env, EINVAL);
      return JNI_FALSE;
    }
    memcpy(oob_data->address, addressBytes, len);
    env->ReleaseByteArrayElements(address, addressBytes, 0);
  }

  // Get the device name byte[] java object
  jbyteArray deviceName =
          callByteArrayGetter(env, oobData, "android/bluetooth/OobData", "getDeviceName");

  // Optional
  // Convert it to a jbyte* and copy it to the struct
  if (deviceName != NULL) {
    jbyte* deviceNameBytes = env->GetByteArrayElements(deviceName, NULL);
    int len = env->GetArrayLength(deviceName);
    if (len > OOB_NAME_MAX_SIZE) {
      log::info(
              "wrong length of deviceName, should be empty or less than or equal "
              "to {} bytes.",
              OOB_NAME_MAX_SIZE);
      jniThrowIOException(env, EINVAL);
      env->ReleaseByteArrayElements(deviceName, deviceNameBytes, 0);
      return JNI_FALSE;
    }
    memcpy(oob_data->device_name, deviceNameBytes, len);
    env->ReleaseByteArrayElements(deviceName, deviceNameBytes, 0);
  }
  // Used by both classic and LE
  jbyteArray confirmation =
          callByteArrayGetter(env, oobData, "android/bluetooth/OobData", "getConfirmationHash");
  if (confirmation == NULL) {
    log::error("confirmation cannot be null!");
    jniThrowIOException(env, EINVAL);
    return JNI_FALSE;
  }

  // Confirmation is mandatory
  jbyte* confirmationBytes = env->GetByteArrayElements(confirmation, NULL);
  len = env->GetArrayLength(confirmation);
  if (confirmationBytes == NULL || len != OOB_C_SIZE) {
    log::info("wrong length of Confirmation, should be empty or {} bytes.", OOB_C_SIZE);
    jniThrowIOException(env, EINVAL);
    env->ReleaseByteArrayElements(confirmation, confirmationBytes, 0);
    return JNI_FALSE;
  }
  memcpy(oob_data->c, confirmationBytes, OOB_C_SIZE);
  env->ReleaseByteArrayElements(confirmation, confirmationBytes, 0);

  // Random is supposedly optional according to the specification
  jbyteArray randomizer =
          callByteArrayGetter(env, oobData, "android/bluetooth/OobData", "getRandomizerHash");
  if (randomizer != NULL) {
    jbyte* randomizerBytes = env->GetByteArrayElements(randomizer, NULL);
    int len = env->GetArrayLength(randomizer);
    if (randomizerBytes == NULL || len != OOB_R_SIZE) {
      log::info("wrong length of Random, should be empty or {} bytes.", OOB_R_SIZE);
      jniThrowIOException(env, EINVAL);
      env->ReleaseByteArrayElements(randomizer, randomizerBytes, 0);
      return JNI_FALSE;
    }
    memcpy(oob_data->r, randomizerBytes, OOB_R_SIZE);
    env->ReleaseByteArrayElements(randomizer, randomizerBytes, 0);
  }

  // Transport specific data fetching/setting
  if (transport == BT_TRANSPORT_BR_EDR) {
    // Classic
    // Not optional
    jbyteArray oobDataLength =
            callByteArrayGetter(env, oobData, "android/bluetooth/OobData", "getClassicLength");
    if (oobDataLength == NULL || env->GetArrayLength(oobDataLength) != OOB_DATA_LEN_SIZE) {
      log::info("wrong length of oobDataLength, should be empty or {} bytes.", OOB_DATA_LEN_SIZE);
      jniThrowIOException(env, EINVAL);
      return JNI_FALSE;
    }

    jbyte* oobDataLengthBytes = env->GetByteArrayElements(oobDataLength, NULL);
    memcpy(oob_data->oob_data_length, oobDataLengthBytes, OOB_DATA_LEN_SIZE);
    env->ReleaseByteArrayElements(oobDataLength, oobDataLengthBytes, 0);

    // Optional
    jbyteArray classOfDevice =
            callByteArrayGetter(env, oobData, "android/bluetooth/OobData", "getClassOfDevice");
    if (classOfDevice != NULL) {
      jbyte* classOfDeviceBytes = env->GetByteArrayElements(classOfDevice, NULL);
      int len = env->GetArrayLength(classOfDevice);
      if (len != OOB_COD_SIZE) {
        log::info("wrong length of classOfDevice, should be empty or {} bytes.", OOB_COD_SIZE);
        jniThrowIOException(env, EINVAL);
        env->ReleaseByteArrayElements(classOfDevice, classOfDeviceBytes, 0);
        return JNI_FALSE;
      }
      memcpy(oob_data->class_of_device, classOfDeviceBytes, OOB_COD_SIZE);
      env->ReleaseByteArrayElements(classOfDevice, classOfDeviceBytes, 0);
    }
  } else if (transport == BT_TRANSPORT_LE) {
    // LE
    jbyteArray temporaryKey =
            callByteArrayGetter(env, oobData, "android/bluetooth/OobData", "getLeTemporaryKey");
    if (temporaryKey != NULL) {
      jbyte* temporaryKeyBytes = env->GetByteArrayElements(temporaryKey, NULL);
      int len = env->GetArrayLength(temporaryKey);
      if (len != OOB_TK_SIZE) {
        log::info("wrong length of temporaryKey, should be empty or {} bytes.", OOB_TK_SIZE);
        jniThrowIOException(env, EINVAL);
        env->ReleaseByteArrayElements(temporaryKey, temporaryKeyBytes, 0);
        return JNI_FALSE;
      }
      memcpy(oob_data->sm_tk, temporaryKeyBytes, OOB_TK_SIZE);
      env->ReleaseByteArrayElements(temporaryKey, temporaryKeyBytes, 0);
    }

    jbyteArray leAppearance =
            callByteArrayGetter(env, oobData, "android/bluetooth/OobData", "getLeAppearance");
    if (leAppearance != NULL) {
      jbyte* leAppearanceBytes = env->GetByteArrayElements(leAppearance, NULL);
      int len = env->GetArrayLength(leAppearance);
      if (len != OOB_LE_APPEARANCE_SIZE) {
        log::info("wrong length of leAppearance, should be empty or {} bytes.",
                  OOB_LE_APPEARANCE_SIZE);
        jniThrowIOException(env, EINVAL);
        env->ReleaseByteArrayElements(leAppearance, leAppearanceBytes, 0);
        return JNI_FALSE;
      }
      memcpy(oob_data->sm_tk, leAppearanceBytes, OOB_LE_APPEARANCE_SIZE);
      env->ReleaseByteArrayElements(leAppearance, leAppearanceBytes, 0);
    }

    jint leRole = callIntGetter(env, oobData, "android/bluetooth/OobData", "getLeDeviceRole");
    oob_data->le_device_role = leRole;

    jint leFlag = callIntGetter(env, oobData, "android/bluetooth/OobData", "getLeFlags");
    oob_data->le_flags = leFlag;
  }
  return JNI_TRUE;
}

static void generateLocalOobDataNative(JNIEnv* /* env */, jobject /* obj */, jint transport) {
  // No BT interface? Can't do anything.
  if (!sBluetoothInterface) {
    return;
  }

  tBT_TRANSPORT bt_transport = to_bt_transport(transport);

  if (sBluetoothInterface->generate_local_oob_data(bt_transport) != BT_STATUS_SUCCESS) {
    log::error("Call to generate_local_oob_data failed!");
    bt_oob_data_t oob_data = {
            .is_valid = false,
    };
    generate_local_oob_data_callback(bt_transport, oob_data);
  }
}  // namespace android

static jboolean createBondOutOfBandNative(JNIEnv* env, jobject /* obj */, jbyteArray address,
                                          jint transport, jobject p192Data, jobject p256Data) {
  // No BT interface? Can't do anything.
  if (!sBluetoothInterface) {
    return JNI_FALSE;
  }

  // No data? Can't do anything
  if (p192Data == NULL && p256Data == NULL) {
    log::error("All OOB Data are null! Nothing to do.");
    jniThrowIOException(env, EINVAL);
    return JNI_FALSE;
  }

  // This address is already reversed which is why its being passed...
  // In the future we want to remove this and just reverse the address
  // for the oobdata in the host stack.
  if (address == NULL) {
    log::error("Address cannot be null! Nothing to do.");
    jniThrowIOException(env, EINVAL);
    return JNI_FALSE;
  }

  // Check the data
  int len = env->GetArrayLength(address);
  if (len != 6) {
    log::error("addressBytes must be 6 bytes in length (address plus type) 6+1!");
    jniThrowIOException(env, EINVAL);
    return JNI_FALSE;
  }

  jbyte* addr = env->GetByteArrayElements(address, NULL);
  if (addr == NULL) {
    jniThrowIOException(env, EINVAL);
    return JNI_FALSE;
  }

  RawAddress addr_obj = {};
  addr_obj.FromOctets(reinterpret_cast<uint8_t*>(addr));
  env->ReleaseByteArrayElements(address, addr, 0);

  // Convert P192 data from Java POJO to C Struct
  bt_oob_data_t p192_data = {};
  if (p192Data != NULL) {
    if (set_data(env, p192Data, transport, &p192_data) == JNI_FALSE) {
      jniThrowIOException(env, EINVAL);
      return JNI_FALSE;
    }
  }

  // Convert P256 data from Java POJO to C Struct
  bt_oob_data_t p256_data = {};
  if (p256Data != NULL) {
    if (set_data(env, p256Data, transport, &p256_data) == JNI_FALSE) {
      jniThrowIOException(env, EINVAL);
      return JNI_FALSE;
    }
  }

  return ((sBluetoothInterface->create_bond_out_of_band(&addr_obj, transport, &p192_data,
                                                        &p256_data)) == BT_STATUS_SUCCESS)
                 ? JNI_TRUE
                 : JNI_FALSE;
}

static jboolean removeBondNative(JNIEnv* env, jobject /* obj */, jbyteArray address) {
  log::verbose("");

  if (!sBluetoothInterface) {
    return JNI_FALSE;
  }

  jbyte* addr = env->GetByteArrayElements(address, NULL);
  if (addr == NULL) {
    jniThrowIOException(env, EINVAL);
    return JNI_FALSE;
  }

  int ret = sBluetoothInterface->remove_bond(reinterpret_cast<RawAddress*>(addr));
  env->ReleaseByteArrayElements(address, addr, 0);

  return (ret == BT_STATUS_SUCCESS) ? JNI_TRUE : JNI_FALSE;
}

static jboolean cancelBondNative(JNIEnv* env, jobject /* obj */, jbyteArray address) {
  log::verbose("");

  if (!sBluetoothInterface) {
    return JNI_FALSE;
  }

  jbyte* addr = env->GetByteArrayElements(address, NULL);
  if (addr == NULL) {
    jniThrowIOException(env, EINVAL);
    return JNI_FALSE;
  }

  int ret = sBluetoothInterface->cancel_bond(reinterpret_cast<RawAddress*>(addr));
  env->ReleaseByteArrayElements(address, addr, 0);
  return (ret == BT_STATUS_SUCCESS) ? JNI_TRUE : JNI_FALSE;
}

static jboolean pairingIsBusyNative(JNIEnv* /*env*/, jobject /* obj */) {
  log::verbose("");

  if (!sBluetoothInterface) {
    return JNI_FALSE;
  }

  return sBluetoothInterface->pairing_is_busy();
}

static int getConnectionStateNative(JNIEnv* env, jobject /* obj */, jbyteArray address) {
  log::verbose("");
  if (!sBluetoothInterface) {
    return JNI_FALSE;
  }

  jbyte* addr = env->GetByteArrayElements(address, NULL);
  if (addr == NULL) {
    jniThrowIOException(env, EINVAL);
    return JNI_FALSE;
  }

  int ret = sBluetoothInterface->get_connection_state(reinterpret_cast<RawAddress*>(addr));
  env->ReleaseByteArrayElements(address, addr, 0);

  return ret;
}

static jboolean pinReplyNative(JNIEnv* env, jobject /* obj */, jbyteArray address, jboolean accept,
                               jint len, jbyteArray pinArray) {
  log::verbose("");

  if (!sBluetoothInterface) {
    return JNI_FALSE;
  }

  jbyte* addr = env->GetByteArrayElements(address, NULL);
  if (addr == NULL) {
    jniThrowIOException(env, EINVAL);
    return JNI_FALSE;
  }

  jbyte* pinPtr = NULL;
  if (accept) {
    pinPtr = env->GetByteArrayElements(pinArray, NULL);
    if (pinPtr == NULL) {
      jniThrowIOException(env, EINVAL);
      env->ReleaseByteArrayElements(address, addr, 0);
      return JNI_FALSE;
    }
  }

  int ret = sBluetoothInterface->pin_reply(reinterpret_cast<RawAddress*>(addr), accept, len,
                                           reinterpret_cast<bt_pin_code_t*>(pinPtr));
  env->ReleaseByteArrayElements(address, addr, 0);
  env->ReleaseByteArrayElements(pinArray, pinPtr, 0);

  return (ret == BT_STATUS_SUCCESS) ? JNI_TRUE : JNI_FALSE;
}

static jboolean sspReplyNative(JNIEnv* env, jobject /* obj */, jbyteArray address, jint type,
                               jboolean accept, jint passkey) {
  log::verbose("");

  if (!sBluetoothInterface) {
    return JNI_FALSE;
  }

  jbyte* addr = env->GetByteArrayElements(address, NULL);
  if (addr == NULL) {
    jniThrowIOException(env, EINVAL);
    return JNI_FALSE;
  }

  int ret = sBluetoothInterface->ssp_reply(reinterpret_cast<RawAddress*>(addr),
                                           (bt_ssp_variant_t)type, accept, passkey);
  env->ReleaseByteArrayElements(address, addr, 0);

  return (ret == BT_STATUS_SUCCESS) ? JNI_TRUE : JNI_FALSE;
}

static jboolean setScanModeNative(JNIEnv* /* env */, jobject /* obj */, jint mode) {
  log::verbose("");

  if (!sBluetoothInterface) {
    return JNI_FALSE;
  }

  sBluetoothInterface->set_scan_mode((bt_scan_mode_t)mode);
  return JNI_TRUE;
}

static jboolean setAdapterPropertyNative(JNIEnv* env, jobject /* obj */, jint type,
                                         jbyteArray value) {
  log::verbose("");

  if (!sBluetoothInterface) {
    return JNI_FALSE;
  }

  jbyte* val = env->GetByteArrayElements(value, NULL);
  bt_property_t prop;
  prop.type = (bt_property_type_t)type;
  prop.len = env->GetArrayLength(value);
  prop.val = val;

  int ret = sBluetoothInterface->set_adapter_property(&prop);
  env->ReleaseByteArrayElements(value, val, 0);

  return (ret == BT_STATUS_SUCCESS) ? JNI_TRUE : JNI_FALSE;
}

static jboolean getAdapterPropertiesNative(JNIEnv* /* env */, jobject /* obj */) {
  log::verbose("");

  if (!sBluetoothInterface) {
    return JNI_FALSE;
  }

  int ret = sBluetoothInterface->get_adapter_properties();
  return (ret == BT_STATUS_SUCCESS) ? JNI_TRUE : JNI_FALSE;
}

static jboolean getAdapterPropertyNative(JNIEnv* /* env */, jobject /* obj */, jint type) {
  log::verbose("");

  if (!sBluetoothInterface) {
    return JNI_FALSE;
  }

  int ret = sBluetoothInterface->get_adapter_property((bt_property_type_t)type);
  return (ret == BT_STATUS_SUCCESS) ? JNI_TRUE : JNI_FALSE;
}

static jboolean getDevicePropertyNative(JNIEnv* env, jobject /* obj */, jbyteArray address,
                                        jint type) {
  log::verbose("");

  if (!sBluetoothInterface) {
    return JNI_FALSE;
  }

  jbyte* addr = env->GetByteArrayElements(address, NULL);
  if (addr == NULL) {
    jniThrowIOException(env, EINVAL);
    return JNI_FALSE;
  }

  int ret = sBluetoothInterface->get_remote_device_property(reinterpret_cast<RawAddress*>(addr),
                                                            (bt_property_type_t)type);
  env->ReleaseByteArrayElements(address, addr, 0);
  return (ret == BT_STATUS_SUCCESS) ? JNI_TRUE : JNI_FALSE;
}

static jboolean setDevicePropertyNative(JNIEnv* env, jobject /* obj */, jbyteArray address,
                                        jint type, jbyteArray value) {
  log::verbose("");

  if (!sBluetoothInterface) {
    return JNI_FALSE;
  }

  jbyte* val = env->GetByteArrayElements(value, NULL);
  if (val == NULL) {
    jniThrowIOException(env, EINVAL);
    return JNI_FALSE;
  }

  jbyte* addr = env->GetByteArrayElements(address, NULL);
  if (addr == NULL) {
    env->ReleaseByteArrayElements(value, val, 0);
    jniThrowIOException(env, EINVAL);
    return JNI_FALSE;
  }

  bt_property_t prop;
  prop.type = (bt_property_type_t)type;
  prop.len = env->GetArrayLength(value);
  prop.val = val;

  int ret = sBluetoothInterface->set_remote_device_property(reinterpret_cast<RawAddress*>(addr),
                                                            &prop);
  env->ReleaseByteArrayElements(value, val, 0);
  env->ReleaseByteArrayElements(address, addr, 0);

  return (ret == BT_STATUS_SUCCESS) ? JNI_TRUE : JNI_FALSE;
}

static jboolean getRemoteServicesNative(JNIEnv* env, jobject /* obj */, jbyteArray address,
                                        jint transport) {
  log::verbose("");

  if (!sBluetoothInterface) {
    return JNI_FALSE;
  }

  jbyte* addr = addr = env->GetByteArrayElements(address, NULL);
  if (addr == NULL) {
    jniThrowIOException(env, EINVAL);
    return JNI_FALSE;
  }

  int ret =
          sBluetoothInterface->get_remote_services(reinterpret_cast<RawAddress*>(addr), transport);
  env->ReleaseByteArrayElements(address, addr, 0);
  return (ret == BT_STATUS_SUCCESS) ? JNI_TRUE : JNI_FALSE;
}

static int readEnergyInfoNative() {
  log::verbose("");

  if (!sBluetoothInterface) {
    return JNI_FALSE;
  }
  int ret = sBluetoothInterface->read_energy_info();
  return (ret == BT_STATUS_SUCCESS) ? JNI_TRUE : JNI_FALSE;
}

static void dumpNative(JNIEnv* env, jobject /* obj */, jobject fdObj, jobjectArray argArray) {
  log::verbose("");
  if (!sBluetoothInterface) {
    return;
  }

  int fd = jniGetFDFromFileDescriptor(env, fdObj);
  if (fd < 0) {
    return;
  }

  int numArgs = env->GetArrayLength(argArray);

  jstring* argObjs = new jstring[numArgs];
  const char** args = nullptr;
  if (numArgs > 0) {
    args = new const char*[numArgs + 1];
    args[numArgs] = nullptr;
  }

  for (int i = 0; i < numArgs; i++) {
    argObjs[i] = (jstring)env->GetObjectArrayElement(argArray, i);
    args[i] = env->GetStringUTFChars(argObjs[i], NULL);
  }

  sBluetoothInterface->dump(fd, args);

  for (int i = 0; i < numArgs; i++) {
    env->ReleaseStringUTFChars(argObjs[i], args[i]);
  }

  delete[] args;
  delete[] argObjs;
}

static jbyteArray dumpMetricsNative(JNIEnv* env, jobject /* obj */) {
  log::info("");
  if (!sBluetoothInterface) {
    return env->NewByteArray(0);
  }

  std::string output;
  sBluetoothInterface->dumpMetrics(&output);
  jsize output_size = output.size() * sizeof(char);
  jbyteArray output_bytes = env->NewByteArray(output_size);
  env->SetByteArrayRegion(output_bytes, 0, output_size,
                          reinterpret_cast<const jbyte*>(output.data()));
  return output_bytes;
}

static jboolean factoryResetNative(JNIEnv* /* env */, jobject /* obj */) {
  log::verbose("");
  if (!sBluetoothInterface) {
    return JNI_FALSE;
  }
  int ret = sBluetoothInterface->config_clear();
  return (ret == BT_STATUS_SUCCESS) ? JNI_TRUE : JNI_FALSE;
}

static jbyteArray obfuscateAddressNative(JNIEnv* env, jobject /* obj */, jbyteArray address) {
  log::verbose("");
  if (!sBluetoothInterface) {
    return env->NewByteArray(0);
  }
  jbyte* addr = env->GetByteArrayElements(address, nullptr);
  if (addr == nullptr) {
    jniThrowIOException(env, EINVAL);
    return env->NewByteArray(0);
  }
  RawAddress addr_obj = {};
  addr_obj.FromOctets(reinterpret_cast<uint8_t*>(addr));
  env->ReleaseByteArrayElements(address, addr, 0);
  std::string output = sBluetoothInterface->obfuscate_address(addr_obj);
  jsize output_size = output.size() * sizeof(char);
  jbyteArray output_bytes = env->NewByteArray(output_size);
  env->SetByteArrayRegion(output_bytes, 0, output_size,
                          reinterpret_cast<const jbyte*>(output.data()));
  return output_bytes;
}

static jboolean setBufferLengthMillisNative(JNIEnv* /* env */, jobject /* obj */, jint codec,
                                            jint size) {
  log::verbose("");

  if (!sBluetoothInterface) {
    return JNI_FALSE;
  }

  int ret = sBluetoothInterface->set_dynamic_audio_buffer_size(codec, size);
  return (ret == BT_STATUS_SUCCESS) ? JNI_TRUE : JNI_FALSE;
}

static jint connectSocketNative(JNIEnv* env, jobject /* obj */, jbyteArray address, jint type,
                                jbyteArray uuid, jint port, jint flag, jint callingUid,
                                jint dataPath, jstring socketName, jlong hubId, jlong endPointId,
                                jint maxRxPacketSize) {
  int socket_fd = INVALID_FD;
  jbyte* addr = nullptr;
  jbyte* uuidBytes = nullptr;
  Uuid btUuid;
  const char* nativeSocketName = nullptr;

  if (!sBluetoothSocketInterface) {
    goto done;
  }
  addr = env->GetByteArrayElements(address, nullptr);
  uuidBytes = env->GetByteArrayElements(uuid, nullptr);
  if (addr == nullptr || uuidBytes == nullptr) {
    jniThrowIOException(env, EINVAL);
    goto done;
  }

  btUuid = Uuid::From128BitBE(reinterpret_cast<uint8_t*>(uuidBytes));
  if (socketName != nullptr) {
    nativeSocketName = env->GetStringUTFChars(socketName, nullptr);
  }
  if (sBluetoothSocketInterface->connect(reinterpret_cast<RawAddress*>(addr), (btsock_type_t)type,
                                         &btUuid, port, &socket_fd, flag, callingUid,
                                         (btsock_data_path_t)dataPath, nativeSocketName, hubId,
                                         endPointId, maxRxPacketSize) != BT_STATUS_SUCCESS) {
    socket_fd = INVALID_FD;
  }

done:
  if (addr) {
    env->ReleaseByteArrayElements(address, addr, 0);
  }
  if (uuidBytes) {
    env->ReleaseByteArrayElements(uuid, uuidBytes, 0);
  }
  if (nativeSocketName) {
    env->ReleaseStringUTFChars(socketName, nativeSocketName);
  }
  return socket_fd;
}

static jint createSocketChannelNative(JNIEnv* env, jobject /* obj */, jint type,
                                      jstring serviceName, jbyteArray uuid, jint port, jint flag,
                                      jint callingUid, jint dataPath, jstring socketName,
                                      jlong hubId, jlong endPointId, jint maxRxPacketSize) {
  int socket_fd = INVALID_FD;
  jbyte* uuidBytes = nullptr;
  Uuid btUuid;
  const char* nativeServiceName = nullptr;
  const char* nativeSocketName = nullptr;

  if (!sBluetoothSocketInterface) {
    goto done;
  }
  uuidBytes = env->GetByteArrayElements(uuid, nullptr);
  if (serviceName != nullptr) {
    nativeServiceName = env->GetStringUTFChars(serviceName, nullptr);
  }
  if (uuidBytes == nullptr) {
    jniThrowIOException(env, EINVAL);
    goto done;
  }
  btUuid = Uuid::From128BitBE(reinterpret_cast<uint8_t*>(uuidBytes));
  if (socketName != nullptr) {
    nativeSocketName = env->GetStringUTFChars(socketName, nullptr);
  }

  if (sBluetoothSocketInterface->listen((btsock_type_t)type, nativeServiceName, &btUuid, port,
                                        &socket_fd, flag, callingUid, (btsock_data_path_t)dataPath,
                                        nativeSocketName, hubId, endPointId,
                                        maxRxPacketSize) != BT_STATUS_SUCCESS) {
    socket_fd = INVALID_FD;
  }

done:
  if (uuidBytes) {
    env->ReleaseByteArrayElements(uuid, uuidBytes, 0);
  }
  if (nativeServiceName) {
    env->ReleaseStringUTFChars(serviceName, nativeServiceName);
  }
  if (nativeSocketName) {
    env->ReleaseStringUTFChars(socketName, nativeSocketName);
  }
  return socket_fd;
}

static void requestMaximumTxDataLengthNative(JNIEnv* env, jobject /* obj */, jbyteArray address) {
  if (!sBluetoothSocketInterface) {
    return;
  }
  jbyte* addr = env->GetByteArrayElements(address, nullptr);
  if (addr == nullptr) {
    jniThrowIOException(env, EINVAL);
    return;
  }

  RawAddress addressVar = *reinterpret_cast<RawAddress*>(addr);
  sBluetoothSocketInterface->request_max_tx_data_length(addressVar);
  env->ReleaseByteArrayElements(address, addr, 1);
}

static int getMetricIdNative(JNIEnv* env, jobject /* obj */, jbyteArray address) {
  log::verbose("");
  if (!sBluetoothInterface) {
    return 0;  // 0 is invalid id
  }
  jbyte* addr = env->GetByteArrayElements(address, nullptr);
  if (addr == nullptr) {
    jniThrowIOException(env, EINVAL);
    return 0;
  }
  RawAddress addr_obj = {};
  addr_obj.FromOctets(reinterpret_cast<uint8_t*>(addr));
  env->ReleaseByteArrayElements(address, addr, 0);
  return sBluetoothInterface->get_metric_id(addr_obj);
}

static jboolean allowLowLatencyAudioNative(JNIEnv* env, jobject /* obj */, jboolean allowed,
                                           jbyteArray address) {
  log::verbose("");
  if (!sBluetoothInterface) {
    return false;
  }
  jbyte* addr = env->GetByteArrayElements(address, nullptr);
  if (addr == nullptr) {
    jniThrowIOException(env, EINVAL);
    return false;
  }

  RawAddress addr_obj = {};
  addr_obj.FromOctets(reinterpret_cast<uint8_t*>(addr));
  env->ReleaseByteArrayElements(address, addr, 0);
  sBluetoothInterface->allow_low_latency_audio(allowed, addr_obj);
  return true;
}

static void metadataChangedNative(JNIEnv* env, jobject /* obj */, jbyteArray address, jint key,
                                  jbyteArray value) {
  log::verbose("");
  if (!sBluetoothInterface) {
    return;
  }
  jbyte* addr = env->GetByteArrayElements(address, nullptr);
  if (addr == nullptr) {
    jniThrowIOException(env, EINVAL);
    return;
  }
  RawAddress addr_obj = {};
  addr_obj.FromOctets(reinterpret_cast<uint8_t*>(addr));
  env->ReleaseByteArrayElements(address, addr, 0);

  if (value == NULL) {
    log::error("metadataChangedNative() ignoring NULL array");
    return;
  }

  uint16_t len = (uint16_t)env->GetArrayLength(value);
  jbyte* p_value = env->GetByteArrayElements(value, NULL);
  if (p_value == NULL) {
    return;
  }

  std::vector<uint8_t> val_vec(reinterpret_cast<uint8_t*>(p_value),
                               reinterpret_cast<uint8_t*>(p_value + len));
  env->ReleaseByteArrayElements(value, p_value, 0);

  sBluetoothInterface->metadata_changed(addr_obj, key, std::move(val_vec));
  return;
}

static jboolean interopMatchAddrNative(JNIEnv* env, jclass /* clazz */, jstring feature_name,
                                       jstring address) {
  log::verbose("");

  if (!sBluetoothInterface) {
    log::warn("sBluetoothInterface is null.");
    return JNI_FALSE;
  }

  const char* tmp_addr = env->GetStringUTFChars(address, NULL);
  if (!tmp_addr) {
    log::warn("address is null.");
    return JNI_FALSE;
  }
  RawAddress bdaddr;
  bool success = RawAddress::FromString(tmp_addr, bdaddr);

  env->ReleaseStringUTFChars(address, tmp_addr);

  if (!success) {
    log::warn("address is invalid.");
    return JNI_FALSE;
  }

  const char* feature_name_str = env->GetStringUTFChars(feature_name, NULL);
  if (!feature_name_str) {
    log::warn("feature name is null.");
    return JNI_FALSE;
  }

  bool matched = sBluetoothInterface->interop_match_addr(feature_name_str, &bdaddr);
  env->ReleaseStringUTFChars(feature_name, feature_name_str);

  return matched ? JNI_TRUE : JNI_FALSE;
}

static jboolean interopMatchNameNative(JNIEnv* env, jclass /* clazz */, jstring feature_name,
                                       jstring name) {
  log::verbose("");

  if (!sBluetoothInterface) {
    log::warn("sBluetoothInterface is null.");
    return JNI_FALSE;
  }

  const char* feature_name_str = env->GetStringUTFChars(feature_name, NULL);
  if (!feature_name_str) {
    log::warn("feature name is null.");
    return JNI_FALSE;
  }

  const char* name_str = env->GetStringUTFChars(name, NULL);
  if (!name_str) {
    log::warn("name is null.");
    env->ReleaseStringUTFChars(feature_name, feature_name_str);
    return JNI_FALSE;
  }

  bool matched = sBluetoothInterface->interop_match_name(feature_name_str, name_str);
  env->ReleaseStringUTFChars(feature_name, feature_name_str);
  env->ReleaseStringUTFChars(name, name_str);

  return matched ? JNI_TRUE : JNI_FALSE;
}

static jboolean interopMatchAddrOrNameNative(JNIEnv* env, jclass /* clazz */, jstring feature_name,
                                             jstring address) {
  log::verbose("");

  if (!sBluetoothInterface) {
    log::warn("sBluetoothInterface is null.");
    return JNI_FALSE;
  }

  const char* tmp_addr = env->GetStringUTFChars(address, NULL);
  if (!tmp_addr) {
    log::warn("address is null.");
    return JNI_FALSE;
  }
  RawAddress bdaddr;
  bool success = RawAddress::FromString(tmp_addr, bdaddr);

  env->ReleaseStringUTFChars(address, tmp_addr);

  if (!success) {
    log::warn("address is invalid.");
    return JNI_FALSE;
  }

  const char* feature_name_str = env->GetStringUTFChars(feature_name, NULL);
  if (!feature_name_str) {
    log::warn("feature name is null.");
    return JNI_FALSE;
  }

  bool matched = sBluetoothInterface->interop_match_addr_or_name(feature_name_str, &bdaddr);
  env->ReleaseStringUTFChars(feature_name, feature_name_str);

  return matched ? JNI_TRUE : JNI_FALSE;
}

static void interopDatabaseAddRemoveAddrNative(JNIEnv* env, jclass /* clazz */, jboolean do_add,
                                               jstring feature_name, jstring address, jint length) {
  log::verbose("");

  if (!sBluetoothInterface) {
    log::warn("sBluetoothInterface is null.");
    return;
  }

  if ((do_add == JNI_TRUE) && (length <= 0 || length > 6)) {
    log::error("address length {} is invalid, valid length is [1,6]", length);
    return;
  }

  const char* tmp_addr = env->GetStringUTFChars(address, NULL);
  if (!tmp_addr) {
    log::warn("address is null.");
    return;
  }
  RawAddress bdaddr;
  bool success = RawAddress::FromString(tmp_addr, bdaddr);

  env->ReleaseStringUTFChars(address, tmp_addr);

  if (!success) {
    log::warn("address is invalid.");
    return;
  }

  const char* feature_name_str = env->GetStringUTFChars(feature_name, NULL);
  if (!feature_name_str) {
    log::warn("feature name is null.");
    return;
  }

  sBluetoothInterface->interop_database_add_remove_addr((do_add == JNI_TRUE), feature_name_str,
                                                        &bdaddr, static_cast<int>(length));

  env->ReleaseStringUTFChars(feature_name, feature_name_str);
}

static void interopDatabaseAddRemoveNameNative(JNIEnv* env, jclass /* clazz */, jboolean do_add,
                                               jstring feature_name, jstring name) {
  log::verbose("");

  if (!sBluetoothInterface) {
    log::warn("sBluetoothInterface is null.");
    return;
  }

  const char* feature_name_str = env->GetStringUTFChars(feature_name, NULL);
  if (!feature_name_str) {
    log::warn("feature name is null.");
    return;
  }

  const char* name_str = env->GetStringUTFChars(name, NULL);
  if (!name_str) {
    log::warn("name is null.");
    env->ReleaseStringUTFChars(feature_name, feature_name_str);
    return;
  }

  sBluetoothInterface->interop_database_add_remove_name((do_add == JNI_TRUE), feature_name_str,
                                                        name_str);

  env->ReleaseStringUTFChars(feature_name, feature_name_str);
  env->ReleaseStringUTFChars(name, name_str);
}

static int getRemotePbapPceVersionNative(JNIEnv* env, jobject /* obj */, jstring address) {
  log::verbose("");

  if (!sBluetoothInterface) {
    return JNI_FALSE;
  }

  const char* tmp_addr = env->GetStringUTFChars(address, NULL);
  if (!tmp_addr) {
    log::warn("address is null.");
    return JNI_FALSE;
  }

  RawAddress bdaddr;
  bool success = RawAddress::FromString(tmp_addr, bdaddr);

  env->ReleaseStringUTFChars(address, tmp_addr);

  if (!success) {
    log::warn("address is invalid.");
    return JNI_FALSE;
  }

  return sBluetoothInterface->get_remote_pbap_pce_version(&bdaddr);
}

static jboolean pbapPseDynamicVersionUpgradeIsEnabledNative(JNIEnv* /* env */, jobject /* obj */) {
  return JNI_FALSE;
}

static jint getSocketL2capLocalChannelIdNative(JNIEnv* /* env */, jobject /* obj */,
                                               jlong conn_uuid_lsb, jlong conn_uuid_msb) {
  log::verbose("");

  if (!sBluetoothSocketInterface) {
    return INVALID_CID;
  }
  uint16_t cid;
  Uuid uuid = from_java_uuid(conn_uuid_msb, conn_uuid_lsb);
  if (sBluetoothSocketInterface->get_l2cap_local_cid(uuid, &cid) != BT_STATUS_SUCCESS) {
    return INVALID_CID;
  }
  return (jint)cid;
}

static jint getSocketL2capRemoteChannelIdNative(JNIEnv* /* env */, jobject /* obj */,
                                                jlong conn_uuid_lsb, jlong conn_uuid_msb) {
  log::verbose("");

  if (!sBluetoothSocketInterface) {
    return INVALID_CID;
  }
  uint16_t cid;
  Uuid uuid = from_java_uuid(conn_uuid_msb, conn_uuid_lsb);
  if (sBluetoothSocketInterface->get_l2cap_remote_cid(uuid, &cid) != BT_STATUS_SUCCESS) {
    return INVALID_CID;
  }
  return (jint)cid;
}

static jboolean setDefaultEventMaskExceptNative(JNIEnv* /* env */, jobject /* obj */, jlong mask,
                                                jlong le_mask) {
  log::verbose("");

  if (!sBluetoothInterface) {
    return JNI_FALSE;
  }

  int ret = sBluetoothInterface->set_default_event_mask_except(mask, le_mask);
  return (ret == BT_STATUS_SUCCESS) ? JNI_TRUE : JNI_FALSE;
}

static jboolean clearEventFilterNative(JNIEnv* /* env */, jobject /* obj */) {
  log::verbose("");

  if (!sBluetoothInterface) {
    return JNI_FALSE;
  }

  int ret = sBluetoothInterface->clear_event_filter();
  return (ret == BT_STATUS_SUCCESS) ? JNI_TRUE : JNI_FALSE;
}

static jboolean clearFilterAcceptListNative(JNIEnv* /* env */, jobject /* obj */) {
  log::verbose("");

  if (!sBluetoothInterface) {
    return JNI_FALSE;
  }

  int ret = sBluetoothInterface->clear_filter_accept_list();
  return (ret == BT_STATUS_SUCCESS) ? JNI_TRUE : JNI_FALSE;
}

static jboolean disconnectAllAclsNative(JNIEnv* /* env */, jobject /* obj */) {
  log::verbose("");

  if (!sBluetoothInterface) {
    return JNI_FALSE;
  }

  int ret = sBluetoothInterface->disconnect_all_acls();
  return (ret == BT_STATUS_SUCCESS) ? JNI_TRUE : JNI_FALSE;
}

static jboolean allowWakeByHidNative(JNIEnv* /* env */, jobject /* obj */) {
  log::verbose("");

  if (!sBluetoothInterface) {
    return JNI_FALSE;
  }

  int ret = sBluetoothInterface->allow_wake_by_hid();
  return (ret == BT_STATUS_SUCCESS) ? JNI_TRUE : JNI_FALSE;
}

static jboolean restoreFilterAcceptListNative(JNIEnv* /* env */, jobject /* obj */) {
  log::verbose("");

  if (!sBluetoothInterface) {
    return JNI_FALSE;
  }

  int ret = sBluetoothInterface->restore_filter_accept_list();
  return (ret == BT_STATUS_SUCCESS) ? JNI_TRUE : JNI_FALSE;
}

int register_com_android_bluetooth_btservice_AdapterService(JNIEnv* env) {
  const JNINativeMethod methods[] = {
          {"initNative", "(ZZIZ)Z", reinterpret_cast<void*>(initNative)},
          {"cleanupNative", "()V", reinterpret_cast<void*>(cleanupNative)},
          {"enableNative", "()Z", reinterpret_cast<void*>(enableNative)},
          {"disableNative", "()Z", reinterpret_cast<void*>(disableNative)},
          {"setScanModeNative", "(I)Z", reinterpret_cast<void*>(setScanModeNative)},
          {"setAdapterPropertyNative", "(I[B)Z", reinterpret_cast<void*>(setAdapterPropertyNative)},
          {"getAdapterPropertiesNative", "()Z",
           reinterpret_cast<void*>(getAdapterPropertiesNative)},
          {"getAdapterPropertyNative", "(I)Z", reinterpret_cast<void*>(getAdapterPropertyNative)},
          {"getDevicePropertyNative", "([BI)Z", reinterpret_cast<void*>(getDevicePropertyNative)},
          {"setDevicePropertyNative", "([BI[B)Z", reinterpret_cast<void*>(setDevicePropertyNative)},
          {"startDiscoveryNative", "()Z", reinterpret_cast<void*>(startDiscoveryNative)},
          {"cancelDiscoveryNative", "()Z", reinterpret_cast<void*>(cancelDiscoveryNative)},
          {"createBondNative", "([BII)Z", reinterpret_cast<void*>(createBondNative)},
          {"createBondOutOfBandNative",
           "([BILandroid/bluetooth/OobData;Landroid/bluetooth/OobData;)Z",
           reinterpret_cast<void*>(createBondOutOfBandNative)},
          {"removeBondNative", "([B)Z", reinterpret_cast<void*>(removeBondNative)},
          {"cancelBondNative", "([B)Z", reinterpret_cast<void*>(cancelBondNative)},
          {"pairingIsBusyNative", "()Z", reinterpret_cast<void*>(pairingIsBusyNative)},
          {"generateLocalOobDataNative", "(I)V",
           reinterpret_cast<void*>(generateLocalOobDataNative)},
          {"getConnectionStateNative", "([B)I", reinterpret_cast<void*>(getConnectionStateNative)},
          {"pinReplyNative", "([BZI[B)Z", reinterpret_cast<void*>(pinReplyNative)},
          {"sspReplyNative", "([BIZI)Z", reinterpret_cast<void*>(sspReplyNative)},
          {"getRemoteServicesNative", "([BI)Z", reinterpret_cast<void*>(getRemoteServicesNative)},
          {"readEnergyInfoNative", "()I", reinterpret_cast<void*>(readEnergyInfoNative)},
          {"dumpNative", "(Ljava/io/FileDescriptor;[Ljava/lang/String;)V",
           reinterpret_cast<void*>(dumpNative)},
          {"dumpMetricsNative", "()[B", reinterpret_cast<void*>(dumpMetricsNative)},
          {"factoryResetNative", "()Z", reinterpret_cast<void*>(factoryResetNative)},
          {"obfuscateAddressNative", "([B)[B", reinterpret_cast<void*>(obfuscateAddressNative)},
          {"setBufferLengthMillisNative", "(II)Z",
           reinterpret_cast<void*>(setBufferLengthMillisNative)},
          {"getMetricIdNative", "([B)I", reinterpret_cast<void*>(getMetricIdNative)},
          {"connectSocketNative", "([BI[BIIIILjava/lang/String;JJI)I",
           reinterpret_cast<void*>(connectSocketNative)},
          {"createSocketChannelNative", "(ILjava/lang/String;[BIIIILjava/lang/String;JJI)I",
           reinterpret_cast<void*>(createSocketChannelNative)},
          {"requestMaximumTxDataLengthNative", "([B)V",
           reinterpret_cast<void*>(requestMaximumTxDataLengthNative)},
          {"allowLowLatencyAudioNative", "(Z[B)Z",
           reinterpret_cast<void*>(allowLowLatencyAudioNative)},
          {"metadataChangedNative", "([BI[B)V", reinterpret_cast<void*>(metadataChangedNative)},
          {"interopMatchAddrNative", "(Ljava/lang/String;Ljava/lang/String;)Z",
           reinterpret_cast<void*>(interopMatchAddrNative)},
          {"interopMatchNameNative", "(Ljava/lang/String;Ljava/lang/String;)Z",
           reinterpret_cast<void*>(interopMatchNameNative)},
          {"interopMatchAddrOrNameNative", "(Ljava/lang/String;Ljava/lang/String;)Z",
           reinterpret_cast<void*>(interopMatchAddrOrNameNative)},
          {"interopDatabaseAddRemoveAddrNative", "(ZLjava/lang/String;Ljava/lang/String;I)V",
           reinterpret_cast<void*>(interopDatabaseAddRemoveAddrNative)},
          {"interopDatabaseAddRemoveNameNative", "(ZLjava/lang/String;Ljava/lang/String;)V",
           reinterpret_cast<void*>(interopDatabaseAddRemoveNameNative)},
          {"getRemotePbapPceVersionNative", "(Ljava/lang/String;)I",
           reinterpret_cast<void*>(getRemotePbapPceVersionNative)},
          {"pbapPseDynamicVersionUpgradeIsEnabledNative", "()Z",
           reinterpret_cast<void*>(pbapPseDynamicVersionUpgradeIsEnabledNative)},
          {"getSocketL2capLocalChannelIdNative", "(JJ)I",
           reinterpret_cast<void*>(getSocketL2capLocalChannelIdNative)},
          {"getSocketL2capRemoteChannelIdNative", "(JJ)I",
           reinterpret_cast<void*>(getSocketL2capRemoteChannelIdNative)},
          {"setDefaultEventMaskExceptNative", "(JJ)Z",
           reinterpret_cast<void*>(setDefaultEventMaskExceptNative)},
          {"clearEventFilterNative", "()Z", reinterpret_cast<void*>(clearEventFilterNative)},
          {"clearFilterAcceptListNative", "()Z",
           reinterpret_cast<void*>(clearFilterAcceptListNative)},
          {"disconnectAllAclsNative", "()Z", reinterpret_cast<void*>(disconnectAllAclsNative)},
          {"allowWakeByHidNative", "()Z", reinterpret_cast<void*>(allowWakeByHidNative)},
          {"restoreFilterAcceptListNative", "()Z",
           reinterpret_cast<void*>(restoreFilterAcceptListNative)},
  };
  const int result = REGISTER_NATIVE_METHODS(
          env, "com/android/bluetooth/btservice/AdapterNativeInterface", methods);
  if (result != 0) {
    return result;
  }

  jclass jniAdapterNativeInterfaceClass =
          env->FindClass("com/android/bluetooth/btservice/AdapterNativeInterface");
  sJniCallbacksField = env->GetFieldID(jniAdapterNativeInterfaceClass, "mJniCallbacks",
                                       "Lcom/android/bluetooth/btservice/JniCallbacks;");
  env->DeleteLocalRef(jniAdapterNativeInterfaceClass);

  const JNIJavaMethod javaMethods[] = {
          {"oobDataReceivedCallback", "(ILandroid/bluetooth/OobData;)V",
           &method_oobDataReceivedCallback},
          {"stateChangeCallback", "(I)V", &method_stateChangeCallback},
          {"adapterPropertyChangedCallback", "([I[[B)V", &method_adapterPropertyChangedCallback},
          {"discoveryStateChangeCallback", "(I)V", &method_discoveryStateChangeCallback},
          {"devicePropertyChangedCallback", "([B[I[[B)V", &method_devicePropertyChangedCallback},
          {"deviceFoundCallback", "([B)V", &method_deviceFoundCallback},
          {"pinRequestCallback", "([B[BIZ)V", &method_pinRequestCallback},
          {"sspRequestCallback", "([BII)V", &method_sspRequestCallback},
          {"bondStateChangeCallback", "(I[BII)V", &method_bondStateChangeCallback},
          {"addressConsolidateCallback", "([B[B)V", &method_addressConsolidateCallback},
          {"leAddressAssociateCallback", "([B[BI)V", &method_leAddressAssociateCallback},
          {"aclStateChangeCallback", "(I[BIIII)V", &method_aclStateChangeCallback},
          {"linkQualityReportCallback", "(JIIIIII)V", &method_linkQualityReportCallback},
          {"switchBufferSizeCallback", "(Z)V", &method_switchBufferSizeCallback},
          {"switchCodecCallback", "(Z)V", &method_switchCodecCallback},
          {"acquireWakeLock", "(Ljava/lang/String;)Z", &method_acquireWakeLock},
          {"releaseWakeLock", "(Ljava/lang/String;)Z", &method_releaseWakeLock},
          {"energyInfoCallback", "(IIJJJJ[Landroid/bluetooth/UidTraffic;)V", &method_energyInfo},
          {"keyMissingCallback", "([B)V", &method_keyMissingCallback},
          {"encryptionChangeCallback", "([BIZIZI)V", &method_encryptionChangeCallback},
  };
  GET_JAVA_METHODS(env, "com/android/bluetooth/btservice/JniCallbacks", javaMethods);

  const JNIJavaMethod javaUuidTrafficMethods[] = {
          {"<init>", "(IJJ)V", &android_bluetooth_UidTraffic.constructor},
  };
  GET_JAVA_METHODS(env, "android/bluetooth/UidTraffic", javaUuidTrafficMethods);

  if (env->GetJavaVM(&vm) != JNI_OK) {
    log::error("Could not get JavaVM");
  }

  if (hal_util_load_bt_library(&sBluetoothInterface)) {
    log::error("No Bluetooth Library found");
  }

  return 0;
}

} /* namespace android */

/*
 * JNI Initialization
 */
jint JNI_OnLoad(JavaVM* jvm, void* /* reserved */) {
  /* Set the default logging level for the process using the tag
   *  "log.tag.bluetooth" and/or "persist.log.tag.bluetooth" via the android
   * logging framework.
   */
  const char* stack_default_log_tag = "bluetooth";
  int default_prio = ANDROID_LOG_INFO;
  if (__android_log_is_loggable(ANDROID_LOG_VERBOSE, stack_default_log_tag, default_prio)) {
    __android_log_set_minimum_priority(ANDROID_LOG_VERBOSE);
    log::info("Set stack default log level to 'VERBOSE'");
  } else if (__android_log_is_loggable(ANDROID_LOG_DEBUG, stack_default_log_tag, default_prio)) {
    __android_log_set_minimum_priority(ANDROID_LOG_DEBUG);
    log::info("Set stack default log level to 'DEBUG'");
  } else if (__android_log_is_loggable(ANDROID_LOG_INFO, stack_default_log_tag, default_prio)) {
    __android_log_set_minimum_priority(ANDROID_LOG_INFO);
    log::info("Set stack default log level to 'INFO'");
  } else if (__android_log_is_loggable(ANDROID_LOG_WARN, stack_default_log_tag, default_prio)) {
    __android_log_set_minimum_priority(ANDROID_LOG_WARN);
    log::info("Set stack default log level to 'WARN'");
  } else if (__android_log_is_loggable(ANDROID_LOG_ERROR, stack_default_log_tag, default_prio)) {
    __android_log_set_minimum_priority(ANDROID_LOG_ERROR);
    log::info("Set stack default log level to 'ERROR'");
  }

  JNIEnv* e;
  int status;

  log::verbose("Bluetooth Adapter Service : loading JNI\n");

  // Check JNI version
  if (jvm->GetEnv(reinterpret_cast<void**>(&e), JNI_VERSION_1_6)) {
    log::error("JNI version mismatch error");
    return JNI_ERR;
  }

  status = android::register_com_android_bluetooth_btservice_AdapterService(e);
  if (status < 0) {
    log::error("jni adapter service registration failure, status: {}", status);
    return JNI_ERR;
  }

  status = android::register_com_android_bluetooth_btservice_BluetoothKeystore(e);
  if (status < 0) {
    log::error("jni BluetoothKeyStore registration failure: {}", status);
    return JNI_ERR;
  }

  status = android::register_com_android_bluetooth_hfp(e);
  if (status < 0) {
    log::error("jni hfp registration failure, status: {}", status);
    return JNI_ERR;
  }

  status = android::register_com_android_bluetooth_hfpclient(e);
  if (status < 0) {
    log::error("jni hfp client registration failure, status: {}", status);
    return JNI_ERR;
  }

  status = android::register_com_android_bluetooth_a2dp(e);
  if (status < 0) {
    log::error("jni a2dp source registration failure: {}", status);
    return JNI_ERR;
  }

  status = android::register_com_android_bluetooth_a2dp_sink(e);
  if (status < 0) {
    log::error("jni a2dp sink registration failure: {}", status);
    return JNI_ERR;
  }

  status = android::register_com_android_bluetooth_avrcp_target(e);
  if (status < 0) {
    log::error("jni new avrcp target registration failure: {}", status);
  }

  status = android::register_com_android_bluetooth_avrcp_controller(e);
  if (status < 0) {
    log::error("jni avrcp controller registration failure: {}", status);
    return JNI_ERR;
  }

  status = android::register_com_android_bluetooth_hid_host(e);
  if (status < 0) {
    log::error("jni hid registration failure: {}", status);
    return JNI_ERR;
  }

  status = android::register_com_android_bluetooth_hid_device(e);
  if (status < 0) {
    log::error("jni hidd registration failure: {}", status);
    return JNI_ERR;
  }

  status = android::register_com_android_bluetooth_pan(e);
  if (status < 0) {
    log::error("jni pan registration failure: {}", status);
    return JNI_ERR;
  }

  status = android::register_com_android_bluetooth_gatt(e);
  if (status < 0) {
    log::error("jni gatt registration failure: {}", status);
    return JNI_ERR;
  }

  status = android::register_com_android_bluetooth_sdp(e);
  if (status < 0) {
    log::error("jni sdp registration failure: {}", status);
    return JNI_ERR;
  }

  status = android::register_com_android_bluetooth_hearing_aid(e);
  if (status < 0) {
    log::error("jni hearing aid registration failure: {}", status);
    return JNI_ERR;
  }

  status = android::register_com_android_bluetooth_hap_client(e);
  if (status < 0) {
    log::error("jni le audio hearing access client registration failure: {}", status);
    return JNI_ERR;
  }

  status = android::register_com_android_bluetooth_le_audio(e);
  if (status < 0) {
    log::error("jni le_audio registration failure: {}", status);
    return JNI_ERR;
  }

  status = android::register_com_android_bluetooth_vc(e);
  if (status < 0) {
    log::error("jni vc registration failure: {}", status);
    return JNI_ERR;
  }

  status = android::register_com_android_bluetooth_csip_set_coordinator(e);
  if (status < 0) {
    log::error("jni csis client registration failure: {}", status);
    return JNI_ERR;
  }

  status = android::register_com_android_bluetooth_btservice_BluetoothQualityReport(e);
  if (status < 0) {
    log::error("jni bluetooth quality report registration failure: {}", status);
    return JNI_ERR;
  }

  status = android::register_com_android_bluetooth_btservice_BluetoothHciVendorSpecific(e);
  if (status < 0) {
    log::error("jni bluetooth hci vendor-specific registration failure: {}", status);
    return JNI_ERR;
  }

  return JNI_VERSION_1_6;
}

namespace android {

/** Load the java methods or die*/
void jniGetMethodsOrDie(JNIEnv* env, const char* className, const JNIJavaMethod* methods,
                        int nMethods) {
  jclass clazz = env->FindClass(className);
  if (clazz == nullptr) {
    log::fatal("Native registration unable to find class '{}' aborting...", className);
  }

  for (int i = 0; i < nMethods; i++) {
    const JNIJavaMethod& method = methods[i];
    if (method.is_static) {
      *method.id = env->GetStaticMethodID(clazz, method.name, method.signature);
    } else {
      *method.id = env->GetMethodID(clazz, method.name, method.signature);
    }
    if (method.id == nullptr) {
      log::fatal("In class {}: Unable to find '{}' with signature={} is_static={}", className,
                 method.name, method.signature, method.is_static);
    }
  }

  env->DeleteLocalRef(clazz);
}
}  // namespace android
