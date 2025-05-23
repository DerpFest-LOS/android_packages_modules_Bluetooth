/******************************************************************************
 *
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
 *  this file contains SDP interface functions
 *
 ******************************************************************************/

#define LOG_TAG "stack::sdp"

#include "stack/include/sdp_api.h"

#include <base/strings/stringprintf.h>
#include <bluetooth/log.h>
#include <string.h>

#include <cstdint>
#include <utility>

#include "internal_include/bt_target.h"
#include "main/shim/dumpsys.h"
#include "stack/include/bt_types.h"
#include "stack/include/bt_uuid16.h"
#include "stack/include/sdpdefs.h"
#include "stack/sdp/internal/sdp_api.h"
#include "stack/sdp/sdpint.h"
#include "types/bluetooth/uuid.h"
#include "types/raw_address.h"

using bluetooth::Uuid;
using namespace bluetooth;

namespace {
constexpr unsigned kMaxSdpConnections = static_cast<unsigned>(SDP_MAX_CONNECTIONS);
constexpr unsigned kMaxSdpRecords = static_cast<unsigned>(SDP_MAX_DISC_SERVER_RECS);
}  // namespace

/*******************************************************************************
 *
 * Function         SDP_InitDiscoveryDb
 *
 * Description      This function is called to initialize a discovery database.
 *
 * Parameters:      p_db        - (input) address of an area of memory where the
 *                                        discovery database is managed.
 *                  len         - (input) size (in bytes) of the memory
 *                                 NOTE: This must be larger than
 *                                       sizeof(tSDP_DISCOVERY_DB)
 *                  num_uuid    - (input) number of UUID filters applied
 *                  p_uuid_list - (input) list of UUID filters
 *                  num_attr    - (input) number of attribute filters applied
 *                  p_attr_list - (input) list of attribute filters
 *
 *
 * Returns          bool
 *                          true if successful
 *                          false if one or more parameters are bad
 *
 ******************************************************************************/
bool SDP_InitDiscoveryDb(tSDP_DISCOVERY_DB* p_db, uint32_t len, uint16_t num_uuid,
                         const Uuid* p_uuid_list, uint16_t num_attr, const uint16_t* p_attr_list) {
  uint16_t xx;

  /* verify the parameters */
  if (p_db == NULL || (sizeof(tSDP_DISCOVERY_DB) > len) || num_attr > SDP_MAX_ATTR_FILTERS ||
      num_uuid > SDP_MAX_UUID_FILTERS) {
    log::error("SDP_InitDiscoveryDb Illegal param: p_db {}, len {}, num_uuid {}, num_attr {}",
               std::format_ptr(p_db), len, num_uuid, num_attr);

    return false;
  }

  memset(p_db, 0, static_cast<size_t>(len));

  p_db->mem_size = len - sizeof(tSDP_DISCOVERY_DB);
  p_db->mem_free = p_db->mem_size;
  p_db->p_first_rec = NULL;
  p_db->p_free_mem = (uint8_t*)(p_db + 1);

  for (xx = 0; xx < num_uuid; xx++) {
    p_db->uuid_filters[xx] = *p_uuid_list++;
  }

  p_db->num_uuid_filters = num_uuid;

  for (xx = 0; xx < num_attr; xx++) {
    p_db->attr_filters[xx] = *p_attr_list++;
  }

  /* sort attributes */
  sdpu_sort_attr_list(num_attr, p_db);

  p_db->num_attr_filters = num_attr;
  return true;
}

/*******************************************************************************
 *
 * Function         SDP_CancelServiceSearch
 *
 * Description      This function cancels an active query to an SDP server.
 *
 * Returns          true if discovery cancelled, false if a matching activity is
 *                  not found.
 *
 ******************************************************************************/
bool SDP_CancelServiceSearch(const tSDP_DISCOVERY_DB* p_db) {
  tCONN_CB* p_ccb = sdpu_find_ccb_by_db(p_db);
  if (!p_ccb) {
    return false;
  }

  sdp_disconnect(p_ccb, tSDP_STATUS::SDP_CANCEL);
  p_ccb->disc_state = SDP_DISC_WAIT_CANCEL;
  return true;
}

/*******************************************************************************
 *
 * Function         SDP_ServiceSearchRequest
 *
 * Description      This function queries an SDP server for information.
 *
 * Returns          true if discovery started, false if failed.
 *
 ******************************************************************************/
bool SDP_ServiceSearchRequest(const RawAddress& bd_addr, tSDP_DISCOVERY_DB* p_db,
                              tSDP_DISC_CMPL_CB* p_cb) {
  /* Specific BD address */
  tCONN_CB* p_ccb = sdp_conn_originate(bd_addr);
  if (!p_ccb) {
    log::warn("no spare CCB for peer:{} max:{}", bd_addr, kMaxSdpConnections);
    sdpu_dump_all_ccb();
    return false;
  }

  p_ccb->disc_state = SDP_DISC_WAIT_CONN;
  p_ccb->p_db = p_db;
  p_ccb->p_cb = p_cb;

  return true;
}

/*******************************************************************************
 *
 * Function         SDP_ServiceSearchAttributeRequest
 *
 * Description      This function queries an SDP server for information.
 *
 *                  The difference between this API function and the function
 *                  SDP_ServiceSearchRequest is that this one does a
 *                  combined ServiceSearchAttributeRequest SDP function.
 *                  (This is for Unplug Testing)
 *
 * Returns          true if discovery started, false if failed.
 *
 ******************************************************************************/
bool SDP_ServiceSearchAttributeRequest(const RawAddress& bd_addr, tSDP_DISCOVERY_DB* p_db,
                                       tSDP_DISC_CMPL_CB* p_cb) {
  /* Specific BD address */
  tCONN_CB* p_ccb = sdp_conn_originate(bd_addr);
  if (!p_ccb) {
    log::warn("no spare CCB for peer:{} max:{}", bd_addr, kMaxSdpConnections);
    sdpu_dump_all_ccb();
    return false;
  }

  p_ccb->disc_state = SDP_DISC_WAIT_CONN;
  p_ccb->p_db = p_db;
  p_ccb->p_cb = p_cb;

  p_ccb->is_attr_search = true;

  return true;
}
/*******************************************************************************
 *
 * Function         SDP_ServiceSearchAttributeRequest2
 *
 * Description      This function queries an SDP server for information.
 *
 *                  The difference between this API function and the function
 *                  SDP_ServiceSearchRequest is that this one does a
 *                  combined ServiceSearchAttributeRequest SDP function.
 *                  (This is for Unplug Testing)
 *
 * Returns          true if discovery started, false if failed.
 *
 ******************************************************************************/
