// NovaKeyboard - based on ESP32-BLE-Keyboard 0.3.0 by T-vK
// (github.com/T-vK/ESP32-BLE-Keyboard), renamed and extended for Pocket Nova:
// the reconnect fix in onConnect(), Swift Pair advertising, several
// connections at once for the phone remote, and a mouse for the air mouse. All credit for the original
// keyboard code goes to its author.

#if defined(USE_NIMBLE)
#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <NimBLEUtils.h>
#include <NimBLEHIDDevice.h>
#else
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include "BLE2902.h"
#include "BLEHIDDevice.h"
#endif // USE_NIMBLE
#include "HIDTypes.h"
#include <driver/adc.h>
#include "sdkconfig.h"

#include "NovaKeyboard.h"

#if defined(CONFIG_ARDUHAL_ESP_LOG)
  #include "esp32-hal-log.h"
  #define LOG_TAG ""
#else
  #include "esp_log.h"
  static const char* LOG_TAG = "BLEDevice";
#endif


// Report IDs:
#define KEYBOARD_ID 0x01
#define MEDIA_KEYS_ID 0x02
#define MOUSE_ID 0x03

static const uint8_t _hidReportDescriptor[] = {
  USAGE_PAGE(1),      0x01,          // USAGE_PAGE (Generic Desktop Ctrls)
  USAGE(1),           0x06,          // USAGE (Keyboard)
  COLLECTION(1),      0x01,          // COLLECTION (Application)
  // ------------------------------------------------- Keyboard
  REPORT_ID(1),       KEYBOARD_ID,   //   REPORT_ID (1)
  USAGE_PAGE(1),      0x07,          //   USAGE_PAGE (Kbrd/Keypad)
  USAGE_MINIMUM(1),   0xE0,          //   USAGE_MINIMUM (0xE0)
  USAGE_MAXIMUM(1),   0xE7,          //   USAGE_MAXIMUM (0xE7)
  LOGICAL_MINIMUM(1), 0x00,          //   LOGICAL_MINIMUM (0)
  LOGICAL_MAXIMUM(1), 0x01,          //   Logical Maximum (1)
  REPORT_SIZE(1),     0x01,          //   REPORT_SIZE (1)
  REPORT_COUNT(1),    0x08,          //   REPORT_COUNT (8)
  HIDINPUT(1),        0x02,          //   INPUT (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
  REPORT_COUNT(1),    0x01,          //   REPORT_COUNT (1) ; 1 byte (Reserved)
  REPORT_SIZE(1),     0x08,          //   REPORT_SIZE (8)
  HIDINPUT(1),        0x01,          //   INPUT (Const,Array,Abs,No Wrap,Linear,Preferred State,No Null Position)
  REPORT_COUNT(1),    0x05,          //   REPORT_COUNT (5) ; 5 bits (Num lock, Caps lock, Scroll lock, Compose, Kana)
  REPORT_SIZE(1),     0x01,          //   REPORT_SIZE (1)
  USAGE_PAGE(1),      0x08,          //   USAGE_PAGE (LEDs)
  USAGE_MINIMUM(1),   0x01,          //   USAGE_MINIMUM (0x01) ; Num Lock
  USAGE_MAXIMUM(1),   0x05,          //   USAGE_MAXIMUM (0x05) ; Kana
  HIDOUTPUT(1),       0x02,          //   OUTPUT (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
  REPORT_COUNT(1),    0x01,          //   REPORT_COUNT (1) ; 3 bits (Padding)
  REPORT_SIZE(1),     0x03,          //   REPORT_SIZE (3)
  HIDOUTPUT(1),       0x01,          //   OUTPUT (Const,Array,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
  REPORT_COUNT(1),    0x06,          //   REPORT_COUNT (6) ; 6 bytes (Keys)
  REPORT_SIZE(1),     0x08,          //   REPORT_SIZE(8)
  LOGICAL_MINIMUM(1), 0x00,          //   LOGICAL_MINIMUM(0)
  LOGICAL_MAXIMUM(1), 0x65,          //   LOGICAL_MAXIMUM(0x65) ; 101 keys
  USAGE_PAGE(1),      0x07,          //   USAGE_PAGE (Kbrd/Keypad)
  USAGE_MINIMUM(1),   0x00,          //   USAGE_MINIMUM (0)
  USAGE_MAXIMUM(1),   0x65,          //   USAGE_MAXIMUM (0x65)
  HIDINPUT(1),        0x00,          //   INPUT (Data,Array,Abs,No Wrap,Linear,Preferred State,No Null Position)
  END_COLLECTION(0),                 // END_COLLECTION
  // ------------------------------------------------- Media Keys
  USAGE_PAGE(1),      0x0C,          // USAGE_PAGE (Consumer)
  USAGE(1),           0x01,          // USAGE (Consumer Control)
  COLLECTION(1),      0x01,          // COLLECTION (Application)
  REPORT_ID(1),       MEDIA_KEYS_ID, //   REPORT_ID (3)
  USAGE_PAGE(1),      0x0C,          //   USAGE_PAGE (Consumer)
  LOGICAL_MINIMUM(1), 0x00,          //   LOGICAL_MINIMUM (0)
  LOGICAL_MAXIMUM(1), 0x01,          //   LOGICAL_MAXIMUM (1)
  REPORT_SIZE(1),     0x01,          //   REPORT_SIZE (1)
  REPORT_COUNT(1),    0x10,          //   REPORT_COUNT (16)
  USAGE(1),           0xB5,          //   USAGE (Scan Next Track)     ; bit 0: 1
  USAGE(1),           0xB6,          //   USAGE (Scan Previous Track) ; bit 1: 2
  USAGE(1),           0xB7,          //   USAGE (Stop)                ; bit 2: 4
  USAGE(1),           0xCD,          //   USAGE (Play/Pause)          ; bit 3: 8
  USAGE(1),           0xE2,          //   USAGE (Mute)                ; bit 4: 16
  USAGE(1),           0xE9,          //   USAGE (Volume Increment)    ; bit 5: 32
  USAGE(1),           0xEA,          //   USAGE (Volume Decrement)    ; bit 6: 64
  USAGE(2),           0x23, 0x02,    //   Usage (WWW Home)            ; bit 7: 128
  USAGE(2),           0x94, 0x01,    //   Usage (My Computer) ; bit 0: 1
  USAGE(2),           0x92, 0x01,    //   Usage (Calculator)  ; bit 1: 2
  USAGE(2),           0x2A, 0x02,    //   Usage (WWW fav)     ; bit 2: 4
  USAGE(2),           0x21, 0x02,    //   Usage (WWW search)  ; bit 3: 8
  USAGE(2),           0x26, 0x02,    //   Usage (WWW stop)    ; bit 4: 16
  USAGE(2),           0x24, 0x02,    //   Usage (WWW back)    ; bit 5: 32
  USAGE(2),           0x83, 0x01,    //   Usage (Media sel)   ; bit 6: 64
  USAGE(2),           0x8A, 0x01,    //   Usage (Mail)        ; bit 7: 128
  HIDINPUT(1),        0x02,          //   INPUT (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
  END_COLLECTION(0),                 // END_COLLECTION
  // ------------------------------------------------- Mouse (Pocket Nova addition)
  // One report, 4 bytes: [buttons][x][y][wheel]. x/y/wheel are RELATIVE
  // moves (-127..127 per report), like any USB mouse.
  USAGE_PAGE(1),      0x01,          // USAGE_PAGE (Generic Desktop)
  USAGE(1),           0x02,          // USAGE (Mouse)
  COLLECTION(1),      0x01,          // COLLECTION (Application)
  USAGE(1),           0x01,          //   USAGE (Pointer)
  COLLECTION(1),      0x00,          //   COLLECTION (Physical)
  REPORT_ID(1),       MOUSE_ID,      //     REPORT_ID (3)
  USAGE_PAGE(1),      0x09,          //     USAGE_PAGE (Button)
  USAGE_MINIMUM(1),   0x01,          //     USAGE_MINIMUM (Button 1)
  USAGE_MAXIMUM(1),   0x03,          //     USAGE_MAXIMUM (Button 3)
  LOGICAL_MINIMUM(1), 0x00,          //     LOGICAL_MINIMUM (0)
  LOGICAL_MAXIMUM(1), 0x01,          //     LOGICAL_MAXIMUM (1)
  REPORT_SIZE(1),     0x01,          //     REPORT_SIZE (1)
  REPORT_COUNT(1),    0x03,          //     REPORT_COUNT (3) ; left, right, middle
  HIDINPUT(1),        0x02,          //     INPUT (Data,Var,Abs)
  REPORT_SIZE(1),     0x05,          //     REPORT_SIZE (5) ; padding to a whole byte
  REPORT_COUNT(1),    0x01,          //     REPORT_COUNT (1)
  HIDINPUT(1),        0x03,          //     INPUT (Const,Var,Abs)
  USAGE_PAGE(1),      0x01,          //     USAGE_PAGE (Generic Desktop)
  USAGE(1),           0x30,          //     USAGE (X)
  USAGE(1),           0x31,          //     USAGE (Y)
  USAGE(1),           0x38,          //     USAGE (Wheel)
  LOGICAL_MINIMUM(1), 0x81,          //     LOGICAL_MINIMUM (-127)
  LOGICAL_MAXIMUM(1), 0x7f,          //     LOGICAL_MAXIMUM (127)
  REPORT_SIZE(1),     0x08,          //     REPORT_SIZE (8)
  REPORT_COUNT(1),    0x03,          //     REPORT_COUNT (3)
  HIDINPUT(1),        0x06,          //     INPUT (Data,Var,Rel)
  END_COLLECTION(0),                 //   END_COLLECTION
  END_COLLECTION(0)                  // END_COLLECTION
};

