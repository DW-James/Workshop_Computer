/*
 * TinyUSB configuration for Robodub USB MIDI device.
 * Adapted from ComputerCard web_interface example.
 *
 * Enables MIDI device class only — no CDC, HID, MSC, or Vendor.
 * SysEx messages carry bidirectional config data between the
 * Robodub firmware and a web interface (WebMIDI in Chrome).
 */

#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
 extern "C" {
#endif

//--------------------------------------------------------------------
// COMMON CONFIGURATION
//--------------------------------------------------------------------

#ifndef CFG_TUSB_MCU
  #error CFG_TUSB_MCU must be defined
#endif

#ifndef BOARD_DEVICE_RHPORT_NUM
  #define BOARD_DEVICE_RHPORT_NUM     0
#endif

#ifndef BOARD_DEVICE_RHPORT_SPEED
  #define BOARD_DEVICE_RHPORT_SPEED   OPT_MODE_FULL_SPEED
#endif

#if   BOARD_DEVICE_RHPORT_NUM == 0
  #define CFG_TUSB_RHPORT0_MODE     (OPT_MODE_DEVICE | BOARD_DEVICE_RHPORT_SPEED)
#elif BOARD_DEVICE_RHPORT_NUM == 1
  #define CFG_TUSB_RHPORT1_MODE     (OPT_MODE_DEVICE | BOARD_DEVICE_RHPORT_SPEED)
#else
  #error "Incorrect RHPort configuration"
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS               OPT_OS_NONE
#endif

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN          __attribute__ ((aligned(4)))
#endif

//--------------------------------------------------------------------
// DEVICE CONFIGURATION
//--------------------------------------------------------------------

#ifndef CFG_TUD_ENDPOINT0_SIZE
#define CFG_TUD_ENDPOINT0_SIZE    64
#endif

// Only MIDI enabled — all other USB classes disabled
#define CFG_TUD_HID               0
#define CFG_TUD_CDC               0
#define CFG_TUD_MSC               0
#define CFG_TUD_MIDI              1
#define CFG_TUD_VENDOR            0

// MIDI FIFO size of TX and RX
#define CFG_TUD_MIDI_RX_BUFSIZE   64
#define CFG_TUD_MIDI_TX_BUFSIZE   64

#ifdef __cplusplus
 }
#endif

#endif /* _TUSB_CONFIG_H_ */
