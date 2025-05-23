/*
 * Copyright 2022 The Android Open Source Project
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

#include <algorithm>
#include <cstdint>
#include <deque>
#include <memory>
#include <sstream>
#include <string>

#include "include/hardware/bluetooth.h"
#include "macros.h"
#include "types/bluetooth/uuid.h"

inline std::string bt_property_type_text(const ::bt_property_type_t type) {
  switch (type) {
    CASE_RETURN_TEXT(BT_PROPERTY_BDNAME);
    CASE_RETURN_TEXT(BT_PROPERTY_BDADDR);
    CASE_RETURN_TEXT(BT_PROPERTY_UUIDS);
    CASE_RETURN_TEXT(BT_PROPERTY_CLASS_OF_DEVICE);
    CASE_RETURN_TEXT(BT_PROPERTY_TYPE_OF_DEVICE);
    CASE_RETURN_TEXT(BT_PROPERTY_SERVICE_RECORD);
    CASE_RETURN_TEXT(BT_PROPERTY_ADAPTER_BONDED_DEVICES);
    CASE_RETURN_TEXT(BT_PROPERTY_ADAPTER_DISCOVERABLE_TIMEOUT);
    CASE_RETURN_TEXT(BT_PROPERTY_REMOTE_FRIENDLY_NAME);
    CASE_RETURN_TEXT(BT_PROPERTY_REMOTE_RSSI);
    CASE_RETURN_TEXT(BT_PROPERTY_REMOTE_VERSION_INFO);
    CASE_RETURN_TEXT(BT_PROPERTY_LOCAL_LE_FEATURES);
    CASE_RETURN_TEXT(BT_PROPERTY_RESERVED_0E);
    CASE_RETURN_TEXT(BT_PROPERTY_RESERVED_0F);
    CASE_RETURN_TEXT(BT_PROPERTY_DYNAMIC_AUDIO_BUFFER);
    CASE_RETURN_TEXT(BT_PROPERTY_REMOTE_IS_COORDINATED_SET_MEMBER);
    CASE_RETURN_TEXT(BT_PROPERTY_APPEARANCE);
    CASE_RETURN_TEXT(BT_PROPERTY_VENDOR_PRODUCT_INFO);
    CASE_RETURN_TEXT(BT_PROPERTY_REMOTE_ASHA_CAPABILITY);
    CASE_RETURN_TEXT(BT_PROPERTY_REMOTE_ASHA_TRUNCATED_HISYNCID);
    CASE_RETURN_TEXT(BT_PROPERTY_REMOTE_MODEL_NUM);
    CASE_RETURN_TEXT(BT_PROPERTY_REMOTE_DEVICE_TIMESTAMP);
    CASE_RETURN_TEXT(BT_PROPERTY_REMOTE_ADDR_TYPE);
    CASE_RETURN_TEXT(BT_PROPERTY_RESERVED_0x14);
    default:
      RETURN_UNKNOWN_TYPE_STRING(::bt_property_type_t, type);
  }
}

namespace bluetooth {
namespace test {
namespace headless {

struct bt_property_t {
  ::bt_property_type_t Type() const { return type; }

  virtual std::string ToString() const = 0;

  // TODO verify this prints as expected
  std::string ToRaw() {
    std::ostringstream oss;
    const uint8_t* p = data.get();
    for (size_t i = 0; i < sizeof(bt_property_t); i++, p++) {
      oss << "0x" << std::hex << *p << " ";
    }
    return oss.str();
  }

protected:
  bt_property_t(const uint8_t* data, const size_t len) {
    this->len = len;
    this->data = std::make_unique<uint8_t[]>(len);
    std::copy(data, data + len, this->data.get());
  }
  virtual ~bt_property_t() = default;

  std::unique_ptr<uint8_t[]> data;
  size_t len;
  ::bt_property_type_t type;
};

namespace property {

struct void_t : public bt_property_t {
  void_t(const uint8_t* data, const size_t len, int type) : bt_property_t(data, len) {
    this->type = (::bt_property_type_t)type;
  }

public:
  virtual std::string ToString() const override {
    return std::format("Unimplemented property type:{} name:{}", type, bt_property_type_text(type));
  }
};

struct uuid_t : public bt_property_t {
public:
  uuid_t(const uint8_t* data, const size_t len) : bt_property_t(data, len) {}

  std::deque<bluetooth::Uuid> get_uuids() const {
    std::deque<bluetooth::Uuid> uuids;
    bluetooth::Uuid* p_uuid = reinterpret_cast<bluetooth::Uuid*>(data.get());
    for (size_t i = 0; i < num_uuid(); i++, p_uuid++) {
      bluetooth::Uuid uuid =
              bluetooth::Uuid::From128BitBE(reinterpret_cast<const uint8_t*>(p_uuid));
      uuids.push_back(uuid);
    }
    return uuids;
  }

  virtual std::string ToString() const override {
    return std::format("Number of uuids:{}", get_uuids().size());
  }

private:
  size_t num_uuid() const { return len / sizeof(bluetooth::Uuid); }
};

struct name_t : public bt_property_t {
  name_t(const uint8_t* data, const size_t len) : bt_property_t(data, len) {
    type = BT_PROPERTY_BDNAME;
  }

  std::string get_name() const {
    char* s = reinterpret_cast<char*>(data.get());
    return std::string(s);
  }

  virtual std::string ToString() const override { return std::format("Name:{}", get_name()); }
};

struct bdaddr_t : public bt_property_t {
  bdaddr_t(const uint8_t* data, const size_t len) : bt_property_t(data, len) {
    type = BT_PROPERTY_BDNAME;
  }

  RawAddress get_addr() const {
    uint8_t* s = reinterpret_cast<uint8_t*>(data.get());
    // TODO This may need to be reversed
    RawAddress bd_addr;
    log::assert_that(6U == bd_addr.FromOctets(s), "Mac address is not 6 bytes");
    return bd_addr;
  }

  virtual std::string ToString() const override {
    return std::format("bd_addr:{}", get_addr().ToString());
  }
};

struct class_of_device_t : public bt_property_t {
  class_of_device_t(const uint8_t* data, const size_t len) : bt_property_t(data, len) {
    type = BT_PROPERTY_CLASS_OF_DEVICE;
  }

  uint32_t get_class_of_device() const {
    uint32_t* cod = reinterpret_cast<uint32_t*>(data.get());
    return *cod;
  }

  virtual std::string ToString() const override {
    return std::format("cod:0x{:04x}", get_class_of_device());
  }
};

struct type_of_device_t : public bt_property_t {
  type_of_device_t(const uint8_t* data, const size_t len) : bt_property_t(data, len) {
    type = BT_PROPERTY_TYPE_OF_DEVICE;
  }

  uint32_t get_type_of_device() const {
    uint32_t* tod = reinterpret_cast<uint32_t*>(data.get());
    return *tod;
  }

  virtual std::string ToString() const override {
    return std::format("tod:0x{:04x}", get_type_of_device());
  }
};

}  // namespace property

bluetooth::test::headless::bt_property_t* property_factory(const ::bt_property_t& bt_property);

template <typename T>
T* get_property_type(bluetooth::test::headless::bt_property_t* bt_property) {
  return static_cast<T*>(bt_property);
}

}  // namespace headless
}  // namespace test
}  // namespace bluetooth
