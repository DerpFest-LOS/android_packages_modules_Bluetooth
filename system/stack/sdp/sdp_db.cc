/******************************************************************************
 *
 *  Copyright (C) 2017, The Linux Foundation.
 *  Copyright 1999-2012 Broadcom Corporation
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

/******************************************************************************
 *
 *  this file contains functions that handle the database
 *
 ******************************************************************************/

#define LOG_TAG "stack::sdp"

#include <bluetooth/log.h>
#include <string.h>

#include <cstdint>

#include "internal_include/bt_target.h"
#include "osi/include/allocator.h"
#include "stack/include/bt_types.h"
#include "stack/include/bt_uuid16.h"
#include "stack/include/sdpdefs.h"
#include "stack/sdp/internal/sdp_api.h"
#include "stack/sdp/sdp_discovery_db.h"
#include "stack/sdp/sdpint.h"

using namespace bluetooth;

/*******************************************************************************
 *
 * Function         find_uuid_in_seq
 *
 * Description      This function searches a data element sequenct for a UUID.
 *
 * Returns          true if found, else false
 *
 ******************************************************************************/
static bool find_uuid_in_seq(uint8_t* p, uint32_t seq_len, const uint8_t* p_uuid, uint16_t uuid_len,
                             int nest_level) {
  uint8_t* p_end = p + seq_len;
  uint8_t type;
  uint32_t len;

  /* A little safety check to avoid excessive recursion */
  if (nest_level > 3) {
    return false;
  }

  while (p < p_end) {
    type = *p++;
    p = sdpu_get_len_from_type(p, p_end, type, &len);
    if (p == NULL || (p + len) > p_end) {
      log::warn("bad length");
      break;
    }
    type = type >> 3;
    if (type == UUID_DESC_TYPE) {
      if (sdpu_compare_uuid_arrays(p, len, p_uuid, uuid_len)) {
        return true;
      }
    } else if (type == DATA_ELE_SEQ_DESC_TYPE) {
      if (find_uuid_in_seq(p, len, p_uuid, uuid_len, nest_level + 1)) {
        return true;
      }
    }
    p = p + len;
  }

  /* If here, failed to match */
  return false;
}

/*******************************************************************************
 *
 * Function         sdp_db_service_search
 *
 * Description      This function searches for a record that contains the
 *                  specified UIDs. It is passed either NULL to start at the
 *                  beginning, or the previous record found.
 *
 * Returns          Pointer to the record, or NULL if not found.
 *
 ******************************************************************************/
const tSDP_RECORD* sdp_db_service_search(const tSDP_RECORD* p_rec, const tSDP_UUID_SEQ* p_seq) {
  uint16_t xx, yy;
  const tSDP_ATTRIBUTE* p_attr;
  tSDP_RECORD* p_end = &sdp_cb.server_db.record[sdp_cb.server_db.num_records];

  /* If NULL, start at the beginning, else start at the first specified record
   */
  if (!p_rec) {
    p_rec = &sdp_cb.server_db.record[0];
  } else {
    p_rec++;
  }

  /* Look through the records. The spec says that a match occurs if */
  /* the record contains all the passed UUIDs in it.                */
  for (; p_rec < p_end; p_rec++) {
    for (yy = 0; yy < p_seq->num_uids; yy++) {
      p_attr = &p_rec->attribute[0];
      for (xx = 0; xx < p_rec->num_attributes; xx++, p_attr++) {
        if (p_attr->type == UUID_DESC_TYPE) {
          if (sdpu_compare_uuid_arrays(p_attr->value_ptr, p_attr->len,
                                       &p_seq->uuid_entry[yy].value[0],
                                       p_seq->uuid_entry[yy].len)) {
            break;
          }
        } else if (p_attr->type == DATA_ELE_SEQ_DESC_TYPE) {
          if (find_uuid_in_seq(p_attr->value_ptr, p_attr->len, &p_seq->uuid_entry[yy].value[0],
                               p_seq->uuid_entry[yy].len, 0)) {
            break;
          }
        }
      }
      /* If any UUID was not found,  on to the next record */
      if (xx == p_rec->num_attributes) {
        break;
      }
    }

    /* If every UUID was found in the record, return the record */
    if (yy == p_seq->num_uids) {
      return p_rec;
    }
  }

  /* If here, no more records found */
  return NULL;
}

