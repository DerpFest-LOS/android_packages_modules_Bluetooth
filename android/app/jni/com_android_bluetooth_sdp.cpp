/*
 * Copyright (C) 2015 The Android Open Source Project
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

#define LOG_TAG "BluetoothSdpJni"

#include <bluetooth/log.h>
#include <jni.h>
#include <nativehelper/JNIHelp.h>
#include <nativehelper/scoped_local_ref.h>

#include <cerrno>
#include <cstdint>
#include <cstring>

#include "com_android_bluetooth.h"
#include "hardware/bluetooth.h"
#include "hardware/bt_sdp.h"
#include "types/bluetooth/uuid.h"
#include "types/raw_address.h"

using bluetooth::Uuid;

static const Uuid UUID_OBEX_OBJECT_PUSH = Uuid::From16Bit(0x1105);
static const Uuid UUID_PBAP_PSE = Uuid::From16Bit(0x112F);
static const Uuid UUID_MAP_MAS = Uuid::From16Bit(0x1132);
static const Uuid UUID_MAP_MNS = Uuid::From16Bit(0x1133);
static const Uuid UUID_SAP = Uuid::From16Bit(0x112D);
static const Uuid UUID_DIP = Uuid::From16Bit(0x1200);

namespace android {
static jmethodID method_sdpRecordFoundCallback;
static jmethodID method_sdpMasRecordFoundCallback;
static jmethodID method_sdpMnsRecordFoundCallback;
static jmethodID method_sdpPseRecordFoundCallback;
static jmethodID method_sdpOppOpsRecordFoundCallback;
static jmethodID method_sdpSapsRecordFoundCallback;
static jmethodID method_sdpDipRecordFoundCallback;

static const btsdp_interface_t* sBluetoothSdpInterface = NULL;

static void sdp_search_callback(bt_status_t status, const RawAddress& bd_addr, const Uuid& uuid_in,
                                int record_size, bluetooth_sdp_record* record);

btsdp_callbacks_t sBluetoothSdpCallbacks = {sizeof(sBluetoothSdpCallbacks), sdp_search_callback};

static jobject sCallbacksObj = NULL;

static void initializeNative(JNIEnv* env, jobject object) {
  const bt_interface_t* btInf = getBluetoothInterface();

  if (btInf == NULL) {
    log::error("Bluetooth module is not loaded");
    return;
  }
  if (sBluetoothSdpInterface != NULL) {
    log::warn("Cleaning up Bluetooth SDP Interface before initializing...");
    sBluetoothSdpInterface->deinit();
    sBluetoothSdpInterface = NULL;
  }

  sBluetoothSdpInterface =
          (btsdp_interface_t*)btInf->get_profile_interface(BT_PROFILE_SDP_CLIENT_ID);
  if (sBluetoothSdpInterface == NULL) {
    log::error("Error getting SDP client interface");
  } else {
    sBluetoothSdpInterface->init(&sBluetoothSdpCallbacks);
  }

  sCallbacksObj = env->NewGlobalRef(object);
}

static jboolean sdpSearchNative(JNIEnv* env, jobject /* obj */, jbyteArray address,
                                jbyteArray uuidObj) {
  log::debug("");

  if (!sBluetoothSdpInterface) {
    return JNI_FALSE;
  }

  jbyte* addr = env->GetByteArrayElements(address, NULL);
  if (addr == NULL) {
    jniThrowIOException(env, EINVAL);
    return JNI_FALSE;
  }

  jbyte* raw_uuid = env->GetByteArrayElements(uuidObj, NULL);
  if (!raw_uuid) {
    log::error("failed to get uuid");
    env->ReleaseByteArrayElements(address, addr, 0);
    return JNI_FALSE;
  }
  Uuid uuid = Uuid::From128BitBE((uint8_t*)raw_uuid);
  log::debug("UUID {}", uuid);

  int ret = sBluetoothSdpInterface->sdp_search((RawAddress*)addr, uuid);
  if (ret != BT_STATUS_SUCCESS) {
    log::error("SDP Search initialization failed: {}", ret);
  }

  if (addr) {
    env->ReleaseByteArrayElements(address, addr, 0);
  }
  if (raw_uuid) {
    env->ReleaseByteArrayElements(uuidObj, raw_uuid, 0);
  }
  return (ret == BT_STATUS_SUCCESS) ? JNI_TRUE : JNI_FALSE;
}