bool SDP_ServiceSearchAttributeRequest2(
        const RawAddress& bd_addr, tSDP_DISCOVERY_DB* p_db,
        base::RepeatingCallback<tSDP_DISC_CMPL_CB> complete_callback) {
  /* Specific BD address */
  tCONN_CB* p_ccb = sdp_conn_originate(bd_addr);
  if (!p_ccb) {
    log::warn("no spare CCB for peer:{} max:{}", bd_addr, kMaxSdpConnections);
    sdpu_dump_all_ccb();
    return false;
  }

  p_ccb->disc_state = SDP_DISC_WAIT_CONN;
  p_ccb->p_db = p_db;
  p_ccb->complete_callback = std::move(complete_callback);

  p_ccb->is_attr_search = true;

  return true;
}

/*******************************************************************************
 *
 * Function         SDP_FindAttributeInRec
 *
 * Description      This function searches an SDP discovery record for a
 *                  specific attribute.
 *
 * Returns          Pointer to matching attribute entry, or NULL
 *
 ******************************************************************************/
tSDP_DISC_ATTR* SDP_FindAttributeInRec(const tSDP_DISC_REC* p_rec, uint16_t attr_id) {
  tSDP_DISC_ATTR* p_attr;

  p_attr = p_rec->p_first_attr;
  while (p_attr) {
    if (p_attr->attr_id == attr_id) {
      return p_attr;
    }

    p_attr = p_attr->p_next_attr;
  }

  /* If here, no matching attribute found */
  return NULL;
}

/*******************************************************************************
 *
 * Function         SDP_FindServiceUUIDInRec
 *
 * Description      This function is called to read the service UUID within a
 *                  record if there is any.
 *
 * Parameters:      p_rec      - pointer to a SDP record.
 *                  p_uuid     - output parameter to save the UUID found.
 *
 * Returns          true if found, otherwise false.
 *
 ******************************************************************************/
bool SDP_FindServiceUUIDInRec(const tSDP_DISC_REC* p_rec, Uuid* p_uuid) {
  tSDP_DISC_ATTR *p_attr, *p_sattr, *p_extra_sattr;

  p_attr = p_rec->p_first_attr;

  while (p_attr) {
    if ((p_attr->attr_id == ATTR_ID_SERVICE_CLASS_ID_LIST) &&
        (SDP_DISC_ATTR_TYPE(p_attr->attr_len_type) == DATA_ELE_SEQ_DESC_TYPE)) {
      for (p_sattr = p_attr->attr_value.v.p_sub_attr; p_sattr; p_sattr = p_sattr->p_next_attr) {
        if (SDP_DISC_ATTR_TYPE(p_sattr->attr_len_type) == UUID_DESC_TYPE) {
          if (SDP_DISC_ATTR_LEN(p_sattr->attr_len_type) == Uuid::kNumBytes16) {
            *p_uuid = Uuid::From16Bit(p_sattr->attr_value.v.u16);
          } else if (SDP_DISC_ATTR_LEN(p_sattr->attr_len_type) == Uuid::kNumBytes128) {
            *p_uuid = Uuid::From128BitBE(p_sattr->attr_value.v.array);
          } else if (SDP_DISC_ATTR_LEN(p_sattr->attr_len_type) == Uuid::kNumBytes32) {
            *p_uuid = Uuid::From32Bit(p_sattr->attr_value.v.u32);
          }

          return true;
        } else {
          /* Checking for Toyota G Block Car Kit:
           **  This car kit puts an extra data element sequence
           **  where the UUID is suppose to be!!!
           */
          if (SDP_DISC_ATTR_TYPE(p_sattr->attr_len_type) == DATA_ELE_SEQ_DESC_TYPE) {
            /* Look through data element sequence until no more UUIDs */
            for (p_extra_sattr = p_sattr->attr_value.v.p_sub_attr; p_extra_sattr;
                 p_extra_sattr = p_extra_sattr->p_next_attr) {
              /* Increment past this to see if the next attribut is UUID */
              if ((SDP_DISC_ATTR_TYPE(p_extra_sattr->attr_len_type) == UUID_DESC_TYPE)
                  /* only support 16 bits UUID for now */
                  && (SDP_DISC_ATTR_LEN(p_extra_sattr->attr_len_type) == 2)) {
                *p_uuid = Uuid::From16Bit(p_extra_sattr->attr_value.v.u16);
                return true;
              }
            }
          }
        }
      }
      break;
    } else if (p_attr->attr_id == ATTR_ID_SERVICE_ID) {
      if ((SDP_DISC_ATTR_TYPE(p_attr->attr_len_type) == UUID_DESC_TYPE)
          /* only support 16 bits UUID for now */
          && (SDP_DISC_ATTR_LEN(p_attr->attr_len_type) == 2)) {
        *p_uuid = Uuid::From16Bit(p_attr->attr_value.v.u16);
        return true;
      }
    }
    p_attr = p_attr->p_next_attr;
  }
  return false;
}

/*******************************************************************************
 *
 * Function         SDP_FindServiceUUIDInRec_128bit
 *
 * Description      This function is called to read the 128-bit service UUID
 *                  within a record if there is any.
 *
 * Parameters:      p_rec      - pointer to a SDP record.
 *                  p_uuid     - output parameter to save the UUID found.
 *
 * Returns          true if found, otherwise false.
 *
 ******************************************************************************/