/*******************************************************************************
 *
 * Function         sdp_db_find_record
 *
 * Description      This function searches for a record with a specific handle
 *                  It is passed the handle of the record.
 *
 * Returns          Pointer to the record, or NULL if not found.
 *
 ******************************************************************************/
tSDP_RECORD* sdp_db_find_record(uint32_t handle) {
  tSDP_RECORD* p_rec;
  tSDP_RECORD* p_end = &sdp_cb.server_db.record[sdp_cb.server_db.num_records];

  /* Look through the records for the caller's handle */
  for (p_rec = &sdp_cb.server_db.record[0]; p_rec < p_end; p_rec++) {
    if (p_rec->record_handle == handle) {
      return p_rec;
    }
  }

  /* Record with that handle not found. */
  return NULL;
}

/*******************************************************************************
 *
 * Function         sdp_db_find_attr_in_rec
 *
 * Description      This function searches a record for specific attributes.
 *                  It is passed a pointer to the record. If the record contains
 *                  the specified attribute, (the caller may specify be a range
 *                  of attributes), the attribute is returned.
 *
 * Returns          Pointer to the attribute, or NULL if not found.
 *
 ******************************************************************************/
const tSDP_ATTRIBUTE* sdp_db_find_attr_in_rec(const tSDP_RECORD* p_rec, uint16_t start_attr,
                                              uint16_t end_attr) {
  const tSDP_ATTRIBUTE* p_at;
  uint16_t xx;

  /* Note that the attributes in a record are assumed to be in sorted order */
  for (xx = 0, p_at = &p_rec->attribute[0]; xx < p_rec->num_attributes; xx++, p_at++) {
    if ((p_at->id >= start_attr) && (p_at->id <= end_attr)) {
      return p_at;
    }
  }

  /* No matching attribute found */
  return NULL;
}

/*******************************************************************************
 *
 * Function         sdp_compose_proto_list
 *
 * Description      This function is called to compose a data sequence from
 *                  protocol element list struct pointer
 *
 * Returns          the length of the data sequence
 *
 ******************************************************************************/
static int sdp_compose_proto_list(uint8_t* p, uint16_t num_elem, tSDP_PROTOCOL_ELEM* p_elem_list) {
  uint16_t xx, yy, len;
  bool is_rfcomm_scn;
  uint8_t* p_head = p;
  uint8_t* p_len;

  /* First, build the protocol list. This consists of a set of data element
  ** sequences, one for each layer. Each layer sequence consists of layer's
  ** UUID and optional parameters
  */
  for (xx = 0; xx < num_elem; xx++, p_elem_list++) {
    len = 3 + (p_elem_list->num_params * 3);
    UINT8_TO_BE_STREAM(p, (DATA_ELE_SEQ_DESC_TYPE << 3) | SIZE_IN_NEXT_BYTE);

    p_len = p;
    *p++ = (uint8_t)len;

    UINT8_TO_BE_STREAM(p, (UUID_DESC_TYPE << 3) | SIZE_TWO_BYTES);
    UINT16_TO_BE_STREAM(p, p_elem_list->protocol_uuid);

    if (p_elem_list->protocol_uuid == UUID_PROTOCOL_RFCOMM) {
      is_rfcomm_scn = true;
    } else {
      is_rfcomm_scn = false;
    }

    for (yy = 0; yy < p_elem_list->num_params; yy++) {
      if (is_rfcomm_scn) {
        UINT8_TO_BE_STREAM(p, (UINT_DESC_TYPE << 3) | SIZE_ONE_BYTE);
        UINT8_TO_BE_STREAM(p, p_elem_list->params[yy]);

        *p_len -= 1;
      } else {
        UINT8_TO_BE_STREAM(p, (UINT_DESC_TYPE << 3) | SIZE_TWO_BYTES);
        UINT16_TO_BE_STREAM(p, p_elem_list->params[yy]);
      }
    }
  }
  return p - p_head;
}

/*******************************************************************************
 *
 * Function         SDP_AddAttribute
 *
 * Description      This function is called to add an attribute to a record.
 *                  This would be through the SDP database maintenance API.
 *                  If the attribute already exists in the record, it is
 *                  replaced with the new value.
 *
 * NOTE             Attribute values must be passed as a Big Endian stream.
 *
 * Returns          true if added OK, else false
 *
 ******************************************************************************/