static void sdp_search_callback(bt_status_t status, const RawAddress& bd_addr, const Uuid& uuid_in,
                                int count, bluetooth_sdp_record* records) {
  CallbackEnv sCallbackEnv(__func__);
  if (!sCallbackEnv.valid()) {
    return;
  }

  ScopedLocalRef<jbyteArray> addr(sCallbackEnv.get(),
                                  sCallbackEnv->NewByteArray(sizeof(RawAddress)));
  if (!addr.get()) {
    return;
  }

  ScopedLocalRef<jbyteArray> uuid(sCallbackEnv.get(), sCallbackEnv->NewByteArray(sizeof(Uuid)));
  if (!uuid.get()) {
    return;
  }

  sCallbackEnv->SetByteArrayRegion(addr.get(), 0, sizeof(RawAddress), (const jbyte*)&bd_addr);
  sCallbackEnv->SetByteArrayRegion(uuid.get(), 0, sizeof(Uuid),
                                   (const jbyte*)uuid_in.To128BitBE().data());

  log::debug("Status is: {}, Record count: {}", bt_status_text(status), count);

  // Ensure we run the loop at least once, to also signal errors if they occur
  for (int i = 0; i < count || i == 0; i++) {
    bool more_results = (i < (count - 1)) ? true : false;
    bluetooth_sdp_record* record = &records[i];
    ScopedLocalRef<jstring> service_name(sCallbackEnv.get(), NULL);
    if (record->hdr.service_name_length > 0) {
      log::debug("ServiceName:  {}", record->mas.hdr.service_name);
      service_name.reset((jstring)sCallbackEnv->NewStringUTF(record->mas.hdr.service_name));
    }

    /* call the right callback according to the uuid*/
    if (uuid_in == UUID_MAP_MAS) {
      sCallbackEnv->CallVoidMethod(
              sCallbacksObj, method_sdpMasRecordFoundCallback, (jint)status, addr.get(), uuid.get(),
              (jint)record->mas.mas_instance_id, (jint)record->mas.hdr.l2cap_psm,
              (jint)record->mas.hdr.rfcomm_channel_number, (jint)record->mas.hdr.profile_version,
              (jint)record->mas.supported_features, (jint)record->mas.supported_message_types,
              service_name.get(), more_results);

    } else if (uuid_in == UUID_MAP_MNS) {
      sCallbackEnv->CallVoidMethod(
              sCallbacksObj, method_sdpMnsRecordFoundCallback, (jint)status, addr.get(), uuid.get(),
              (jint)record->mns.hdr.l2cap_psm, (jint)record->mns.hdr.rfcomm_channel_number,
              (jint)record->mns.hdr.profile_version, (jint)record->mns.supported_features,
              service_name.get(), more_results);

    } else if (uuid_in == UUID_PBAP_PSE) {
      sCallbackEnv->CallVoidMethod(
              sCallbacksObj, method_sdpPseRecordFoundCallback, (jint)status, addr.get(), uuid.get(),
              (jint)record->pse.hdr.l2cap_psm, (jint)record->pse.hdr.rfcomm_channel_number,
              (jint)record->pse.hdr.profile_version, (jint)record->pse.supported_features,
              (jint)record->pse.supported_repositories, service_name.get(), more_results);

    } else if (uuid_in == UUID_OBEX_OBJECT_PUSH) {
      jint formats_list_size = record->ops.supported_formats_list_len;
      ScopedLocalRef<jbyteArray> formats_list(sCallbackEnv.get(),
                                              sCallbackEnv->NewByteArray(formats_list_size));
      if (!formats_list.get()) {
        return;
      }
      sCallbackEnv->SetByteArrayRegion(formats_list.get(), 0, formats_list_size,
                                       (jbyte*)record->ops.supported_formats_list);

      sCallbackEnv->CallVoidMethod(sCallbacksObj, method_sdpOppOpsRecordFoundCallback, (jint)status,
                                   addr.get(), uuid.get(), (jint)record->ops.hdr.l2cap_psm,
                                   (jint)record->ops.hdr.rfcomm_channel_number,
                                   (jint)record->ops.hdr.profile_version, service_name.get(),
                                   formats_list.get(), more_results);

    } else if (uuid_in == UUID_SAP) {
      sCallbackEnv->CallVoidMethod(
              sCallbacksObj, method_sdpSapsRecordFoundCallback, (jint)status, addr.get(),
              uuid.get(), (jint)record->mas.hdr.rfcomm_channel_number,
              (jint)record->mas.hdr.profile_version, service_name.get(), more_results);
    } else if (uuid_in == UUID_DIP) {
      log::debug("Get UUID_DIP");
      sCallbackEnv->CallVoidMethod(sCallbacksObj, method_sdpDipRecordFoundCallback, (jint)status,
                                   addr.get(), uuid.get(), (jint)record->dip.spec_id,
                                   (jint)record->dip.vendor, (jint)record->dip.vendor_id_source,
                                   (jint)record->dip.product, (jint)record->dip.version,
                                   record->dip.primary_record, more_results);
    } else {
      // we don't have a wrapper for this uuid, send as raw data
      jint record_data_size = record->hdr.user1_ptr_len;
      ScopedLocalRef<jbyteArray> record_data(sCallbackEnv.get(),
                                             sCallbackEnv->NewByteArray(record_data_size));
      if (!record_data.get()) {
        return;
      }

      sCallbackEnv->SetByteArrayRegion(record_data.get(), 0, record_data_size,
                                       (jbyte*)record->hdr.user1_ptr);
      sCallbackEnv->CallVoidMethod(sCallbacksObj, method_sdpRecordFoundCallback, (jint)status,
                                   addr.get(), uuid.get(), record_data_size, record_data.get());
    }
  }  // End of for-loop
}