NovaKeyboard::NovaKeyboard(std::string deviceName, std::string deviceManufacturer, uint8_t batteryLevel) 
    : hid(0)
    , deviceName(std::string(deviceName).substr(0, 15))
    , deviceManufacturer(std::string(deviceManufacturer).substr(0,15))
    , batteryLevel(batteryLevel) {}

void NovaKeyboard::begin(void)
{
  BLEDevice::init(deviceName);
  BLEServer* pServer = BLEDevice::createServer();
  pServer->setCallbacks(this);
  server = pServer;

  hid = new BLEHIDDevice(pServer);
  inputKeyboard = hid->inputReport(KEYBOARD_ID);  // <-- input REPORTID from report map
  outputKeyboard = hid->outputReport(KEYBOARD_ID);
  inputMediaKeys = hid->inputReport(MEDIA_KEYS_ID);
  inputMouse = hid->inputReport(MOUSE_ID);

  outputKeyboard->setCallbacks(this);

  hid->manufacturer()->setValue(deviceManufacturer);

  hid->pnp(0x02, 0xe502, 0xa111, 0x0210);
  hid->hidInfo(0x00, 0x01);

  BLESecurity* pSecurity = new BLESecurity();

  pSecurity->setAuthenticationMode(ESP_LE_AUTH_BOND);

  hid->reportMap((uint8_t*)_hidReportDescriptor, sizeof(_hidReportDescriptor));
  hid->startServices();

  onStarted(pServer);

  advertising = pServer->getAdvertising();
#if defined(USE_NIMBLE)
  advertising->setAppearance(HID_KEYBOARD);
  advertising->addServiceUUID(hid->hidService()->getUUID());
  advertising->setScanResponse(false);
  advertising->start();
#else
  applyAdvertising();
#endif
  hid->setBatteryLevel(batteryLevel);

  ESP_LOGD(LOG_TAG, "Advertising started!");
}