bool SDP_AddAttribute(uint32_t handle, uint16_t attr_id, uint8_t attr_type, uint32_t attr_len,
                      uint8_t* p_val) {
  uint16_t zz;
  tSDP_RECORD* p_rec = &sdp_cb.server_db.record[0];

  if (p_val == nullptr) {
    log::warn("Trying to add attribute with p_val == nullptr, skipped");
    return false;
  }

  // TODO(305066880): invoke would_log when implemented to check
  // if LOG_VERBOSE is displayed.
  if (true) {
    if ((attr_type == UINT_DESC_TYPE) || (attr_type == TWO_COMP_INT_DESC_TYPE) ||
        (attr_type == UUID_DESC_TYPE) || (attr_type == DATA_ELE_SEQ_DESC_TYPE) ||
        (attr_type == DATA_ELE_ALT_DESC_TYPE)) {
#define MAX_ARR_LEN 200
      // one extra byte for storing terminating zero byte
      char num_array[2 * MAX_ARR_LEN + 1] = {0};
      uint32_t len = (attr_len > MAX_ARR_LEN) ? MAX_ARR_LEN : attr_len;
#undef MAX_ARR_LEN

      for (uint32_t i = 0; i < len; i++) {
        snprintf(&num_array[i * 2], sizeof(num_array) - i * 2, "%02X", (uint8_t)(p_val[i]));
      }
      log::verbose("SDP_AddAttribute: handle:{:X}, id:{:04X}, type:{}, len:{}, p_val:{}, *p_val:{}",
                   handle, attr_id, attr_type, attr_len, std::format_ptr(p_val), num_array);
    } else if (attr_type == BOOLEAN_DESC_TYPE) {
      log::verbose("SDP_AddAttribute: handle:{:X}, id:{:04X}, type:{}, len:{}, p_val:{}, *p_val:{}",
                   handle, attr_id, attr_type, attr_len, std::format_ptr(p_val), *p_val);
    } else if ((attr_type == TEXT_STR_DESC_TYPE) || (attr_type == URL_DESC_TYPE)) {
      if (p_val[attr_len - 1] == '\0') {
        log::verbose(
                "SDP_AddAttribute: handle:{:X}, id:{:04X}, type:{}, len:{}, p_val:{}, *p_val:{}",
                handle, attr_id, attr_type, attr_len, std::format_ptr(p_val), (char*)p_val);
      } else {
        log::verbose("SDP_AddAttribute: handle:{:X}, id:{:04X}, type:{}, len:{}, p_val:{}", handle,
                     attr_id, attr_type, attr_len, std::format_ptr(p_val));
      }
    } else {
      log::verbose("SDP_AddAttribute: handle:{:X}, id:{:04X}, type:{}, len:{}, p_val:{}", handle,
                   attr_id, attr_type, attr_len, std::format_ptr(p_val));
    }
  }

  /* Find the record in the database */
  for (zz = 0; zz < sdp_cb.server_db.num_records; zz++, p_rec++) {
    if (p_rec->record_handle == handle) {
      // error out early, no need to look up
      if (p_rec->free_pad_ptr >= SDP_MAX_PAD_LEN) {
        log::error("the free pad for SDP record with handle {} is full, skip adding the attribute",
                   handle);
        return false;
      }

      return SDP_AddAttributeToRecord(p_rec, attr_id, attr_type, attr_len, p_val);
    }
  }
  return false;
}

/*******************************************************************************
 *
 * Function         SDP_CreateRecord
 *
 * Description      This function is called to create a record in the database.
 *                  This would be through the SDP database maintenance API. The
 *                  record is created empty, the application should then call
 *                  "add_attribute" to add the record's attributes.
 *
 * Returns          Record handle if OK, else 0.
 *
 ******************************************************************************/