static jint sdpCreateMapMasRecordNative(JNIEnv* env, jobject /* obj */, jstring name_str,
                                        jint mas_id, jint scn, jint l2cap_psm, jint version,
                                        jint msg_types, jint features) {
  log::debug("");
  if (!sBluetoothSdpInterface) {
    return -1;
  }

  bluetooth_sdp_record record = {};  // Must be zero initialized
  record.mas.hdr.type = SDP_TYPE_MAP_MAS;

  const char* service_name = NULL;
  if (name_str != NULL) {
    service_name = env->GetStringUTFChars(name_str, NULL);
    record.mas.hdr.service_name = (char*)service_name;
    record.mas.hdr.service_name_length = strlen(service_name);
  } else {
    record.mas.hdr.service_name = NULL;
    record.mas.hdr.service_name_length = 0;
  }
  record.mas.hdr.rfcomm_channel_number = scn;
  record.mas.hdr.l2cap_psm = l2cap_psm;
  record.mas.hdr.profile_version = version;

  record.mas.mas_instance_id = mas_id;
  record.mas.supported_features = features;
  record.mas.supported_message_types = msg_types;

  int handle = -1;
  int ret = sBluetoothSdpInterface->create_sdp_record(&record, &handle);
  if (ret != BT_STATUS_SUCCESS) {
    log::error("SDP Create record failed: {}", ret);
  } else {
    log::debug("SDP Create record success - handle: {}", handle);
  }

  if (service_name) {
    env->ReleaseStringUTFChars(name_str, service_name);
  }
  return handle;
}