// ---------------------------------------------------------------------
//  ADVERTISING (Pocket Nova addition)
//  While it waits for a connection, a Bluetooth device broadcasts a small
//  "advertising" packet a few times a second: max 31 bytes, made of
//  [length][type][data...] sections. Phones and PCs read these packets to
//  build their "devices nearby" lists, without connecting to anything.
//
//  Ours holds:
//    Flags       02 01 06              "LE only, discoverable"
//    Appearance  03 19 C1 03           0x03C1 = keyboard (picks the icon)
//    Services    03 03 12 18           0x1812 = HID, "I'm an input device"
//    Swift Pair  06 FF 06 00 03 00 80  (only while swiftPair is on)
//    Name        the name, if it fits
//
//  SWIFT PAIR: FF = "manufacturer data". 06 00 is Microsoft's company ID,
//  03 means "Swift Pair beacon", 00 is the scenario (Bluetooth LE) and 80
//  is a reserved byte. When Windows sees this nearby, it pops up
//  "New Bluetooth device found - Connect". Windows only shows it when the
//  device is close, judged by signal strength (RSSI).
//
//  The full name goes in the "scan response" too: a second 31-byte
//  packet a PC can ask for, so a long name still shows up in full.
// ---------------------------------------------------------------------
void NovaKeyboard::setSwiftPair(bool on) {
  if (on == swiftPair) return;
  swiftPair = on;
  if (advertising) applyAdvertising();
}