bool SDP_FindServiceUUIDInRec_128bit(const tSDP_DISC_REC* p_rec, Uuid* p_uuid) {
  tSDP_DISC_ATTR* p_attr = p_rec->p_first_attr;
  while (p_attr) {
    if ((p_attr->attr_id == ATTR_ID_SERVICE_CLASS_ID_LIST) &&
        (SDP_DISC_ATTR_TYPE(p_attr->attr_len_type) == DATA_ELE_SEQ_DESC_TYPE)) {
      tSDP_DISC_ATTR* p_sattr = p_attr->attr_value.v.p_sub_attr;
      while (p_sattr) {
        if (SDP_DISC_ATTR_TYPE(p_sattr->attr_len_type) == UUID_DESC_TYPE) {
          /* only support 128 bits UUID for now */
          if (SDP_DISC_ATTR_LEN(p_sattr->attr_len_type) == 16) {
            *p_uuid = Uuid::From128BitBE(p_sattr->attr_value.v.array);
          }
          return true;
        }

        p_sattr = p_sattr->p_next_attr;
      }
      break;
    } else if (p_attr->attr_id == ATTR_ID_SERVICE_ID) {
      if ((SDP_DISC_ATTR_TYPE(p_attr->attr_len_type) == UUID_DESC_TYPE)
          /* only support 128 bits UUID for now */
          && (SDP_DISC_ATTR_LEN(p_attr->attr_len_type) == 16)) {
        *p_uuid = Uuid::From128BitBE(p_attr->attr_value.v.array);
        return true;
      }
    }
    p_attr = p_attr->p_next_attr;
  }
  return false;
}

/*******************************************************************************
 *
 * Function         SDP_FindServiceInDb
 *
 * Description      This function queries an SDP database for a specific
 *                  service. If the p_start_rec pointer is NULL, it looks from
 *                  the beginning of the database, else it continues from the
 *                  next record after p_start_rec.
 *
 * Returns          Pointer to record containing service class, or NULL
 *
 ******************************************************************************/
tSDP_DISC_REC* SDP_FindServiceInDb(const tSDP_DISCOVERY_DB* p_db, uint16_t service_uuid,
                                   tSDP_DISC_REC* p_start_rec) {
  tSDP_DISC_REC* p_rec;
  tSDP_DISC_ATTR *p_attr, *p_sattr, *p_extra_sattr;

  /* Must have a valid database */
  if (p_db == NULL) {
    return NULL;
  }

  if (!p_start_rec) {
    p_rec = p_db->p_first_rec;
  } else {
    p_rec = p_start_rec->p_next_rec;
  }

  while (p_rec) {
    p_attr = p_rec->p_first_attr;
    while (p_attr) {
      if ((p_attr->attr_id == ATTR_ID_SERVICE_CLASS_ID_LIST) &&
          (SDP_DISC_ATTR_TYPE(p_attr->attr_len_type) == DATA_ELE_SEQ_DESC_TYPE)) {
        for (p_sattr = p_attr->attr_value.v.p_sub_attr; p_sattr; p_sattr = p_sattr->p_next_attr) {
          if ((SDP_DISC_ATTR_TYPE(p_sattr->attr_len_type) == UUID_DESC_TYPE) &&
              (SDP_DISC_ATTR_LEN(p_sattr->attr_len_type) == 2)) {
            log::verbose("SDP_FindServiceInDb - p_sattr value = 0x{:x} serviceuuid = 0x{:x}",
                         p_sattr->attr_value.v.u16, service_uuid);
            if (service_uuid == UUID_SERVCLASS_HDP_PROFILE) {
              if ((p_sattr->attr_value.v.u16 == UUID_SERVCLASS_HDP_SOURCE) ||
                  (p_sattr->attr_value.v.u16 == UUID_SERVCLASS_HDP_SINK)) {
                log::verbose("SDP_FindServiceInDb found HDP source or sink\n");
                return p_rec;
              }
            }
          }

          if (SDP_DISC_ATTR_TYPE(p_sattr->attr_len_type) == UUID_DESC_TYPE &&
              (service_uuid == 0 || (SDP_DISC_ATTR_LEN(p_sattr->attr_len_type) == 2 &&
                                     p_sattr->attr_value.v.u16 == service_uuid))) {
            /* for a specific uuid, or any one */
            return p_rec;
          } else {
            /* Checking for Toyota G Block Car Kit:
             **  This car kit puts an extra data element sequence
             **  where the UUID is suppose to be!!!
             */
            if (SDP_DISC_ATTR_TYPE(p_sattr->attr_len_type) == DATA_ELE_SEQ_DESC_TYPE) {
              /* Look through data element sequence until no more UUIDs */
              for (p_extra_sattr = p_sattr->attr_value.v.p_sub_attr; p_extra_sattr;
                   p_extra_sattr = p_extra_sattr->p_next_attr) {
                /* Increment past this to see if the next attribut is UUID */
                if ((SDP_DISC_ATTR_TYPE(p_extra_sattr->attr_len_type) == UUID_DESC_TYPE) &&
                    (SDP_DISC_ATTR_LEN(p_extra_sattr->attr_len_type) == 2)
                    /* for a specific uuid, or any one */
                    && ((p_extra_sattr->attr_value.v.u16 == service_uuid) || (service_uuid == 0))) {
                  return p_rec;
                }
              }
            }
          }
        }
        break;
      } else if (p_attr->attr_id == ATTR_ID_SERVICE_ID) {
        if ((SDP_DISC_ATTR_TYPE(p_attr->attr_len_type) == UUID_DESC_TYPE) &&
            (SDP_DISC_ATTR_LEN(p_attr->attr_len_type) == 2)
            /* find a specific UUID or anyone */
            && ((p_attr->attr_value.v.u16 == service_uuid) || service_uuid == 0)) {
          return p_rec;
        }
      }

      p_attr = p_attr->p_next_attr;
    }

    p_rec = p_rec->p_next_rec;
  }
  /* If here, no matching UUID found */
  return NULL;
}

/*******************************************************************************
 *
 * Function         SDP_FindServiceInDb_128bit
 *
 * Description      Query an SDP database for a specific service. If the
 *                  p_start_rec pointer is NULL, it looks from the beginning of
 *                  the database, else it continues from the next record after
 *                  p_start_rec.
 *
 *                  This function is kept separate from SDP_FindServiceInDb
 *                  since that API is expected to return only 16-bit UUIDs
 *
 * Returns          Pointer to record containing service class, or NULL
 *
 ******************************************************************************/