uint32_t SDP_CreateRecord(void) {
  uint32_t handle;
  uint8_t buf[4];
  tSDP_DB* p_db = &sdp_cb.server_db;

  /* First, check if there is a free record */
  if (p_db->num_records >= SDP_MAX_RECORDS) {
    log::error("SDP_CreateRecord fail, exceed maximum records:{}", SDP_MAX_RECORDS);
    return 0;
  }

  memset(&p_db->record[p_db->num_records], 0, sizeof(tSDP_RECORD));

  /* We will use a handle of the first unreserved handle plus last record
  ** number + 1 */
  if (p_db->num_records) {
    handle = p_db->record[p_db->num_records - 1].record_handle + 1;
  } else {
    handle = 0x10000;
  }

  p_db->record[p_db->num_records].record_handle = handle;

  p_db->num_records++;
  log::verbose("SDP_CreateRecord ok, num_records:{}", p_db->num_records);
  /* Add the first attribute (the handle) automatically */
  UINT32_TO_BE_FIELD(buf, handle);
  SDP_AddAttribute(handle, ATTR_ID_SERVICE_RECORD_HDL, UINT_DESC_TYPE, 4, buf);

  return p_db->record[p_db->num_records - 1].record_handle;
}

/*******************************************************************************
 *
 * Function         SDP_DeleteRecord
 *
 * Description      This function is called to add a record (or all records)
 *                  from the database. This would be through the SDP database
 *                  maintenance API.
 *
 *                  If a record handle of 0 is passed, all records are deleted.
 *
 * Returns          true if succeeded, else false
 *
 ******************************************************************************/
bool SDP_DeleteRecord(uint32_t handle) {
  uint16_t xx, yy, zz;
  tSDP_RECORD* p_rec = &sdp_cb.server_db.record[0];

  if (handle == 0 || sdp_cb.server_db.num_records == 0) {
    /* Delete all records in the database */
    sdp_cb.server_db.num_records = 0;

    /* require new DI record to be created in SDP_SetLocalDiRecord */
    sdp_cb.server_db.di_primary_handle = 0;

    return true;
  }

  /* Find the record in the database */
  for (xx = 0; xx < sdp_cb.server_db.num_records; xx++, p_rec++) {
    if (p_rec->record_handle != handle) {
      continue;
    }

    /* Found it. Shift everything up one */
    for (yy = xx; yy < sdp_cb.server_db.num_records - 1; yy++, p_rec++) {
      *p_rec = *(p_rec + 1);

      /* Adjust the attribute value pointer for each attribute */
      for (zz = 0; zz < p_rec->num_attributes; zz++) {
        p_rec->attribute[zz].value_ptr -= sizeof(tSDP_RECORD);
      }
    }

    sdp_cb.server_db.num_records--;

    log::verbose("SDP_DeleteRecord ok, num_records:{}", sdp_cb.server_db.num_records);
    /* if we're deleting the primary DI record, clear the */
    /* value in the control block */
    if (sdp_cb.server_db.di_primary_handle == handle) {
      sdp_cb.server_db.di_primary_handle = 0;
    }

    return true;
  }
  return false;
}

/*******************************************************************************
 *
 * Function         SDP_AddAttributeToRecord
 *
 * Description      This function is called to add an attribute to a record.
 *                  This would be through the SDP database maintenance API.
 *                  If the attribute already exists in the record, it is
 *                  replaced with the new value.
 *
 * NOTE             Attribute values must be passed as a Big Endian stream.
 *
 * Returns          true if added OK, else false
 *
 ******************************************************************************/