void NovaKeyboard::applyAdvertising(void) {
#if !defined(USE_NIMBLE)
  BLEAdvertisementData adv;
  adv.setFlags(ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT);
  adv.setAppearance(HID_KEYBOARD);
  adv.setCompleteServices(BLEUUID((uint16_t)0x1812));
  if (swiftPair) adv.setManufacturerData(std::string("\x06\x00\x03\x00\x80", 5));
  int room = 31 - (int)adv.getPayload().length() - 2;   // 2 = the name's length and type bytes
  if ((int)deviceName.length() <= room) adv.setName(deviceName);
  else if (room > 0) adv.setShortName(deviceName.substr(0, room));

  // Scan response: the remote service's 128-bit ID (18 bytes) so the phone
  // page can find Pocket Nova by it, then as much of the name as fits.
  BLEAdvertisementData scan;
  if (!scanServiceUuid.empty()) scan.setCompleteServices(BLEUUID(scanServiceUuid));
  int sroom = 31 - (int)scan.getPayload().length() - 2;
  if ((int)deviceName.length() <= sroom) scan.setName(deviceName);
  else if (sroom > 0) scan.setShortName(deviceName.substr(0, sroom));

  advertising->stop();   // new data takes effect on the next start
  advertising->setAdvertisementData(adv);
  advertising->setScanResponseData(scan);
  if (links < 3) advertising->start();
#endif
}

void NovaKeyboard::end(void)
{
}

bool NovaKeyboard::isConnected(void) {
  return this->connected;
}

void NovaKeyboard::setBatteryLevel(uint8_t level) {
  this->batteryLevel = level;
  if (hid != 0)
    this->hid->setBatteryLevel(this->batteryLevel);
}

//must be called before begin in order to set the name
void NovaKeyboard::setName(std::string deviceName) {
  this->deviceName = deviceName;
}

/**
 * @brief Sets the waiting time (in milliseconds) between multiple keystrokes in NimBLE mode.
 * 
 * @param ms Time in milliseconds
 */
void NovaKeyboard::setDelay(uint32_t ms) {
  this->_delay_ms = ms;
}

void NovaKeyboard::sendReport(KeyReport* keys)
{
  if (sink && sink(KEYBOARD_ID, (const uint8_t*)keys, sizeof(KeyReport))) return;
  if (this->isConnected())
  {
    this->inputKeyboard->setValue((uint8_t*)keys, sizeof(KeyReport));
    this->inputKeyboard->notify();
#if defined(USE_NIMBLE)        
    // vTaskDelay(delayTicks);
    this->delay_ms(_delay_ms);
#endif // USE_NIMBLE
  }	
}

void NovaKeyboard::sendReport(MediaKeyReport* keys)
{
  if (sink && sink(MEDIA_KEYS_ID, (const uint8_t*)keys, sizeof(MediaKeyReport))) return;
  if (this->isConnected())
  {
    this->inputMediaKeys->setValue((uint8_t*)keys, sizeof(MediaKeyReport));
    this->inputMediaKeys->notify();
#if defined(USE_NIMBLE)        
    //vTaskDelay(delayTicks);
    this->delay_ms(_delay_ms);
#endif // USE_NIMBLE
  }	
}

// ---------------------------------------------------------------------
//  MOUSE (Pocket Nova addition). A mouse never says where the cursor IS,
//  only how far it moved since the last report; the PC adds it up.
// ---------------------------------------------------------------------
void NovaKeyboard::mouseReport(int8_t x, int8_t y, int8_t wheel) {
  uint8_t r[4] = {_mouseButtons, (uint8_t)x, (uint8_t)y, (uint8_t)wheel};
  if (sink && sink(MOUSE_ID, r, sizeof(r))) return;
  if (!this->isConnected()) return;
  this->inputMouse->setValue(r, sizeof(r));
  this->inputMouse->notify();
}

static int8_t clamp127(int v) { return v > 127 ? 127 : v < -127 ? -127 : v; }

void NovaKeyboard::mouseMove(int x, int y, int wheel) {
  // Bigger moves than one report can hold go out as several.
  while (x || y || wheel) {
    int8_t sx = clamp127(x), sy = clamp127(y), sw = clamp127(wheel);
    mouseReport(sx, sy, sw);
    x -= sx; y -= sy; wheel -= sw;
  }
}