tSDP_DISC_REC* SDP_FindServiceInDb_128bit(const tSDP_DISCOVERY_DB* p_db,
                                          tSDP_DISC_REC* p_start_rec) {
  tSDP_DISC_REC* p_rec;
  tSDP_DISC_ATTR *p_attr, *p_sattr;

  /* Must have a valid database */
  if (p_db == NULL) {
    return NULL;
  }

  if (!p_start_rec) {
    p_rec = p_db->p_first_rec;
  } else {
    p_rec = p_start_rec->p_next_rec;
  }

  while (p_rec) {
    p_attr = p_rec->p_first_attr;
    while (p_attr) {
      if ((p_attr->attr_id == ATTR_ID_SERVICE_CLASS_ID_LIST) &&
          (SDP_DISC_ATTR_TYPE(p_attr->attr_len_type) == DATA_ELE_SEQ_DESC_TYPE)) {
        for (p_sattr = p_attr->attr_value.v.p_sub_attr; p_sattr; p_sattr = p_sattr->p_next_attr) {
          if ((SDP_DISC_ATTR_TYPE(p_sattr->attr_len_type) == UUID_DESC_TYPE) &&
              (SDP_DISC_ATTR_LEN(p_sattr->attr_len_type) == 16)) {
            return p_rec;
          }
        }
        break;
      } else if (p_attr->attr_id == ATTR_ID_SERVICE_ID) {
        if ((SDP_DISC_ATTR_TYPE(p_attr->attr_len_type) == UUID_DESC_TYPE) &&
            (SDP_DISC_ATTR_LEN(p_attr->attr_len_type) == 16)) {
          return p_rec;
        }
      }

      p_attr = p_attr->p_next_attr;
    }

    p_rec = p_rec->p_next_rec;
  }
  /* If here, no matching UUID found */
  return NULL;
}

/*******************************************************************************
 *
 * Function         SDP_FindServiceUUIDInDb
 *
 * Description      Query an SDP database for a specific service. If the
 *                  p_start_rec pointer is NULL, it looks from the beginning of
 *                  the database, else it continues from the next record after
 *                  p_start_rec.
 *
 * NOTE             the only difference between this function and the previous
 *                  function "SDP_FindServiceInDb()" is that this function takes
 *                  a Uuid input
 *
 * Returns          Pointer to record containing service class, or NULL
 *
 ******************************************************************************/
tSDP_DISC_REC* SDP_FindServiceUUIDInDb(const tSDP_DISCOVERY_DB* p_db, const Uuid& uuid,
                                       tSDP_DISC_REC* p_start_rec) {
  tSDP_DISC_REC* p_rec;
  tSDP_DISC_ATTR *p_attr, *p_sattr;

  /* Must have a valid database */
  if (p_db == NULL) {
    return NULL;
  }

  if (!p_start_rec) {
    p_rec = p_db->p_first_rec;
  } else {
    p_rec = p_start_rec->p_next_rec;
  }

  while (p_rec) {
    p_attr = p_rec->p_first_attr;
    while (p_attr) {
      if ((p_attr->attr_id == ATTR_ID_SERVICE_CLASS_ID_LIST) &&
          (SDP_DISC_ATTR_TYPE(p_attr->attr_len_type) == DATA_ELE_SEQ_DESC_TYPE)) {
        for (p_sattr = p_attr->attr_value.v.p_sub_attr; p_sattr; p_sattr = p_sattr->p_next_attr) {
          if (SDP_DISC_ATTR_TYPE(p_sattr->attr_len_type) == UUID_DESC_TYPE) {
            if (sdpu_compare_uuid_with_attr(uuid, p_sattr)) {
              return p_rec;
            }
          }
        }
        break;
      } else if (p_attr->attr_id == ATTR_ID_SERVICE_ID) {
        if (SDP_DISC_ATTR_TYPE(p_attr->attr_len_type) == UUID_DESC_TYPE) {
          if (sdpu_compare_uuid_with_attr(uuid, p_attr)) {
            return p_rec;
          }
        }
      }

      p_attr = p_attr->p_next_attr;
    }

    p_rec = p_rec->p_next_rec;
  }
  /* If here, no matching UUID found */
  return NULL;
}

/*******************************************************************************
 *
 * Function         sdp_fill_proto_elem
 *
 * Description      This function retrieves the protocol element.
 *
 * Returns          true if found, false if not
 *                  If found, the passed protocol list element is filled in.
 *
 ******************************************************************************/
static bool sdp_fill_proto_elem(const tSDP_DISC_ATTR* p_attr, uint16_t layer_uuid,
                                tSDP_PROTOCOL_ELEM* p_elem) {
  tSDP_DISC_ATTR* p_sattr;

  /* Walk through the protocol descriptor list */
  for (p_attr = p_attr->attr_value.v.p_sub_attr; p_attr; p_attr = p_attr->p_next_attr) {
    /* Safety check - each entry should itself be a sequence */
    if (SDP_DISC_ATTR_TYPE(p_attr->attr_len_type) != DATA_ELE_SEQ_DESC_TYPE) {
      return false;
    }

    /* Now, see if the entry contains the layer we are interested in */
    for (p_sattr = p_attr->attr_value.v.p_sub_attr; p_sattr; p_sattr = p_sattr->p_next_attr) {
      /* LOG_VERBOSE ("SDP - p_sattr 0x%x, layer_uuid:0x%x, u16:0x%x####",
          p_sattr, layer_uuid, p_sattr->attr_value.v.u16); */

      if ((SDP_DISC_ATTR_TYPE(p_sattr->attr_len_type) == UUID_DESC_TYPE) &&
          (SDP_DISC_ATTR_LEN(p_sattr->attr_len_type) == 2) &&
          (p_sattr->attr_value.v.u16 == layer_uuid)) {
        /* Bingo. Now fill in the passed element */
        p_elem->protocol_uuid = layer_uuid;
        p_elem->num_params = 0;

        /* Store the parameters, if any */
        for (p_sattr = p_sattr->p_next_attr; p_sattr; p_sattr = p_sattr->p_next_attr) {
          if (SDP_DISC_ATTR_TYPE(p_sattr->attr_len_type) != UINT_DESC_TYPE) {
            break;
          }

          if (SDP_DISC_ATTR_LEN(p_sattr->attr_len_type) == 2) {
            p_elem->params[p_elem->num_params++] = p_sattr->attr_value.v.u16;
          } else {
            p_elem->params[p_elem->num_params++] = p_sattr->attr_value.v.u8;
          }

          if (p_elem->num_params >= SDP_MAX_PROTOCOL_PARAMS) {
            break;
          }
        }
        return true;
      }
    }
  }

  return false;
}