static jint sdpCreateMapMnsRecordNative(JNIEnv* env, jobject /* obj */, jstring name_str, jint scn,
                                        jint l2cap_psm, jint version, jint features) {
  log::debug("");
  if (!sBluetoothSdpInterface) {
    return -1;
  }

  bluetooth_sdp_record record = {};  // Must be zero initialized
  record.mns.hdr.type = SDP_TYPE_MAP_MNS;

  const char* service_name = NULL;
  if (name_str != NULL) {
    service_name = env->GetStringUTFChars(name_str, NULL);
    record.mns.hdr.service_name = (char*)service_name;
    record.mns.hdr.service_name_length = strlen(service_name);
  } else {
    record.mns.hdr.service_name = NULL;
    record.mns.hdr.service_name_length = 0;
  }
  record.mns.hdr.rfcomm_channel_number = scn;
  record.mns.hdr.l2cap_psm = l2cap_psm;
  record.mns.hdr.profile_version = version;

  record.mns.supported_features = features;

  int handle = -1;
  int ret = sBluetoothSdpInterface->create_sdp_record(&record, &handle);
  if (ret != BT_STATUS_SUCCESS) {
    log::error("SDP Create record failed: {}", ret);
  } else {
    log::debug("SDP Create record success - handle: {}", handle);
  }

  if (service_name) {
    env->ReleaseStringUTFChars(name_str, service_name);
  }
  return handle;
}

static jint sdpCreatePbapPceRecordNative(JNIEnv* env, jobject /* obj */, jstring name_str,
                                         jint version) {
  log::debug("");
  if (!sBluetoothSdpInterface) {
    return -1;
  }

  bluetooth_sdp_record record = {};  // Must be zero initialized
  record.pce.hdr.type = SDP_TYPE_PBAP_PCE;

  const char* service_name = NULL;
  if (name_str != NULL) {
    service_name = env->GetStringUTFChars(name_str, NULL);
    record.pce.hdr.service_name = (char*)service_name;
    record.pce.hdr.service_name_length = strlen(service_name);
  } else {
    record.pce.hdr.service_name = NULL;
    record.pce.hdr.service_name_length = 0;
  }
  record.pce.hdr.profile_version = version;

  int handle = -1;
  int ret = sBluetoothSdpInterface->create_sdp_record(&record, &handle);
  if (ret != BT_STATUS_SUCCESS) {
    log::error("SDP Create record failed: {}", ret);
  } else {
    log::debug("SDP Create record success - handle: {}", handle);
  }

  if (service_name) {
    env->ReleaseStringUTFChars(name_str, service_name);
  }
  return handle;
}

static jint sdpCreatePbapPseRecordNative(JNIEnv* env, jobject /* obj */, jstring name_str, jint scn,
                                         jint l2cap_psm, jint version, jint supported_repositories,
                                         jint features) {
  log::debug("");
  if (!sBluetoothSdpInterface) {
    return -1;
  }

  bluetooth_sdp_record record = {};  // Must be zero initialized
  record.pse.hdr.type = SDP_TYPE_PBAP_PSE;

  const char* service_name = NULL;
  if (name_str != NULL) {
    service_name = env->GetStringUTFChars(name_str, NULL);
    record.pse.hdr.service_name = (char*)service_name;
    record.pse.hdr.service_name_length = strlen(service_name);
  } else {
    record.pse.hdr.service_name = NULL;
    record.pse.hdr.service_name_length = 0;
  }
  record.pse.hdr.rfcomm_channel_number = scn;
  record.pse.hdr.l2cap_psm = l2cap_psm;
  record.pse.hdr.profile_version = version;

  record.pse.supported_features = features;
  record.pse.supported_repositories = supported_repositories;

  int handle = -1;
  int ret = sBluetoothSdpInterface->create_sdp_record(&record, &handle);
  if (ret != BT_STATUS_SUCCESS) {
    log::error("SDP Create record failed: {}", ret);
  } else {
    log::debug("SDP Create record success - handle: {}", handle);
  }

  if (service_name) {
    env->ReleaseStringUTFChars(name_str, service_name);
  }
  return handle;
}

