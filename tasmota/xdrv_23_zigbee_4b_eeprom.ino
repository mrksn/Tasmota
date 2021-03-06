/*
  xdrv_23_zigbee_4a_eeprom.ino - zigbee support for Tasmota - saving configuration in I2C Eeprom of ZBBridge

  Copyright (C) 2020  Theo Arends and Stephan Hadinger

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifdef USE_ZIGBEE


// =======================
// ZbData v1
// File structure:
//
// [Array of devices]
// uint8  - length of device record (excluding the length byte)
// uint16 - short address
//
// [Device specific data first]
// uint8 - length of structure (excluding the length byte)
// uint8[] - device wide data
//
// [Array of data structures]
// uint8  - length of structure
// uint8[] - list of data
//

// returns the lenght of consumed buffer, or -1 if error
int32_t hydrateDeviceWideData(class Z_Device & device, const SBuffer & buf, size_t start, size_t len) {
  size_t segment_len = buf.get8(start);
  if ((segment_len < 6) || (segment_len > len)) {
    AddLog_P(LOG_LEVEL_INFO, PSTR(D_LOG_ZIGBEE "invalid device wide data length=%d"), segment_len);
    return -1;
  }
  device.last_seen = buf.get32(start+1);
  device.lqi = buf.get8(start + 5);
  device.batterypercent = buf.get8(start + 6);
  return segment_len + 1;
}

// return true if success
bool hydrateDeviceData(class Z_Device & device, const SBuffer & buf, size_t start, size_t len) {
  // First hydrate device wide data
  int32_t ret = hydrateDeviceWideData(device, buf, start, len);
  if (ret < 0) { return false; }

  size_t offset = ret;
  while (offset + 5 <= len) {    // each entry is at least 5 bytes
    uint8_t data_len = buf.get8(start + offset);
// #ifdef Z_EEPROM_DEBUG
//   {
//     char hex_char[((data_len+1) * 2) + 2];
//     AddLog_P(LOG_LEVEL_INFO, PSTR(D_LOG_ZIGBEE "hydrateDeviceData data_len=%d contains %s"), data_len, ToHex_P(buf.buf(start+offset+1), data_len, hex_char, sizeof(hex_char)));
//   }
// #endif
    Z_Data & data_elt = device.data.createFromBuffer(buf, start + offset + 1, data_len);
    (void)data_elt;   // avoid compiler warning
    offset += data_len + 1;
  }
  return true;
}

// negative means error
// positive is the segment length
int32_t hydrateSingleDevice(const class SBuffer & buf, size_t start, size_t len) {
  uint8_t segment_len = buf.get8(start);
  if ((segment_len < 4) || (start + segment_len > len)) {
    AddLog_P(LOG_LEVEL_INFO, PSTR(D_LOG_ZIGBEE "invalid segment_len=%d"), segment_len);
    return -1;
  }
  // read shortaddr
  uint16_t shortaddr = buf.get16(start + 1);
  if (shortaddr >= 0xFFF0) {
    AddLog_P(LOG_LEVEL_INFO, PSTR(D_LOG_ZIGBEE "invalid shortaddr=0x%04X"), shortaddr);
    return -1;
  }
#ifdef Z_EEPROM_DEBUG
  {
    if (segment_len > 3) {
      char hex_char[((segment_len+1) * 2) + 2];
      AddLog_P(LOG_LEVEL_INFO, PSTR(D_LOG_ZIGBEE "ZbData 0x%04X,%s"), shortaddr, ToHex_P(buf.buf(start+3), segment_len+1-3, hex_char, sizeof(hex_char)));
    }
  }
#endif
  // check if the device exists, if not skip the record
  Z_Device & device = zigbee_devices.findShortAddr(shortaddr);
  if (&device != nullptr) {

    // parse the rest
    bool ret = hydrateDeviceData(device, buf, start + 3, segment_len - 3);

    if (!ret) { return -1; }
  }
  return segment_len + 1;
}

/*********************************************************************************************\
 * 
 * Hydrate data from the EEPROM
 * 
\*********************************************************************************************/
// Parse the entire blob
// return true if ok
bool hydrateDevicesDataFromEEPROM(void) {
#ifdef USE_ZIGBEE_EZSP
  if (!zigbee.eeprom_ready) { return false; }
  int32_t file_length = ZFS::getLength(ZIGB_DATA2);
  if (file_length > 0) {
    AddLog_P(LOG_LEVEL_INFO, PSTR(D_LOG_ZIGBEE "Zigbee device data in EEPROM (%d bytes)"), file_length);
  } else {
    AddLog_P(LOG_LEVEL_INFO, PSTR(D_LOG_ZIGBEE "No Zigbee device data in EEPROM"));
    return false;
  }

  const uint16_t READ_BUFFER = 192;
  uint16_t cursor = 0x0000;         // cursor in the file
  bool read_more = true;

  SBuffer buf(READ_BUFFER);
  while (read_more) {
    buf.setLen(buf.size());         // set to max size and fill with zeros
    int32_t bytes_read = ZFS::readBytes(ZIGB_DATA2, buf.getBuffer(), buf.size(), cursor, READ_BUFFER);
// #ifdef Z_EEPROM_DEBUG
//     AddLog_P(LOG_LEVEL_INFO, PSTR(D_LOG_ZIGBEE "readBytes buffer_len=%d, read_start=%d, read_len=%d, actual_read=%d"), buf.size(), cursor, length, bytes_read);
// #endif
    if (bytes_read > 0) {
      buf.setLen(bytes_read);       // adjust to actual size
      int32_t segment_len = hydrateSingleDevice(buf, 0, buf.len());
// #ifdef Z_EEPROM_DEBUG
//       AddLog_P(LOG_LEVEL_INFO, PSTR(D_LOG_ZIGBEE "hydrateSingleDevice segment_len=%d"), segment_len);
// #endif
      if (segment_len <= 0) { return false; }

      cursor += segment_len;
    } else {
      read_more = false;
    }
  }
  return true;
#else // USE_ZIGBEE_EZSP
  return false;
#endif // USE_ZIGBEE_EZSP
}