bool SDP_AddAttributeToRecord(tSDP_RECORD* p_rec, uint16_t attr_id, uint8_t attr_type,
                              uint32_t attr_len, uint8_t* p_val) {
  uint16_t xx, yy;
  tSDP_ATTRIBUTE* p_attr = &p_rec->attribute[0];

  /* Found the record. Now, see if the attribute already exists */
  for (xx = 0; xx < p_rec->num_attributes; xx++, p_attr++) {
    /* The attribute exists. replace it */
    if (p_attr->id == attr_id) {
      SDP_DeleteAttributeFromRecord(p_rec, attr_id);
      break;
    }
    if (p_attr->id > attr_id) {
      break;
    }
  }

  if (p_rec->num_attributes >= SDP_MAX_REC_ATTR) {
    return false;
  }

  /* If not found, see if we can allocate a new entry */
  if (xx == p_rec->num_attributes) {
    p_attr = &p_rec->attribute[p_rec->num_attributes];
  } else {
    /* Since the attributes are kept in sorted order, insert ours here */
    for (yy = p_rec->num_attributes; yy > xx; yy--) {
      p_rec->attribute[yy] = p_rec->attribute[yy - 1];
    }
  }

  p_attr->id = attr_id;
  p_attr->type = attr_type;
  p_attr->len = attr_len;

  if (p_rec->free_pad_ptr + attr_len >= SDP_MAX_PAD_LEN) {
    if (p_rec->free_pad_ptr >= SDP_MAX_PAD_LEN) {
      log::error(
              "SDP_AddAttributeToRecord failed: free pad {} equals or exceeds max "
              "padding length {}",
              p_rec->free_pad_ptr, SDP_MAX_PAD_LEN);
      return false;
    }

    /* do truncate only for text string type descriptor */
    if (attr_type == TEXT_STR_DESC_TYPE) {
      log::warn("SDP_AddAttributeToRecord: attr_len:{} too long. truncate to ({})", attr_len,
                SDP_MAX_PAD_LEN - p_rec->free_pad_ptr);

      attr_len = SDP_MAX_PAD_LEN - p_rec->free_pad_ptr;
      p_val[SDP_MAX_PAD_LEN - p_rec->free_pad_ptr - 1] = '\0';
    } else {
      attr_len = 0;
    }
  }

  if (attr_len > 0) {
    p_attr->len = attr_len;
    memcpy(&p_rec->attr_pad[p_rec->free_pad_ptr], p_val, (size_t)attr_len);
    p_attr->value_ptr = &p_rec->attr_pad[p_rec->free_pad_ptr];
    p_rec->free_pad_ptr += attr_len;
  } else if (attr_len == 0 && p_attr->len != 0) {
    /* if truncate to 0 length, simply don't add */
    log::error("SDP_AddAttributeToRecord fail, length exceed maximum: ID {}: attr_len:{}", attr_id,
               attr_len);
    p_attr->id = p_attr->type = p_attr->len = 0;
    return false;
  }
  p_rec->num_attributes++;
  return true;
}

/*******************************************************************************
 *
 * Function         SDP_AddSequence
 *
 * Description      This function is called to add a sequence to a record.
 *                  This would be through the SDP database maintenance API.
 *                  If the sequence already exists in the record, it is replaced
 *                  with the new sequence.
 *
 * NOTE             Element values must be passed as a Big Endian stream.
 *
 * Returns          true if added OK, else false
 *
 ******************************************************************************/
bool SDP_AddSequence(uint32_t handle, uint16_t attr_id, uint16_t num_elem, uint8_t type[],
                     uint8_t len[], uint8_t* p_val[]) {
  uint16_t xx;
  uint8_t* p;
  uint8_t* p_head;
  bool result;
  uint8_t* p_buff = (uint8_t*)osi_malloc(sizeof(uint8_t) * SDP_MAX_ATTR_LEN * 2);

  p = p_buff;

  /* First, build the sequence */
  for (xx = 0; xx < num_elem; xx++) {
    p_head = p;
    switch (len[xx]) {
      case 1:
        UINT8_TO_BE_STREAM(p, (type[xx] << 3) | SIZE_ONE_BYTE);
        break;
      case 2:
        UINT8_TO_BE_STREAM(p, (type[xx] << 3) | SIZE_TWO_BYTES);
        break;
      case 4:
        UINT8_TO_BE_STREAM(p, (type[xx] << 3) | SIZE_FOUR_BYTES);
        break;
      case 8:
        UINT8_TO_BE_STREAM(p, (type[xx] << 3) | SIZE_EIGHT_BYTES);
        break;
      case 16:
        UINT8_TO_BE_STREAM(p, (type[xx] << 3) | SIZE_SIXTEEN_BYTES);
        break;
      default:
        UINT8_TO_BE_STREAM(p, (type[xx] << 3) | SIZE_IN_NEXT_BYTE);
        UINT8_TO_BE_STREAM(p, len[xx]);
        break;
    }

    ARRAY_TO_BE_STREAM(p, p_val[xx], len[xx]);

    if (p - p_buff > SDP_MAX_ATTR_LEN) {
      /* go back to before we add this element */
      p = p_head;
      if (p_head == p_buff) {
        /* the first element exceed the max length */
        log::error("SDP_AddSequence - too long(attribute is not added)!!");
        osi_free(p_buff);
        return false;
      } else {
        log::error("SDP_AddSequence - too long, add {} elements of {}", xx, num_elem);
      }
      break;
    }
  }
  result =
          SDP_AddAttribute(handle, attr_id, DATA_ELE_SEQ_DESC_TYPE, (uint32_t)(p - p_buff), p_buff);
  osi_free(p_buff);
  return result;
}