void NovaKeyboard::mousePress(uint8_t b)   { _mouseButtons |= b;  mouseReport(0, 0, 0); }
void NovaKeyboard::mouseRelease(uint8_t b) { _mouseButtons &= ~b; mouseReport(0, 0, 0); }
void NovaKeyboard::mouseClick(uint8_t b)   { mousePress(b); mouseRelease(b); }

extern
const uint8_t _asciimap[128] PROGMEM;

#define SHIFT 0x80
const uint8_t _asciimap[128] =
{
	0x00,             // NUL
	0x00,             // SOH
	0x00,             // STX
	0x00,             // ETX
	0x00,             // EOT
	0x00,             // ENQ
	0x00,             // ACK
	0x00,             // BEL
	0x2a,			// BS	Backspace
	0x2b,			// TAB	Tab
	0x28,			// LF	Enter
	0x00,             // VT
	0x00,             // FF
	0x00,             // CR
	0x00,             // SO
	0x00,             // SI
	0x00,             // DEL
	0x00,             // DC1
	0x00,             // DC2
	0x00,             // DC3
	0x00,             // DC4
	0x00,             // NAK
	0x00,             // SYN
	0x00,             // ETB
	0x00,             // CAN
	0x00,             // EM
	0x00,             // SUB
	0x00,             // ESC
	0x00,             // FS
	0x00,             // GS
	0x00,             // RS
	0x00,             // US

	0x2c,		   //  ' '
	0x1e|SHIFT,	   // !
	0x34|SHIFT,	   // "
	0x20|SHIFT,    // #
	0x21|SHIFT,    // $
	0x22|SHIFT,    // %
	0x24|SHIFT,    // &
	0x34,          // '
	0x26|SHIFT,    // (
	0x27|SHIFT,    // )
	0x25|SHIFT,    // *
	0x2e|SHIFT,    // +
	0x36,          // ,
	0x2d,          // -
	0x37,          // .
	0x38,          // /
	0x27,          // 0
	0x1e,          // 1
	0x1f,          // 2
	0x20,          // 3
	0x21,          // 4
	0x22,          // 5
	0x23,          // 6
	0x24,          // 7
	0x25,          // 8
	0x26,          // 9
	0x33|SHIFT,      // :
	0x33,          // ;
	0x36|SHIFT,      // <
	0x2e,          // =
	0x37|SHIFT,      // >
	0x38|SHIFT,      // ?
	0x1f|SHIFT,      // @
	0x04|SHIFT,      // A
	0x05|SHIFT,      // B
	0x06|SHIFT,      // C
	0x07|SHIFT,      // D
	0x08|SHIFT,      // E
	0x09|SHIFT,      // F
	0x0a|SHIFT,      // G
	0x0b|SHIFT,      // H
	0x0c|SHIFT,      // I
	0x0d|SHIFT,      // J
	0x0e|SHIFT,      // K
	0x0f|SHIFT,      // L
	0x10|SHIFT,      // M
	0x11|SHIFT,      // N
	0x12|SHIFT,      // O
	0x13|SHIFT,      // P
	0x14|SHIFT,      // Q
	0x15|SHIFT,      // R
	0x16|SHIFT,      // S
	0x17|SHIFT,      // T
	0x18|SHIFT,      // U
	0x19|SHIFT,      // V
	0x1a|SHIFT,      // W
	0x1b|SHIFT,      // X
	0x1c|SHIFT,      // Y
	0x1d|SHIFT,      // Z
	0x2f,          // [
	0x31,          // bslash
	0x30,          // ]
	0x23|SHIFT,    // ^
	0x2d|SHIFT,    // _
	0x35,          // `
	0x04,          // a
	0x05,          // b
	0x06,          // c
	0x07,          // d
	0x08,          // e
	0x09,          // f
	0x0a,          // g
	0x0b,          // h
	0x0c,          // i
	0x0d,          // j
	0x0e,          // k
	0x0f,          // l
	0x10,          // m
	0x11,          // n
	0x12,          // o
	0x13,          // p
	0x14,          // q
	0x15,          // r
	0x16,          // s
	0x17,          // t
	0x18,          // u
	0x19,          // v
	0x1a,          // w
	0x1b,          // x
	0x1c,          // y
	0x1d,          // z
	0x2f|SHIFT,    // {
	0x31|SHIFT,    // |
	0x30|SHIFT,    // }
	0x35|SHIFT,    // ~
	0				// DEL
};


uint8_t USBPutChar(uint8_t c);