class SBuffer hibernateDeviceData(const struct Z_Device & device, bool mqtt = false) {
  SBuffer buf(192);

  // If we have zero information about the device, just skip ir
  if (device.validLqi() ||
      device.validBatteryPercent() ||
      device.validLastSeen() ||
      !device.data.isEmpty()) {

    buf.add8(0x00);     // overall length, will be updated later
    buf.add16(device.shortaddr);

    // device wide data
    buf.add8(6);        // 6 bytes
    buf.add32(device.last_seen);
    buf.add8(device.lqi);
    buf.add8(device.batterypercent);

    for (const auto & data_elt : device.data) {
      size_t item_len = data_elt.DataTypeToLength(data_elt.getType());
      buf.add8(item_len);      // place-holder for length
      buf.addBuffer((uint8_t*) &data_elt, item_len);
    }

    // update overall length
    buf.set8(0, buf.len() - 1);

    {
      size_t buf_len = buf.len() - 3;
      char hex[2*buf_len + 1];
      // skip first 3 bytes
      ToHex_P(buf.buf(3), buf_len, hex, sizeof(hex));

      if (mqtt) {
        Response_P(PSTR("{\"" D_PRFX_ZB D_CMND_ZIGBEE_DATA "\":\"ZbData 0x%04X,%s\"}"), device.shortaddr, hex);
        MqttPublishPrefixTopicRulesProcess_P(RESULT_OR_STAT, PSTR(D_PRFX_ZB D_CMND_ZIGBEE_DATA));
      } else {
        AddLog_P(LOG_LEVEL_INFO, PSTR(D_LOG_ZIGBEE "ZbData 0x%04X,%s"), device.shortaddr, hex);
      }
    }
  }

  return buf;
}

/*********************************************************************************************\
 * 
 * Hibernate data to the EEPROM
 * 
\*********************************************************************************************/
void hibernateAllData(void) {
#ifdef USE_ZIGBEE_EZSP
  if (Rtc.utc_time < START_VALID_TIME) { return; }
  if (!zigbee.eeprom_ready) { return; }

  ZFS_Write_File write_data(ZIGB_DATA2);
  // first prefix is number of devices
  uint8_t device_num = zigbee_devices.devicesSize();

  for (const auto & device : zigbee_devices.getDevices()) {
    // allocte a buffer for a single device
    SBuffer buf = hibernateDeviceData(device, false);    // simple log, no mqtt
    if (buf.len() > 0) {
      write_data.addBytes(buf.getBuffer(), buf.len());
    }
  }
  int32_t ret = write_data.close();
#ifdef Z_EEPROM_DEBUG
  AddLog_P(LOG_LEVEL_INFO, PSTR(D_LOG_ZIGBEE "ZbData - %d bytes written to EEPROM"), ret);
#endif
#endif // USE_ZIGBEE_EZSP
}

/*********************************************************************************************\
 * Timer to save every 60 minutes
\*********************************************************************************************/
const uint32_t Z_SAVE_DATA_TIMER = 60 * 60 * 1000;       // save data every 60 minutes (in ms)

//
// Callback for setting the timer to save Zigbee Data in x seconds
//
int32_t Z_Set_Save_Data_Timer(uint8_t value) {
  zigbee_devices.setTimer(0x0000, 0, Z_SAVE_DATA_TIMER, 0, 0, Z_CAT_ALWAYS, 0 /* value */, &Z_SaveDataTimer);
  return 0;                              // continue
}

void Z_SaveDataTimer(uint16_t shortaddr, uint16_t groupaddr, uint16_t cluster, uint8_t endpoint, uint32_t value) {
  hibernateAllData();
  Z_Set_Save_Data_Timer(0);     // set a new timer
}

#endif // USE_ZIGBEE