/*******************************************************************************
 *
 * Function         SDP_AddUuidSequence
 *
 * Description      This function is called to add a UUID sequence to a record.
 *                  This would be through the SDP database maintenance API.
 *                  If the sequence already exists in the record, it is replaced
 *                  with the new sequence.
 *
 * Returns          true if added OK, else false
 *
 ******************************************************************************/
bool SDP_AddUuidSequence(uint32_t handle, uint16_t attr_id, uint16_t num_uuids, uint16_t* p_uuids) {
  uint16_t xx;
  uint8_t* p;
  int32_t max_len = SDP_MAX_ATTR_LEN - 3;
  bool result;
  uint8_t* p_buff = (uint8_t*)osi_malloc(sizeof(uint8_t) * SDP_MAX_ATTR_LEN * 2);

  p = p_buff;

  /* First, build the sequence */
  for (xx = 0; xx < num_uuids; xx++, p_uuids++) {
    UINT8_TO_BE_STREAM(p, (UUID_DESC_TYPE << 3) | SIZE_TWO_BYTES);
    UINT16_TO_BE_STREAM(p, *p_uuids);

    if ((p - p_buff) > max_len) {
      log::warn("SDP_AddUuidSequence - too long, add {} uuids of {}", xx, num_uuids);
      break;
    }
  }

  result =
          SDP_AddAttribute(handle, attr_id, DATA_ELE_SEQ_DESC_TYPE, (uint32_t)(p - p_buff), p_buff);
  osi_free(p_buff);
  return result;
}

/*******************************************************************************
 *
 * Function         SDP_AddProtocolList
 *
 * Description      This function is called to add a protocol descriptor list to
 *                  a record. This would be through the SDP database
 *                  maintenance API. If the protocol list already exists in the
 *                  record, it is replaced with the new list.
 *
 * Returns          true if added OK, else false
 *
 ******************************************************************************/
bool SDP_AddProtocolList(uint32_t handle, uint16_t num_elem, tSDP_PROTOCOL_ELEM* p_elem_list) {
  int offset;
  bool result;
  uint8_t* p_buff = (uint8_t*)osi_malloc(sizeof(uint8_t) * SDP_MAX_ATTR_LEN * 2);

  offset = sdp_compose_proto_list(p_buff, num_elem, p_elem_list);
  result = SDP_AddAttribute(handle, ATTR_ID_PROTOCOL_DESC_LIST, DATA_ELE_SEQ_DESC_TYPE,
                            (uint32_t)offset, p_buff);
  osi_free(p_buff);
  return result;
}

/*******************************************************************************
 *
 * Function         SDP_AddAdditionProtoLists
 *
 * Description      This function is called to add a protocol descriptor list to
 *                  a record. This would be through the SDP database maintenance
 *                  API. If the protocol list already exists in the record, it
 *                  is replaced with the new list.
 *
 * Returns          true if added OK, else false
 *
 ******************************************************************************/
bool SDP_AddAdditionProtoLists(uint32_t handle, uint16_t num_elem,
                               tSDP_PROTO_LIST_ELEM* p_proto_list) {
  uint16_t xx;
  uint8_t* p;
  uint8_t* p_len;
  int offset;
  bool result;
  uint8_t* p_buff = (uint8_t*)osi_malloc(sizeof(uint8_t) * SDP_MAX_ATTR_LEN * 2);

  p = p_buff;

  /* for each ProtocolDescriptorList */
  for (xx = 0; xx < num_elem; xx++, p_proto_list++) {
    UINT8_TO_BE_STREAM(p, (DATA_ELE_SEQ_DESC_TYPE << 3) | SIZE_IN_NEXT_BYTE);
    p_len = p++;

    offset = sdp_compose_proto_list(p, p_proto_list->num_elems, p_proto_list->list_elem);
    p += offset;

    *p_len = (uint8_t)(p - p_len - 1);
  }
  result = SDP_AddAttribute(handle, ATTR_ID_ADDITION_PROTO_DESC_LISTS, DATA_ELE_SEQ_DESC_TYPE,
                            (uint32_t)(p - p_buff), p_buff);
  osi_free(p_buff);
  return result;
}