/*******************************************************************************
 *
 * Function         SDP_FindProtocolListElemInRec
 *
 * Description      This function looks at a specific discovery record for a
 *                  protocol list element.
 *
 * Returns          true if found, false if not
 *                  If found, the passed protocol list element is filled in.
 *
 ******************************************************************************/
bool SDP_FindProtocolListElemInRec(const tSDP_DISC_REC* p_rec, uint16_t layer_uuid,
                                   tSDP_PROTOCOL_ELEM* p_elem) {
  tSDP_DISC_ATTR* p_attr;

  p_attr = p_rec->p_first_attr;
  while (p_attr) {
    /* Find the protocol descriptor list */
    if ((p_attr->attr_id == ATTR_ID_PROTOCOL_DESC_LIST) &&
        (SDP_DISC_ATTR_TYPE(p_attr->attr_len_type) == DATA_ELE_SEQ_DESC_TYPE)) {
      return sdp_fill_proto_elem(p_attr, layer_uuid, p_elem);
    }
    p_attr = p_attr->p_next_attr;
  }
  /* If here, no match found */
  return false;
}

/*******************************************************************************
 *
 * Function         SDP_FindProfileVersionInRec
 *
 * Description      This function looks at a specific discovery record for the
 *                  Profile list descriptor, and pulls out the version number.
 *                  The version number consists of an 8-bit major version and
 *                  an 8-bit minor version.
 *
 * Returns          true if found, false if not
 *                  If found, the major and minor version numbers that were
 *                  passed in are filled in.
 *
 ******************************************************************************/
bool SDP_FindProfileVersionInRec(const tSDP_DISC_REC* p_rec, uint16_t profile_uuid,
                                 uint16_t* p_version) {
  tSDP_DISC_ATTR *p_attr, *p_sattr;

  p_attr = p_rec->p_first_attr;
  while (p_attr) {
    /* Find the profile descriptor list */
    if ((p_attr->attr_id == ATTR_ID_BT_PROFILE_DESC_LIST) &&
        (SDP_DISC_ATTR_TYPE(p_attr->attr_len_type) == DATA_ELE_SEQ_DESC_TYPE)) {
      /* Walk through the protocol descriptor list */
      for (p_attr = p_attr->attr_value.v.p_sub_attr; p_attr; p_attr = p_attr->p_next_attr) {
        /* Safety check - each entry should itself be a sequence */
        if (SDP_DISC_ATTR_TYPE(p_attr->attr_len_type) != DATA_ELE_SEQ_DESC_TYPE) {
          return false;
        }

        /* Now, see if the entry contains the profile UUID we are interested in
         */
        for (p_sattr = p_attr->attr_value.v.p_sub_attr; p_sattr; p_sattr = p_sattr->p_next_attr) {
          if ((SDP_DISC_ATTR_TYPE(p_sattr->attr_len_type) == UUID_DESC_TYPE) &&
              (SDP_DISC_ATTR_LEN(p_sattr->attr_len_type) ==
               2) /* <- This is bytes, not size code! */
              && (p_sattr->attr_value.v.u16 == profile_uuid)) {
            /* Now fill in the major and minor numbers */
            /* if the attribute matches the description for version (type UINT,
             * size 2 bytes) */
            p_sattr = p_sattr->p_next_attr;

            if ((SDP_DISC_ATTR_TYPE(p_sattr->attr_len_type) == UINT_DESC_TYPE) &&
                (SDP_DISC_ATTR_LEN(p_sattr->attr_len_type) == 2)) {
              /* The high order 8 bits is the major number, low order is the
               * minor number (big endian) */
              *p_version = p_sattr->attr_value.v.u16;

              return true;
            } else {
              return false;  // The type and/or size was not valid for the
                             //   profile list version
            }
          }
        }
      }

      return false;
    }
    p_attr = p_attr->p_next_attr;
  }

  /* If here, no match found */
  return false;
}

/*******************************************************************************
 *                   Device Identification (DI) Client Functions
 ******************************************************************************/

/*******************************************************************************
 *
 * Function         SDP_DiDiscover
 *
 * Description      This function queries a remote device for DI information.
 *
 * Returns          tSDP_STATUS::SDP_SUCCESS if query started successfully, else error
 *
 ******************************************************************************/
tSDP_STATUS SDP_DiDiscover(const RawAddress& remote_device, tSDP_DISCOVERY_DB* p_db, uint32_t len,
                           tSDP_DISC_CMPL_CB* p_cb) {
  tSDP_STATUS result = tSDP_STATUS::SDP_DI_DISC_FAILED;
  uint16_t num_uuids = 1;
  uint16_t di_uuid = UUID_SERVCLASS_PNP_INFORMATION;

  /* build uuid for db init */
  Uuid init_uuid = Uuid::From16Bit(di_uuid);

  if (SDP_InitDiscoveryDb(p_db, len, num_uuids, &init_uuid, 0, NULL)) {
    if (SDP_ServiceSearchRequest(remote_device, p_db, p_cb)) {
      result = tSDP_STATUS::SDP_SUCCESS;
    }
  }

  return result;
}

/*******************************************************************************
 *
 * Function         SDP_GetNumDiRecords
 *
 * Description      Searches specified database for DI records
 *
 * Returns          number of DI records found
 *
 ******************************************************************************/
uint8_t SDP_GetNumDiRecords(const tSDP_DISCOVERY_DB* p_db) {
  uint8_t num_records = 0;
  tSDP_DISC_REC* p_curr_record = NULL;

  do {
    p_curr_record = SDP_FindServiceInDb(p_db, UUID_SERVCLASS_PNP_INFORMATION, p_curr_record);
    if (p_curr_record) {
      num_records++;
    }
  } while (p_curr_record);

  return num_records;
}