// press() adds the specified key (printing, non-printing, or modifier)
// to the persistent key report and sends the report.  Because of the way
// USB HID works, the host acts like the key remains pressed until we
// call release(), releaseAll(), or otherwise clear the report and resend.
size_t NovaKeyboard::press(uint8_t k)
{
	uint8_t i;
	if (k >= 136) {			// it's a non-printing key (not a modifier)
		k = k - 136;
	} else if (k >= 128) {	// it's a modifier key
		_keyReport.modifiers |= (1<<(k-128));
		k = 0;
	} else {				// it's a printing key
		k = pgm_read_byte(_asciimap + k);
		if (!k) {
			setWriteError();
			return 0;
		}
		if (k & 0x80) {						// it's a capital letter or other character reached with shift
			_keyReport.modifiers |= 0x02;	// the left shift modifier
			k &= 0x7F;
		}
	}

	// Add k to the key report only if it's not already present
	// and if there is an empty slot.
	if (_keyReport.keys[0] != k && _keyReport.keys[1] != k &&
		_keyReport.keys[2] != k && _keyReport.keys[3] != k &&
		_keyReport.keys[4] != k && _keyReport.keys[5] != k) {

		for (i=0; i<6; i++) {
			if (_keyReport.keys[i] == 0x00) {
				_keyReport.keys[i] = k;
				break;
			}
		}
		if (i == 6) {
			setWriteError();
			return 0;
		}
	}
	sendReport(&_keyReport);
	return 1;
}

size_t NovaKeyboard::press(const MediaKeyReport k)
{
    uint16_t k_16 = k[1] | (k[0] << 8);
    uint16_t mediaKeyReport_16 = _mediaKeyReport[1] | (_mediaKeyReport[0] << 8);

    mediaKeyReport_16 |= k_16;
    _mediaKeyReport[0] = (uint8_t)((mediaKeyReport_16 & 0xFF00) >> 8);
    _mediaKeyReport[1] = (uint8_t)(mediaKeyReport_16 & 0x00FF);

	sendReport(&_mediaKeyReport);
	return 1;
}

// release() takes the specified key out of the persistent key report and
// sends the report.  This tells the OS the key is no longer pressed and that
// it shouldn't be repeated any more.
size_t NovaKeyboard::release(uint8_t k)
{
	uint8_t i;
	if (k >= 136) {			// it's a non-printing key (not a modifier)
		k = k - 136;
	} else if (k >= 128) {	// it's a modifier key
		_keyReport.modifiers &= ~(1<<(k-128));
		k = 0;
	} else {				// it's a printing key
		k = pgm_read_byte(_asciimap + k);
		if (!k) {
			return 0;
		}
		if (k & 0x80) {							// it's a capital letter or other character reached with shift
			_keyReport.modifiers &= ~(0x02);	// the left shift modifier
			k &= 0x7F;
		}
	}

	// Test the key report to see if k is present.  Clear it if it exists.
	// Check all positions in case the key is present more than once (which it shouldn't be)
	for (i=0; i<6; i++) {
		if (0 != k && _keyReport.keys[i] == k) {
			_keyReport.keys[i] = 0x00;
		}
	}

	sendReport(&_keyReport);
	return 1;
}

size_t NovaKeyboard::release(const MediaKeyReport k)
{
    uint16_t k_16 = k[1] | (k[0] << 8);
    uint16_t mediaKeyReport_16 = _mediaKeyReport[1] | (_mediaKeyReport[0] << 8);
    mediaKeyReport_16 &= ~k_16;
    _mediaKeyReport[0] = (uint8_t)((mediaKeyReport_16 & 0xFF00) >> 8);
    _mediaKeyReport[1] = (uint8_t)(mediaKeyReport_16 & 0x00FF);

	sendReport(&_mediaKeyReport);
	return 1;
}

void NovaKeyboard::releaseAll(void)
{
	_keyReport.keys[0] = 0;
	_keyReport.keys[1] = 0;
	_keyReport.keys[2] = 0;
	_keyReport.keys[3] = 0;
	_keyReport.keys[4] = 0;
	_keyReport.keys[5] = 0;
	_keyReport.modifiers = 0;
    _mediaKeyReport[0] = 0;
    _mediaKeyReport[1] = 0;
	sendReport(&_keyReport);
}

size_t NovaKeyboard::write(uint8_t c)
{
	uint8_t p = press(c);  // Keydown
	release(c);            // Keyup
	return p;              // just return the result of press() since release() almost always returns 1
}