static jint sdpCreateOppOpsRecordNative(JNIEnv* env, jobject /* obj */, jstring name_str, jint scn,
                                        jint l2cap_psm, jint version,
                                        jbyteArray supported_formats_list) {
  log::debug("");
  if (!sBluetoothSdpInterface) {
    return -1;
  }

  bluetooth_sdp_record record = {};  // Must be zero initialized
  record.ops.hdr.type = SDP_TYPE_OPP_SERVER;

  const char* service_name = NULL;
  if (name_str != NULL) {
    service_name = env->GetStringUTFChars(name_str, NULL);
    record.ops.hdr.service_name = (char*)service_name;
    record.ops.hdr.service_name_length = strlen(service_name);
  } else {
    record.ops.hdr.service_name = NULL;
    record.ops.hdr.service_name_length = 0;
  }
  record.ops.hdr.rfcomm_channel_number = scn;
  record.ops.hdr.l2cap_psm = l2cap_psm;
  record.ops.hdr.profile_version = version;

  int formats_list_len = 0;
  jbyte* formats_list = env->GetByteArrayElements(supported_formats_list, NULL);
  if (formats_list != NULL) {
    formats_list_len = env->GetArrayLength(supported_formats_list);
    if (formats_list_len > SDP_OPP_SUPPORTED_FORMATS_MAX_LENGTH) {
      formats_list_len = SDP_OPP_SUPPORTED_FORMATS_MAX_LENGTH;
    }
    memcpy(record.ops.supported_formats_list, formats_list, formats_list_len);
  }

  record.ops.supported_formats_list_len = formats_list_len;

  int handle = -1;
  int ret = sBluetoothSdpInterface->create_sdp_record(&record, &handle);
  if (ret != BT_STATUS_SUCCESS) {
    log::error("SDP Create record failed: {}", ret);
  } else {
    log::debug("SDP Create record success - handle: {}", handle);
  }

  if (service_name) {
    env->ReleaseStringUTFChars(name_str, service_name);
  }
  if (formats_list) {
    env->ReleaseByteArrayElements(supported_formats_list, formats_list, 0);
  }
  return handle;
}

static jint sdpCreateSapsRecordNative(JNIEnv* env, jobject /* obj */, jstring name_str, jint scn,
                                      jint version) {
  log::debug("");
  if (!sBluetoothSdpInterface) {
    return -1;
  }

  bluetooth_sdp_record record = {};  // Must be zero initialized
  record.sap.hdr.type = SDP_TYPE_SAP_SERVER;

  const char* service_name = NULL;
  if (name_str != NULL) {
    service_name = env->GetStringUTFChars(name_str, NULL);
    record.mas.hdr.service_name = (char*)service_name;
    record.mas.hdr.service_name_length = strlen(service_name);
  } else {
    record.mas.hdr.service_name = NULL;
    record.mas.hdr.service_name_length = 0;
  }
  record.mas.hdr.rfcomm_channel_number = scn;
  record.mas.hdr.profile_version = version;

  int handle = -1;
  int ret = sBluetoothSdpInterface->create_sdp_record(&record, &handle);
  if (ret != BT_STATUS_SUCCESS) {
    log::error("SDP Create record failed: {}", ret);
  } else {
    log::debug("SDP Create record success - handle: {}", handle);
  }

  if (service_name) {
    env->ReleaseStringUTFChars(name_str, service_name);
  }
  return handle;
}