/*******************************************************************************
 *
 * Function         SDP_AttrStringCopy
 *
 * Description      This function copy given attribute to specified buffer as a
 *                  string
 *
 * Returns          none
 *
 ******************************************************************************/
static void SDP_AttrStringCopy(char* dst, const tSDP_DISC_ATTR* p_attr, uint16_t dst_size,
                               uint8_t expected_type) {
  if (dst == NULL) {
    return;
  }

  dst[0] = '\0';

  if (p_attr) {
    uint8_t type = SDP_DISC_ATTR_TYPE(p_attr->attr_len_type);

    if (type == expected_type) {
      uint16_t len = SDP_DISC_ATTR_LEN(p_attr->attr_len_type);
      if (len > dst_size - 1) {
        len = dst_size - 1;
      }
      memcpy(dst, (const void*)p_attr->attr_value.v.array, len);
      dst[len] = '\0';
    } else {
      log::error("unexpected attr type={}, expected={}", type, expected_type);
    }
  } else {
    log::error("p_attr is NULL");
  }
}

/*******************************************************************************
 *
 * Function         SDP_GetDiRecord
 *
 * Description      This function retrieves a remote device's DI record from
 *                  the specified database.
 *
 * Returns          tSDP_STATUS::SDP_SUCCESS if record retrieved, else error
 *
 ******************************************************************************/
tSDP_STATUS SDP_GetDiRecord(uint8_t get_record_index, tSDP_DI_GET_RECORD* p_device_info,
                            const tSDP_DISCOVERY_DB* p_db) {
  tSDP_STATUS result = tSDP_STATUS::SDP_NO_DI_RECORD_FOUND;
  uint8_t curr_record_index = 1;

  tSDP_DISC_REC* p_curr_record = NULL;

  /* find the requested SDP record in the discovery database */
  do {
    p_curr_record = SDP_FindServiceInDb(p_db, UUID_SERVCLASS_PNP_INFORMATION, p_curr_record);
    if (p_curr_record) {
      if (curr_record_index++ == get_record_index) {
        result = tSDP_STATUS::SDP_SUCCESS;
        break;
      }
    }
  } while (p_curr_record);

  if (result == tSDP_STATUS::SDP_SUCCESS) {
    /* copy the information from the SDP record to the DI record */
    tSDP_DISC_ATTR* p_curr_attr = NULL;

    /* ClientExecutableURL is optional */
    p_curr_attr = SDP_FindAttributeInRec(p_curr_record, ATTR_ID_CLIENT_EXE_URL);
    SDP_AttrStringCopy(p_device_info->rec.client_executable_url, p_curr_attr, SDP_MAX_ATTR_LEN,
                       URL_DESC_TYPE);

    /* Service Description is optional */
    /* 5.1.16 ServiceDescription attribute */
    p_curr_attr = SDP_FindAttributeInRec(p_curr_record, ATTR_ID_SERVICE_DESCRIPTION);
    SDP_AttrStringCopy(p_device_info->rec.service_description, p_curr_attr, SDP_MAX_ATTR_LEN,
                       TEXT_STR_DESC_TYPE);

    /* DocumentationURL is optional */
    p_curr_attr = SDP_FindAttributeInRec(p_curr_record, ATTR_ID_DOCUMENTATION_URL);
    SDP_AttrStringCopy(p_device_info->rec.documentation_url, p_curr_attr, SDP_MAX_ATTR_LEN,
                       URL_DESC_TYPE);

    p_curr_attr = SDP_FindAttributeInRec(p_curr_record, ATTR_ID_SPECIFICATION_ID);
    if (p_curr_attr && SDP_DISC_ATTR_TYPE(p_curr_attr->attr_len_type) == UINT_DESC_TYPE &&
        SDP_DISC_ATTR_LEN(p_curr_attr->attr_len_type) >= 2) {
      p_device_info->spec_id = p_curr_attr->attr_value.v.u16;
    } else {
      result = tSDP_STATUS::SDP_ERR_ATTR_NOT_PRESENT;
    }

    p_curr_attr = SDP_FindAttributeInRec(p_curr_record, ATTR_ID_VENDOR_ID);
    if (p_curr_attr && SDP_DISC_ATTR_TYPE(p_curr_attr->attr_len_type) == UINT_DESC_TYPE &&
        SDP_DISC_ATTR_LEN(p_curr_attr->attr_len_type) >= 2) {
      p_device_info->rec.vendor = p_curr_attr->attr_value.v.u16;
    } else {
      result = tSDP_STATUS::SDP_ERR_ATTR_NOT_PRESENT;
    }

    p_curr_attr = SDP_FindAttributeInRec(p_curr_record, ATTR_ID_VENDOR_ID_SOURCE);
    if (p_curr_attr && SDP_DISC_ATTR_TYPE(p_curr_attr->attr_len_type) == UINT_DESC_TYPE &&
        SDP_DISC_ATTR_LEN(p_curr_attr->attr_len_type) >= 2) {
      p_device_info->rec.vendor_id_source = p_curr_attr->attr_value.v.u16;
    } else {
      result = tSDP_STATUS::SDP_ERR_ATTR_NOT_PRESENT;
    }

    p_curr_attr = SDP_FindAttributeInRec(p_curr_record, ATTR_ID_PRODUCT_ID);
    if (p_curr_attr && SDP_DISC_ATTR_TYPE(p_curr_attr->attr_len_type) == UINT_DESC_TYPE &&
        SDP_DISC_ATTR_LEN(p_curr_attr->attr_len_type) >= 2) {
      p_device_info->rec.product = p_curr_attr->attr_value.v.u16;
    } else {
      result = tSDP_STATUS::SDP_ERR_ATTR_NOT_PRESENT;
    }

    p_curr_attr = SDP_FindAttributeInRec(p_curr_record, ATTR_ID_PRODUCT_VERSION);
    if (p_curr_attr && SDP_DISC_ATTR_TYPE(p_curr_attr->attr_len_type) == UINT_DESC_TYPE &&
        SDP_DISC_ATTR_LEN(p_curr_attr->attr_len_type) >= 2) {
      p_device_info->rec.version = p_curr_attr->attr_value.v.u16;
    } else {
      result = tSDP_STATUS::SDP_ERR_ATTR_NOT_PRESENT;
    }

    p_curr_attr = SDP_FindAttributeInRec(p_curr_record, ATTR_ID_PRIMARY_RECORD);
    if (p_curr_attr && SDP_DISC_ATTR_TYPE(p_curr_attr->attr_len_type) == BOOLEAN_DESC_TYPE &&
        SDP_DISC_ATTR_LEN(p_curr_attr->attr_len_type) >= 1) {
      p_device_info->rec.primary_record = (bool)p_curr_attr->attr_value.v.u8;
    } else {
      result = tSDP_STATUS::SDP_ERR_ATTR_NOT_PRESENT;
    }
  }

  return result;
}