/*******************************************************************************
 *
 * Function         SDP_AddProfileDescriptorList
 *
 * Description      This function is called to add a profile descriptor list to
 *                  a record. This would be through the SDP database maintenance
 *                  API. If the version already exists in the record, it is
 *                  replaced with the new one.
 *
 * Returns          true if added OK, else false
 *
 ******************************************************************************/
bool SDP_AddProfileDescriptorList(uint32_t handle, uint16_t profile_uuid, uint16_t version) {
  uint8_t* p;
  bool result;
  uint8_t* p_buff = (uint8_t*)osi_malloc(sizeof(uint8_t) * SDP_MAX_ATTR_LEN);

  p = p_buff + 2;

  /* First, build the profile descriptor list. This consists of a data element
   * sequence. */
  /* The sequence consists of profile's UUID and version number  */
  UINT8_TO_BE_STREAM(p, (UUID_DESC_TYPE << 3) | SIZE_TWO_BYTES);
  UINT16_TO_BE_STREAM(p, profile_uuid);

  UINT8_TO_BE_STREAM(p, (UINT_DESC_TYPE << 3) | SIZE_TWO_BYTES);
  UINT16_TO_BE_STREAM(p, version);

  /* Add in type and length fields */
  *p_buff = (uint8_t)((DATA_ELE_SEQ_DESC_TYPE << 3) | SIZE_IN_NEXT_BYTE);
  *(p_buff + 1) = (uint8_t)(p - (p_buff + 2));

  result = SDP_AddAttribute(handle, ATTR_ID_BT_PROFILE_DESC_LIST, DATA_ELE_SEQ_DESC_TYPE,
                            (uint32_t)(p - p_buff), p_buff);
  osi_free(p_buff);
  return result;
}

/*******************************************************************************
 *
 * Function         SDP_AddProfileDescriptorListToRecord
 *
 * Description      This function is called to add a profile descriptor list to
 *                  a record. This would be through the SDP database maintenance
 *                  API. If the version already exists in the record, it is
 *                  replaced with the new one.
 *
 * Returns          true if added OK, else false
 *
 ******************************************************************************/
bool SDP_AddProfileDescriptorListToRecord(tSDP_RECORD* prec, uint16_t profile_uuid,
                                          uint16_t version) {
  uint8_t* p;
  bool result;
  uint8_t* p_buff = (uint8_t*)osi_malloc(sizeof(uint8_t) * SDP_MAX_ATTR_LEN);

  p = p_buff + 2;

  /* First, build the profile descriptor list. This consists of a data element
   * sequence. */
  /* The sequence consists of profile's UUID and version number  */
  UINT8_TO_BE_STREAM(p, (UUID_DESC_TYPE << 3) | SIZE_TWO_BYTES);
  UINT16_TO_BE_STREAM(p, profile_uuid);

  UINT8_TO_BE_STREAM(p, (UINT_DESC_TYPE << 3) | SIZE_TWO_BYTES);
  UINT16_TO_BE_STREAM(p, version);

  /* Add in type and length fields */
  *p_buff = (uint8_t)((DATA_ELE_SEQ_DESC_TYPE << 3) | SIZE_IN_NEXT_BYTE);
  *(p_buff + 1) = (uint8_t)(p - (p_buff + 2));

  result = SDP_AddAttributeToRecord(prec, ATTR_ID_BT_PROFILE_DESC_LIST, DATA_ELE_SEQ_DESC_TYPE,
                                    (uint32_t)(p - p_buff), p_buff);
  osi_free(p_buff);
  return result;
}

/*******************************************************************************
 *
 * Function         SDP_AddLanguageBaseAttrIDList
 *
 * Description      This function is called to add a language base attr list to
 *                  a record. This would be through the SDP database maintenance
 *                  API. If the version already exists in the record, it is
 *                  replaced with the new one.
 *
 * Returns          true if added OK, else false
 *
 ******************************************************************************/