static jboolean sdpRemoveSdpRecordNative(JNIEnv* /* env */, jobject /* obj */, jint record_id) {
  log::debug("");
  if (!sBluetoothSdpInterface) {
    return false;
  }

  int ret = sBluetoothSdpInterface->remove_sdp_record(record_id);
  if (ret != BT_STATUS_SUCCESS) {
    log::error("SDP Remove record failed: {}", ret);
    return false;
  }

  log::debug("SDP Remove record success - handle: {}", record_id);
  return true;
}

static void cleanupNative(JNIEnv* env, jobject /* object */) {
  const bt_interface_t* btInf = getBluetoothInterface();

  if (btInf == NULL) {
    log::error("Bluetooth module is not loaded");
    return;
  }

  if (sBluetoothSdpInterface != NULL) {
    log::warn("Cleaning up Bluetooth SDP Interface...");
    sBluetoothSdpInterface->deinit();
    sBluetoothSdpInterface = NULL;
  }

  if (sCallbacksObj != NULL) {
    log::warn("Cleaning up Bluetooth SDP object");
    env->DeleteGlobalRef(sCallbacksObj);
    sCallbacksObj = NULL;
  }
}

int register_com_android_bluetooth_sdp(JNIEnv* env) {
  const JNINativeMethod methods[] = {
          {"initializeNative", "()V", (void*)initializeNative},
          {"cleanupNative", "()V", (void*)cleanupNative},
          {"sdpSearchNative", "([B[B)Z", (void*)sdpSearchNative},
          {"sdpCreateMapMasRecordNative", "(Ljava/lang/String;IIIIII)I",
           (void*)sdpCreateMapMasRecordNative},
          {"sdpCreateMapMnsRecordNative", "(Ljava/lang/String;IIII)I",
           (void*)sdpCreateMapMnsRecordNative},
          {"sdpCreatePbapPceRecordNative", "(Ljava/lang/String;I)I",
           (void*)sdpCreatePbapPceRecordNative},
          {"sdpCreatePbapPseRecordNative", "(Ljava/lang/String;IIIII)I",
           (void*)sdpCreatePbapPseRecordNative},
          {"sdpCreateOppOpsRecordNative", "(Ljava/lang/String;III[B)I",
           (void*)sdpCreateOppOpsRecordNative},
          {"sdpCreateSapsRecordNative", "(Ljava/lang/String;II)I",
           (void*)sdpCreateSapsRecordNative},
          {"sdpRemoveSdpRecordNative", "(I)Z", (void*)sdpRemoveSdpRecordNative},
  };
  const int result = REGISTER_NATIVE_METHODS(
          env, "com/android/bluetooth/sdp/SdpManagerNativeInterface", methods);
  if (result != 0) {
    return result;
  }

  const JNIJavaMethod javaMethods[] = {
          {"sdpRecordFoundCallback", "(I[B[BI[B)V", &method_sdpRecordFoundCallback},
          {"sdpMasRecordFoundCallback", "(I[B[BIIIIIILjava/lang/String;Z)V",
           &method_sdpMasRecordFoundCallback},
          {"sdpMnsRecordFoundCallback", "(I[B[BIIIILjava/lang/String;Z)V",
           &method_sdpMnsRecordFoundCallback},
          {"sdpPseRecordFoundCallback", "(I[B[BIIIIILjava/lang/String;Z)V",
           &method_sdpPseRecordFoundCallback},
          {"sdpOppOpsRecordFoundCallback", "(I[B[BIIILjava/lang/String;[BZ)V",
           &method_sdpOppOpsRecordFoundCallback},
          {"sdpSapsRecordFoundCallback", "(I[B[BIILjava/lang/String;Z)V",
           &method_sdpSapsRecordFoundCallback},
          {"sdpDipRecordFoundCallback", "(I[B[BIIIIIZZ)V", &method_sdpDipRecordFoundCallback},
  };
  GET_JAVA_METHODS(env, "com/android/bluetooth/sdp/SdpManagerNativeInterface", javaMethods);

  return 0;
}
}  // namespace android