/*******************************************************************************
 *                   Device Identification (DI) Server Functions
 ******************************************************************************/

/*******************************************************************************
 *
 * Function         SDP_SetLocalDiRecord
 *
 * Description      This function adds a DI record to the local SDP database.
 *
 *
 *
 * Returns          Returns tSDP_STATUS::SDP_SUCCESS if record added successfully, else error
 *
 ******************************************************************************/
tSDP_STATUS SDP_SetLocalDiRecord(const tSDP_DI_RECORD* p_device_info, uint32_t* p_handle) {
  tSDP_STATUS result = tSDP_STATUS::SDP_SUCCESS;
  uint32_t handle;
  uint16_t di_uuid = UUID_SERVCLASS_PNP_INFORMATION;
  uint16_t di_specid = BLUETOOTH_DI_SPECIFICATION;
  uint8_t temp_u16[2];
  uint8_t* p_temp;
  uint8_t u8;

  *p_handle = 0;
  if (p_device_info == NULL) {
    return tSDP_STATUS::SDP_ILLEGAL_PARAMETER;
  }

  /* if record is to be primary record, get handle to replace old primary */
  if (p_device_info->primary_record && sdp_cb.server_db.di_primary_handle) {
    handle = sdp_cb.server_db.di_primary_handle;
  } else {
    handle = SDP_CreateRecord();
    if (handle == 0) {
      return tSDP_STATUS::SDP_NO_RESOURCES;
    }
  }

  *p_handle = handle;

  /* build the SDP entry */
  /* Add the UUID to the Service Class ID List */
  if (!(SDP_AddServiceClassIdList(handle, 1, &di_uuid))) {
    result = tSDP_STATUS::SDP_DI_REG_FAILED;
  }

  /* mandatory */
  if (result == tSDP_STATUS::SDP_SUCCESS) {
    p_temp = temp_u16;
    UINT16_TO_BE_STREAM(p_temp, di_specid);
    if (!(SDP_AddAttribute(handle, ATTR_ID_SPECIFICATION_ID, UINT_DESC_TYPE, sizeof(di_specid),
                           temp_u16))) {
      result = tSDP_STATUS::SDP_DI_REG_FAILED;
    }
  }

  /* optional - if string is null, do not add attribute */
  if (result == tSDP_STATUS::SDP_SUCCESS) {
    if (p_device_info->client_executable_url[0] != '\0') {
      if (!((strlen(p_device_info->client_executable_url) + 1 <= SDP_MAX_ATTR_LEN) &&
            SDP_AddAttribute(handle, ATTR_ID_CLIENT_EXE_URL, URL_DESC_TYPE,
                             (uint32_t)(strlen(p_device_info->client_executable_url) + 1),
                             (uint8_t*)p_device_info->client_executable_url))) {
        result = tSDP_STATUS::SDP_DI_REG_FAILED;
      }
    }
  }

  /* optional - if string is null, do not add attribute */
  if (result == tSDP_STATUS::SDP_SUCCESS) {
    if (p_device_info->service_description[0] != '\0') {
      if (!((strlen(p_device_info->service_description) + 1 <= SDP_MAX_ATTR_LEN) &&
            SDP_AddAttribute(handle, ATTR_ID_SERVICE_DESCRIPTION, TEXT_STR_DESC_TYPE,
                             (uint32_t)(strlen(p_device_info->service_description) + 1),
                             (uint8_t*)p_device_info->service_description))) {
        result = tSDP_STATUS::SDP_DI_REG_FAILED;
      }
    }
  }

  /* optional - if string is null, do not add attribute */
  if (result == tSDP_STATUS::SDP_SUCCESS) {
    if (p_device_info->documentation_url[0] != '\0') {
      if (!((strlen(p_device_info->documentation_url) + 1 <= SDP_MAX_ATTR_LEN) &&
            SDP_AddAttribute(handle, ATTR_ID_DOCUMENTATION_URL, URL_DESC_TYPE,
                             (uint32_t)(strlen(p_device_info->documentation_url) + 1),
                             (uint8_t*)p_device_info->documentation_url))) {
        result = tSDP_STATUS::SDP_DI_REG_FAILED;
      }
    }
  }

  /* mandatory */
  if (result == tSDP_STATUS::SDP_SUCCESS) {
    p_temp = temp_u16;
    UINT16_TO_BE_STREAM(p_temp, p_device_info->vendor);
    if (!(SDP_AddAttribute(handle, ATTR_ID_VENDOR_ID, UINT_DESC_TYPE, sizeof(p_device_info->vendor),
                           temp_u16))) {
      result = tSDP_STATUS::SDP_DI_REG_FAILED;
    }
  }

  /* mandatory */
  if (result == tSDP_STATUS::SDP_SUCCESS) {
    p_temp = temp_u16;
    UINT16_TO_BE_STREAM(p_temp, p_device_info->product);
    if (!(SDP_AddAttribute(handle, ATTR_ID_PRODUCT_ID, UINT_DESC_TYPE,
                           sizeof(p_device_info->product), temp_u16))) {
      result = tSDP_STATUS::SDP_DI_REG_FAILED;
    }
  }

  /* mandatory */
  if (result == tSDP_STATUS::SDP_SUCCESS) {
    p_temp = temp_u16;
    UINT16_TO_BE_STREAM(p_temp, p_device_info->version);
    if (!(SDP_AddAttribute(handle, ATTR_ID_PRODUCT_VERSION, UINT_DESC_TYPE,
                           sizeof(p_device_info->version), temp_u16))) {
      result = tSDP_STATUS::SDP_DI_REG_FAILED;
    }
  }

  /* mandatory */
  if (result == tSDP_STATUS::SDP_SUCCESS) {
    u8 = (uint8_t)p_device_info->primary_record;
    if (!(SDP_AddAttribute(handle, ATTR_ID_PRIMARY_RECORD, BOOLEAN_DESC_TYPE, 1, &u8))) {
      result = tSDP_STATUS::SDP_DI_REG_FAILED;
    }
  }

  /* mandatory */
  if (result == tSDP_STATUS::SDP_SUCCESS) {
    p_temp = temp_u16;
    UINT16_TO_BE_STREAM(p_temp, p_device_info->vendor_id_source);
    if (!(SDP_AddAttribute(handle, ATTR_ID_VENDOR_ID_SOURCE, UINT_DESC_TYPE,
                           sizeof(p_device_info->vendor_id_source), temp_u16))) {
      result = tSDP_STATUS::SDP_DI_REG_FAILED;
    }
  }

  if (result != tSDP_STATUS::SDP_SUCCESS) {
    SDP_DeleteRecord(handle);
  } else if (p_device_info->primary_record) {
    sdp_cb.server_db.di_primary_handle = handle;
  }

  return result;
}