size_t NovaKeyboard::write(const MediaKeyReport c)
{
	uint16_t p = press(c);  // Keydown
	release(c);            // Keyup
	return p;              // just return the result of press() since release() almost always returns 1
}

size_t NovaKeyboard::write(const uint8_t *buffer, size_t size) {
	size_t n = 0;
	while (size--) {
		if (*buffer != '\r') {
			if (write(*buffer)) {
			  n++;
			} else {
			  break;
			}
		}
		buffer++;
	}
	return n;
}

void NovaKeyboard::onConnect(BLEServer* pServer) {
#if !defined(USE_NIMBLE)
  // THE FIX: a host only "subscribes" to key reports once, when it first
  // pairs. The ESP32 forgets that subscription whenever it restarts, but a
  // paired Windows PC reconnects assuming it's still on, so every key
  // press was silently dropped (notify() skips unsubscribed reports).
  // Turning the subscription back on at each connect makes keys flow again.
  BLE2902* desc = (BLE2902*)this->inputKeyboard->getDescriptorByUUID(BLEUUID((uint16_t)0x2902));
  if (desc) desc->setNotifications(true);
  desc = (BLE2902*)this->inputMediaKeys->getDescriptorByUUID(BLEUUID((uint16_t)0x2902));
  if (desc) desc->setNotifications(true);
  desc = (BLE2902*)this->inputMouse->getDescriptorByUUID(BLEUUID((uint16_t)0x2902));
  if (desc) desc->setNotifications(true);
#endif
}

// ---------------------------------------------------------------------
//  SEVERAL CONNECTIONS (Pocket Nova addition)
//  A Bluetooth LE device can hold a few connections at once (4 here). The
//  PC uses one as the keyboard host; the phone remote page uses another.
//  Each connection has a number (conn_id). Connections that talk to the
//  remote service are marked as "remotes" and don't count as the keyboard
//  host, so the media/keys apps still know whether the PC is there.
// ---------------------------------------------------------------------
void NovaKeyboard::onConnect(BLEServer* pServer, esp_ble_gatts_cb_param_t* param) {
  uint16_t id = param->connect.conn_id;
  if (id < 16) { memcpy(peerAddr[id], param->connect.remote_bda, 6); peerMask |= 1u << id; }
  links++;
  this->connected = hostLinks() > 0;
#if !defined(USE_NIMBLE)
  if (links < 3) advertising->start();   // keep advertising so another device can join
#endif
}

void NovaKeyboard::onDisconnect(BLEServer* pServer, esp_ble_gatts_cb_param_t* param) {
  uint16_t id = param->disconnect.conn_id;
  if (links) links--;
  if (id < 16) { remoteMask &= ~(1u << id); peerMask &= ~(1u << id); }
  this->connected = hostLinks() > 0;
#if !defined(USE_NIMBLE)
  advertising->start();
#endif  // !USE_NIMBLE
}

void NovaKeyboard::markRemote(uint16_t connId) {
  if (connId >= 16 || (remoteMask & (1u << connId))) return;
  remoteMask |= 1u << connId;
  this->connected = hostLinks() > 0;
}

bool NovaKeyboard::hostAddress(uint8_t* out) {
  uint16_t hosts = peerMask & ~remoteMask;
  for (int id = 0; id < 16; id++)
    if (hosts & (1u << id)) { memcpy(out, peerAddr[id], 6); return true; }
  return false;
}

bool NovaKeyboard::peerAddress(uint16_t connId, uint8_t* out) {
  if (connId >= 16 || !(peerMask & (1u << connId))) return false;
  memcpy(out, peerAddr[connId], 6);
  return true;
}

int NovaKeyboard::hostLinks() {
  int remotes = __builtin_popcount(remoteMask);
  return links > remotes ? links - remotes : 0;
}

void NovaKeyboard::onWrite(BLECharacteristic* me) {
  uint8_t* value = (uint8_t*)(me->getValue().c_str());
  (void)value;
  ESP_LOGI(LOG_TAG, "special keys: %d", *value);
}

void NovaKeyboard::delay_ms(uint64_t ms) {
  uint64_t m = esp_timer_get_time();
  if(ms){
    uint64_t e = (m + (ms * 1000));
    if(m > e){ //overflow
        while(esp_timer_get_time() > e) { }
    }
    while(esp_timer_get_time() < e) {}
  }
}