bool SDP_AddLanguageBaseAttrIDList(uint32_t handle, uint16_t lang, uint16_t char_enc,
                                   uint16_t base_id) {
  uint8_t* p;
  bool result;
  uint8_t* p_buff = (uint8_t*)osi_malloc(sizeof(uint8_t) * SDP_MAX_ATTR_LEN);

  p = p_buff;

  /* First, build the language base descriptor list. This consists of a data */
  /* element sequence. The sequence consists of 9 bytes (3 UINt16 fields)    */
  UINT8_TO_BE_STREAM(p, (UINT_DESC_TYPE << 3) | SIZE_TWO_BYTES);
  UINT16_TO_BE_STREAM(p, lang);

  UINT8_TO_BE_STREAM(p, (UINT_DESC_TYPE << 3) | SIZE_TWO_BYTES);
  UINT16_TO_BE_STREAM(p, char_enc);

  UINT8_TO_BE_STREAM(p, (UINT_DESC_TYPE << 3) | SIZE_TWO_BYTES);
  UINT16_TO_BE_STREAM(p, base_id);

  result = SDP_AddAttribute(handle, ATTR_ID_LANGUAGE_BASE_ATTR_ID_LIST, DATA_ELE_SEQ_DESC_TYPE,
                            (uint32_t)(p - p_buff), p_buff);
  osi_free(p_buff);
  return result;
}

/*******************************************************************************
 *
 * Function         SDP_AddServiceClassIdList
 *
 * Description      This function is called to add a service list to a record.
 *                  This would be through the SDP database maintenance API.
 *                  If the service list already exists in the record, it is
 *                  replaced with the new list.
 *
 * Returns          true if added OK, else false
 *
 ******************************************************************************/
bool SDP_AddServiceClassIdList(uint32_t handle, uint16_t num_services, uint16_t* p_service_uuids) {
  uint16_t xx;
  uint8_t* p;
  bool result;
  uint8_t* p_buff = (uint8_t*)osi_malloc(sizeof(uint8_t) * SDP_MAX_ATTR_LEN * 2);

  p = p_buff;

  for (xx = 0; xx < num_services; xx++, p_service_uuids++) {
    UINT8_TO_BE_STREAM(p, (UUID_DESC_TYPE << 3) | SIZE_TWO_BYTES);
    UINT16_TO_BE_STREAM(p, *p_service_uuids);
  }

  result = SDP_AddAttribute(handle, ATTR_ID_SERVICE_CLASS_ID_LIST, DATA_ELE_SEQ_DESC_TYPE,
                            (uint32_t)(p - p_buff), p_buff);
  osi_free(p_buff);
  return result;
}

/*******************************************************************************
 *
 * Function         SDP_DeleteAttributeFromRecord
 *
 * Description      This function is called to delete an attribute from a
 *                  record. This would be through the SDP database maintenance
 *                  API.
 *
 * Returns          true if deleted OK, else false if not found
 *
 ******************************************************************************/

bool SDP_DeleteAttributeFromRecord(tSDP_RECORD* p_rec, uint16_t attr_id) {
  tSDP_ATTRIBUTE* p_attr = &p_rec->attribute[0];
  uint8_t* pad_ptr;
  uint32_t len; /* Number of bytes in the entry */

  /* Found it. Now, find the attribute */
  for (uint16_t attribute_index = 0; attribute_index < p_rec->num_attributes;
       attribute_index++, p_attr++) {
    if (p_attr->id == attr_id) {
      pad_ptr = p_attr->value_ptr;
      len = p_attr->len;

      if (len) {
        for (uint16_t zz = 0; zz < p_rec->num_attributes; zz++) {
          if (p_rec->attribute[zz].value_ptr > pad_ptr) {
            p_rec->attribute[zz].value_ptr -= len;
          }
        }
      }

      /* Found it. Shift everything up one */
      p_rec->num_attributes--;

      for (uint16_t zz = attribute_index; zz < p_rec->num_attributes; zz++, p_attr++) {
        *p_attr = *(p_attr + 1);
      }

      /* adjust attribute values if needed */
      if (len) {
        uint16_t last_attribute_to_adjust =
                (p_rec->free_pad_ptr - ((pad_ptr + len) - &p_rec->attr_pad[0]));
        for (uint16_t zz = 0; zz < last_attribute_to_adjust; zz++, pad_ptr++) {
          *pad_ptr = *(pad_ptr + len);
        }
        p_rec->free_pad_ptr -= len;
      }
      return true;
    }
  }
  /* If here, not found */
  return false;
}