namespace {
bluetooth::legacy::stack::sdp::tSdpApi api_ = {
        .service =
                {
                        .SDP_InitDiscoveryDb = ::SDP_InitDiscoveryDb,
                        .SDP_CancelServiceSearch = ::SDP_CancelServiceSearch,
                        .SDP_ServiceSearchRequest = ::SDP_ServiceSearchRequest,
                        .SDP_ServiceSearchAttributeRequest = ::SDP_ServiceSearchAttributeRequest,
                        .SDP_ServiceSearchAttributeRequest2 = ::SDP_ServiceSearchAttributeRequest2,
                },
        .db =
                {
                        .SDP_FindServiceInDb = ::SDP_FindServiceInDb,
                        .SDP_FindServiceUUIDInDb = ::SDP_FindServiceUUIDInDb,
                        .SDP_FindServiceInDb_128bit = ::SDP_FindServiceInDb_128bit,
                },
        .record =
                {
                        .SDP_FindAttributeInRec = ::SDP_FindAttributeInRec,
                        .SDP_FindServiceUUIDInRec_128bit = ::SDP_FindServiceUUIDInRec_128bit,
                        .SDP_FindProtocolListElemInRec = ::SDP_FindProtocolListElemInRec,
                        .SDP_FindProfileVersionInRec = ::SDP_FindProfileVersionInRec,
                        .SDP_FindServiceUUIDInRec = ::SDP_FindServiceUUIDInRec,
                },
        .handle =
                {
                        .SDP_CreateRecord = ::SDP_CreateRecord,
                        .SDP_DeleteRecord = ::SDP_DeleteRecord,
                        .SDP_AddAttribute = ::SDP_AddAttribute,
                        .SDP_AddSequence = ::SDP_AddSequence,
                        .SDP_AddUuidSequence = ::SDP_AddUuidSequence,
                        .SDP_AddProtocolList = ::SDP_AddProtocolList,
                        .SDP_AddAdditionProtoLists = ::SDP_AddAdditionProtoLists,
                        .SDP_AddProfileDescriptorList = ::SDP_AddProfileDescriptorList,
                        .SDP_AddLanguageBaseAttrIDList = ::SDP_AddLanguageBaseAttrIDList,
                        .SDP_AddServiceClassIdList = ::SDP_AddServiceClassIdList,
                },
        .device_id =
                {
                        .SDP_SetLocalDiRecord = ::SDP_SetLocalDiRecord,
                        .SDP_DiDiscover = ::SDP_DiDiscover,
                        .SDP_GetNumDiRecords = ::SDP_GetNumDiRecords,
                        .SDP_GetDiRecord = ::SDP_GetDiRecord,
                },
};
}  // namespace

const bluetooth::legacy::stack::sdp::tSdpApi*
bluetooth::legacy::stack::sdp::get_legacy_stack_sdp_api() {
  return &api_;
}

extern void BTA_SdpDumpsys(int fd);

#define DUMPSYS_TAG "shim::legacy::sdp"

namespace {

void SDP_DumpConnectionControlBlock(int fd, const tCONN_CB& conn_cb) {
  if (conn_cb.device_address == RawAddress::kEmpty) {
    return;
  }
  LOG_DUMPSYS(fd, "peer:%s discovery_state:%s", std::format("{}", conn_cb.device_address).c_str(),
              sdp_disc_wait_text(conn_cb.disc_state).c_str());
  LOG_DUMPSYS(fd, "  connection_state:%s connection_flags:0x%02x mtu:%hu l2cap_cid:%hu",
              sdp_state_text(conn_cb.con_state).c_str(), conn_cb.con_flags, conn_cb.rem_mtu_size,
              conn_cb.connection_id);

  const uint64_t remaining_ms = alarm_get_remaining_ms(conn_cb.sdp_conn_timer);
  if (remaining_ms) {
    LOG_DUMPSYS(fd, "  timer_set:%Lu ms", static_cast<long long>(remaining_ms));
  }
  if (conn_cb.num_handles >= kMaxSdpRecords) {
    LOG_DUMPSYS(fd, "  WARNING - Number handles:%hu exceeds max handles:%u", conn_cb.num_handles,
                kMaxSdpRecords);
  } else {
    for (int i = 0; i < conn_cb.num_handles; i++) {
      LOG_DUMPSYS(fd, "  handle:%u", conn_cb.handles[i]);
    }
  }
}

}  // namespace

void SDP_Dumpsys(int fd) {
  LOG_DUMPSYS_TITLE(fd, DUMPSYS_TAG);
  LOG_DUMPSYS(fd, "max_attribute_list_size:%hu max_records_per_search:%hu",
              sdp_cb.max_attr_list_size, sdp_cb.max_recs_per_search);
  for (unsigned i = 0; i < kMaxSdpConnections; i++) {
    SDP_DumpConnectionControlBlock(fd, sdp_cb.ccb[i]);
  }
}
#undef DUMPSYS_TAG
