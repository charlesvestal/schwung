/*
 * Copyright (C) 2021, 2024 nukeykt
 *
 *  Redistribution and use of this code or any derivative works are permitted
 *  provided that the following conditions are met:
 *
 *   - Redistributions may not be sold, nor may they be used in a commercial
 *     product or activity.
 *
 *   - Redistributions that are modified from the original source must include
 * the complete source code, including the source code for all components used
 * by a binary built from the modified sources. However, as a special exception,
 * the source code distributed need not include anything that is normally
 * distributed (in either source or binary form) with the major components
 * (compiler, kernel, and so on) of the operating system on which the executable
 * runs, unless that component itself accompanies the executable.
 *
 *   - Redistributions must reproduce the above copyright notice, this list of
 *     conditions and the following disclaimer in the documentation and/or other
 *     materials provided with the distribution.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 */
#pragma once

// #include "SDL.h"
#include "lcd.h"
#include "mcu_opcodes.h"
#include "pcm.h"
#include <stdint.h>
#include <vector>

enum {
  DEV_P1DDR = 0x00,
  DEV_P5DDR = 0x08,
  DEV_P6DDR = 0x09,
  DEV_P7DDR = 0x0c,
  DEV_P7DR = 0x0e,
  DEV_FRT1_TCR = 0x10,
  DEV_FRT1_TCSR = 0x11,
  DEV_FRT1_FRCH = 0x12,
  DEV_FRT1_FRCL = 0x13,
  DEV_FRT1_OCRAH = 0x14,
  DEV_FRT1_OCRAL = 0x15,
  DEV_FRT2_TCR = 0x20,
  DEV_FRT2_TCSR = 0x21,
  DEV_FRT2_FRCH = 0x22,
  DEV_FRT2_FRCL = 0x23,
  DEV_FRT2_OCRAH = 0x24,
  DEV_FRT2_OCRAL = 0x25,
  DEV_FRT3_TCR = 0x30,
  DEV_FRT3_TCSR = 0x31,
  DEV_FRT3_FRCH = 0x32,
  DEV_FRT3_FRCL = 0x33,
  DEV_FRT3_OCRAH = 0x34,
  DEV_FRT3_OCRAL = 0x35,
  DEV_PWM1_TCR = 0x40,
  DEV_PWM1_DTR = 0x41,
  DEV_PWM2_TCR = 0x44,
  DEV_PWM2_DTR = 0x45,
  DEV_PWM3_TCR = 0x48,
  DEV_PWM3_DTR = 0x49,
  DEV_TMR_TCR = 0x50,
  DEV_TMR_TCSR = 0x51,
  DEV_TMR_TCORA = 0x52,
  DEV_TMR_TCORB = 0x53,
  DEV_TMR_TCNT = 0x54,
  DEV_SMR = 0x58,
  DEV_BRR = 0x59,
  DEV_SCR = 0x5a,
  DEV_TDR = 0x5b,
  DEV_SSR = 0x5c,
  DEV_RDR = 0x5d,
  DEV_ADDRAH = 0x60,
  DEV_ADDRAL = 0x61,
  DEV_ADDRBH = 0x62,
  DEV_ADDRBL = 0x63,
  DEV_ADDRCH = 0x64,
  DEV_ADDRCL = 0x65,
  DEV_ADDRDH = 0x66,
  DEV_ADDRDL = 0x67,
  DEV_ADCSR = 0x68,
  DEV_IPRA = 0x70,
  DEV_IPRB = 0x71,
  DEV_IPRC = 0x72,
  DEV_IPRD = 0x73,
  DEV_DTEA = 0x74,
  DEV_DTEB = 0x75,
  DEV_DTEC = 0x76,
  DEV_DTED = 0x77,
  DEV_WCR = 0x78,
  DEV_RAME = 0x79,
  DEV_P1CR = 0x7c,
  DEV_P9DDR = 0x7e,
  DEV_P9DR = 0x7f,
};

const uint16_t sr_mask = 0x870f;
enum {
  STATUS_T = 0x8000,
  STATUS_N = 0x08,
  STATUS_Z = 0x04,
  STATUS_V = 0x02,
  STATUS_C = 0x01,
  STATUS_INT_MASK = 0x700
};

enum {
  VECTOR_RESET = 0,
  VECTOR_RESERVED1, // UNUSED
  VECTOR_INVALID_INSTRUCTION,
  VECTOR_DIVZERO,
  VECTOR_TRAP,
  VECTOR_RESERVED2, // UNUSED
  VECTOR_RESERVED3, // UNUSED
  VECTOR_RESERVED4, // UNUSED
  VECTOR_ADDRESS_ERROR,
  VECTOR_TRACE,
  VECTOR_RESERVED5, // UNUSED
  VECTOR_NMI,
  VECTOR_RESERVED6, // UNUSED
  VECTOR_RESERVED7, // UNUSED
  VECTOR_RESERVED8, // UNUSED
  VECTOR_RESERVED9, // UNUSED
  VECTOR_TRAPA_0,
  VECTOR_TRAPA_1,
  VECTOR_TRAPA_2,
  VECTOR_TRAPA_3,
  VECTOR_TRAPA_4,
  VECTOR_TRAPA_5,
  VECTOR_TRAPA_6,
  VECTOR_TRAPA_7,
  VECTOR_TRAPA_8,
  VECTOR_TRAPA_9,
  VECTOR_TRAPA_A,
  VECTOR_TRAPA_B,
  VECTOR_TRAPA_C,
  VECTOR_TRAPA_D,
  VECTOR_TRAPA_E,
  VECTOR_TRAPA_F,
  VECTOR_IRQ0,
  VECTOR_IRQ1,
  VECTOR_INTERNAL_INTERRUPT_88, // UNUSED
  VECTOR_INTERNAL_INTERRUPT_8C, // UNUSED
  VECTOR_INTERNAL_INTERRUPT_90, // FRT1 ICI
  VECTOR_INTERNAL_INTERRUPT_94, // FRT1 OCIA
  VECTOR_INTERNAL_INTERRUPT_98, // FRT1 OCIB
  VECTOR_INTERNAL_INTERRUPT_9C, // FRT1 FOVI
  VECTOR_INTERNAL_INTERRUPT_A0, // FRT2 ICI
  VECTOR_INTERNAL_INTERRUPT_A4, // FRT2 OCIA
  VECTOR_INTERNAL_INTERRUPT_A8, // FRT2 OCIB
  VECTOR_INTERNAL_INTERRUPT_AC, // FRT2 FOVI
  VECTOR_INTERNAL_INTERRUPT_B0, // FRT3 ICI
  VECTOR_INTERNAL_INTERRUPT_B4, // FRT3 OCIA
  VECTOR_INTERNAL_INTERRUPT_B8, // FRT3 OCIB
  VECTOR_INTERNAL_INTERRUPT_BC, // FRT3 FOVI
  VECTOR_INTERNAL_INTERRUPT_C0, // CMIA
  VECTOR_INTERNAL_INTERRUPT_C4, // CMIB
  VECTOR_INTERNAL_INTERRUPT_C8, // OVI
  VECTOR_INTERNAL_INTERRUPT_CC, // UNUSED
  VECTOR_INTERNAL_INTERRUPT_D0, // ERI
  VECTOR_INTERNAL_INTERRUPT_D4, // RXI
  VECTOR_INTERNAL_INTERRUPT_D8, // TXI
  VECTOR_INTERNAL_INTERRUPT_DC, // UNUSED
  VECTOR_INTERNAL_INTERRUPT_E0, // ADI
};

enum {
  INTERRUPT_SOURCE_NMI = 0,
  INTERRUPT_SOURCE_IRQ0, // GPINT
  INTERRUPT_SOURCE_IRQ1,
  INTERRUPT_SOURCE_FRT0_ICI,
  INTERRUPT_SOURCE_FRT0_OCIA,
  INTERRUPT_SOURCE_FRT0_OCIB,
  INTERRUPT_SOURCE_FRT0_FOVI,
  INTERRUPT_SOURCE_FRT1_ICI,
  INTERRUPT_SOURCE_FRT1_OCIA,
  INTERRUPT_SOURCE_FRT1_OCIB,
  INTERRUPT_SOURCE_FRT1_FOVI,
  INTERRUPT_SOURCE_FRT2_ICI,
  INTERRUPT_SOURCE_FRT2_OCIA,
  INTERRUPT_SOURCE_FRT2_OCIB,
  INTERRUPT_SOURCE_FRT2_FOVI,
  INTERRUPT_SOURCE_TIMER_CMIA,
  INTERRUPT_SOURCE_TIMER_CMIB,
  INTERRUPT_SOURCE_TIMER_OVI,
  INTERRUPT_SOURCE_ANALOG,
  INTERRUPT_SOURCE_UART_RX,
  INTERRUPT_SOURCE_UART_TX,
  INTERRUPT_SOURCE_MAX
};

enum {
  EXCEPTION_SOURCE_ADDRESS_ERROR = 0,
  EXCEPTION_SOURCE_INVALID_INSTRUCTION,
  EXCEPTION_SOURCE_TRACE,
};

struct mcu_t {
  uint16_t r[8];
  uint16_t pc;
  uint16_t sr;
  uint8_t cp, dp, ep, tp, br;
  uint8_t sleep;
  uint8_t ex_ignore;
  int32_t exception_pending;
  uint8_t interrupt_pending[INTERRUPT_SOURCE_MAX];
  uint8_t trapa_pending[16];
  uint64_t cycles;
};

enum {
  // JV880
  MCU_BUTTON_CURSOR_L = 0,
  MCU_BUTTON_CURSOR_R = 1,
  MCU_BUTTON_TONE_SELECT = 2,
  MCU_BUTTON_MUTE = 3,
  MCU_BUTTON_DATA = 4,
  MCU_BUTTON_MONITOR = 5,
  MCU_BUTTON_COMPARE = 6,
  MCU_BUTTON_ENTER = 7,
  MCU_BUTTON_UTILITY = 8,
  MCU_BUTTON_PREVIEW = 9,
  MCU_BUTTON_PATCH_PERFORM = 10,
  MCU_BUTTON_EDIT = 11,
  MCU_BUTTON_SYSTEM = 12,
  MCU_BUTTON_RHYTHM = 13,
};

constexpr uint32_t ANALOG_LEVEL_BATTERY = 0x2a0;

static const int ROM1_SIZE = 0x8000;
static const int ROM2_SIZE = 0x40000;
static const int RAM_SIZE = 0x400;
static const int SRAM_SIZE = 0x8000;
static const int NVRAM_SIZE = 0x8000;   // JV880 only
static const int CARDRAM_SIZE = 0x8000; // JV880 only
static const int ROMSM_SIZE = 0x1000;
const uint32_t uart_buffer_size = 8192;

static const int audio_buffer_size = 4096;

struct MCU {
  uint32_t mcu_button_pressed;

  mcu_t mcu;

  uint8_t rom1[ROM1_SIZE];
  uint8_t rom2[ROM2_SIZE];
  uint8_t ram[RAM_SIZE];
  uint8_t sram[SRAM_SIZE];
  uint8_t nvram[NVRAM_SIZE];
  uint8_t cardram[CARDRAM_SIZE];

  int rom2_mask = ROM2_SIZE - 1;

  int ga_int[8] = {0};
  int ga_int_enable = 0;
  int ga_int_trigger = 0;
  int ga_lcd_counter = 0;

  uint8_t dev_register[0x80] = {0};

  uint8_t io_sd = 0x00;

  int adf_rd = 0;
  uint64_t analog_end_time = 0;

  int ssr_rd = 0;

  bool midi_ready = false;

  uint32_t uart_write_ptr;
  uint32_t uart_read_ptr;
  uint8_t uart_buffer[uart_buffer_size];
  uint8_t uart_rx_byte;
  uint64_t uart_rx_delay;
  uint64_t uart_tx_delay;

  uint32_t operand_type;
  uint16_t operand_ea;
  uint8_t operand_ep;
  uint8_t operand_size;
  uint8_t operand_reg;
  uint8_t operand_status;
  uint16_t operand_data;
  uint8_t opcode_extended;

  uint8_t timer_tempreg;

  bool timer8_enabled;
  bool timer8_cmiea;
  bool timer8_cmfa;
  bool timer8_cmfa_read;
  uint8_t timer8_tcora;
  uint8_t timer8_tcnt;

  uint16_t timer0_ocra;
  uint16_t timer1_ocra;
  uint16_t timer2_ocra;
  uint16_t timer0_frc;
  uint16_t timer1_frc;
  uint16_t timer2_frc;
  bool timer0_ocfa;
  bool timer1_ocfa;
  bool timer2_ocfa;
  bool timer0_ocfa_read;
  bool timer1_ocfa_read;
  bool timer2_ocfa_read;
  bool timer0_ociea;
  bool timer1_ociea;
  bool timer2_ociea;

  Pcm pcm;
  LCD lcd;

  int16_t sample_buffer[audio_buffer_size] = {0};
  int sample_write_ptr = 0;

  MCU();

  int startSC55(const uint8_t *s_rom1, const uint8_t *s_rom2,
                const uint8_t *s_waverom1, const uint8_t *s_waverom2,
                const uint8_t *s_nvram);
  void updateSC55(const int nSamples);
  void postMidiSC55(const uint8_t *message, int length);
  void SC55_Reset();
  void MCU_PostUART(const uint8_t data);
  void MCU_EncoderTrigger(const int dir);

  void MCU_ErrorTrap();

  uint8_t MCU_Read(uint32_t address);
  void MCU_Write(uint32_t address, const uint8_t value);

  void MCU_GA_SetGAInt(const int line, const int value);
  void MCU_UpdateUART_RX();
  void MCU_UpdateUART_TX();

  uint16_t MCU_AnalogReadPin(const uint32_t pin);
  void MCU_AnalogSample(const int channel);
  void MCU_DeviceWrite(uint32_t address, const uint8_t data);
  uint8_t MCU_DeviceRead(uint32_t address);
  void MCU_DeviceReset();
  void MCU_UpdateAnalog(const uint64_t cycles);
  void MCU_Init();
  void MCU_Reset();
  void MCU_PatchROM();

  void MCU_Interrupt_Handle();

  void TIMER_Reset();
  void TIMER_Write(const uint32_t address, const uint8_t data);
  uint8_t TIMER_Read(const uint32_t address);
  void TIMER_Clock(const uint64_t cycles);

  void TIMER2_Write(const uint32_t address, const uint8_t data);
  uint8_t TIMER_Read2(const uint32_t address);

  inline void MCU_PostSample(int *sample) {
    sample_buffer[sample_write_ptr++] = sample[0] >> 16;
    sample_buffer[sample_write_ptr++] = sample[1] >> 16;
    sample_write_ptr %= audio_buffer_size;

    // int ptr = SDL_AtomicGet(&sample_write_ptr);
    // sample_buffer[ptr] = sample[0] >> 16;
    // sample_buffer[ptr + 1] = sample[1] >> 16;
    // SDL_AtomicSet(&sample_write_ptr, (ptr + 2) % audio_buffer_size);
  }

  inline uint32_t MCU_GetAddress(const uint8_t page, const uint16_t address) {
    return (page << 16) | address;
  }

  inline uint8_t MCU_ReadCodeAdvance() {
    // uint8_t ret = MCU_Read(MCU_GetAddress(mcu.cp, mcu.pc));
    uint8_t ret;
    if (mcu.cp == 0)
      ret = rom1[mcu.pc];
    else
      ret = rom2[((mcu.cp << 16) | mcu.pc) & rom2_mask];
    mcu.pc++;
    return ret;
  }

  inline void MCU_SetRegisterByte(const uint8_t reg, const uint8_t val) {
    mcu.r[reg] = val;
  }

  inline uint32_t MCU_GetVectorAddress(const uint32_t vector) {
    return MCU_Read32(vector * 4);
  }

  inline uint32_t MCU_GetPageForRegister(const uint32_t reg) {
    if (reg >= 6)
      return mcu.tp;
    else if (reg >= 4)
      return mcu.ep;
    return mcu.dp;
  }

  inline void MCU_ControlRegisterWrite(const uint32_t reg, const uint32_t siz,
                                       const uint32_t data) {
    if (siz) {
      if (reg == 0) {
        mcu.sr = data;
        mcu.sr &= sr_mask;
      } else if (reg == 5) // FIXME: undocumented
      {
        mcu.dp = data & 0xff;
      } else if (reg == 4) // FIXME: undocumented
      {
        mcu.ep = data & 0xff;
      } else if (reg == 3) // FIXME: undocumented
      {
        mcu.br = data & 0xff;
      } else {
        MCU_ErrorTrap();
      }
    } else {
      if (reg == 1) {
        mcu.sr &= ~0xff;
        mcu.sr |= data & 0xff;
        mcu.sr &= sr_mask;
      } else if (reg == 3) {
        mcu.br = data;
      } else if (reg == 4) {
        mcu.ep = data;
      } else if (reg == 5) {
        mcu.dp = data;
      } else if (reg == 7) {
        mcu.tp = data;
      } else {
        MCU_ErrorTrap();
      }
    }
  }

  inline uint32_t MCU_ControlRegisterRead(const uint32_t reg,
                                          const uint32_t siz) {
    uint32_t ret = 0;
    if (siz) {
      if (reg == 0) {
        ret = mcu.sr & sr_mask;
      } else if (reg == 5) // FIXME: undocumented
      {
        ret = mcu.dp | (mcu.dp << 8);
      } else if (reg == 4) // FIXME: undocumented
      {
        ret = mcu.ep | (mcu.ep << 8);
      } else if (reg == 3) // FIXME: undocumented
      {
        ret = mcu.br | (mcu.br << 8);
        ;
      } else {
        MCU_ErrorTrap();
      }
      ret &= 0xffff;
    } else {
      if (reg == 1) {
        ret = mcu.sr & sr_mask;
      } else if (reg == 3) {
        ret = mcu.br;
      } else if (reg == 4) {
        ret = mcu.ep;
      } else if (reg == 5) {
        ret = mcu.dp;
      } else if (reg == 7) {
        ret = mcu.tp;
      } else {
        MCU_ErrorTrap();
      }
      ret &= 0xff;
    }
    return ret;
  }

  inline void MCU_SetStatus(const uint32_t condition, const uint32_t mask) {
    if (condition)
      mcu.sr |= mask;
    else
      mcu.sr &= ~mask;
  }

  inline void MCU_PushStack(const uint16_t data) {
    if (mcu.r[7] & 1)
      MCU_Interrupt_Exception(EXCEPTION_SOURCE_ADDRESS_ERROR);
    mcu.r[7] -= 2;
    MCU_Write16(mcu.r[7], data);
  }

  inline uint16_t MCU_PopStack() {
    uint16_t ret;
    if (mcu.r[7] & 1)
      MCU_Interrupt_Exception(EXCEPTION_SOURCE_ADDRESS_ERROR);
    ret = MCU_Read16(mcu.r[7]);
    mcu.r[7] += 2;
    return ret;
  }

  inline uint16_t MCU_Read16(uint32_t address) {
    address &= ~1;
    uint8_t b0, b1;
    b0 = MCU_Read(address);
    b1 = MCU_Read(address + 1);
    return (b0 << 8) + b1;
  }

  inline uint32_t MCU_Read32(uint32_t address) {
    address &= ~3;
    uint8_t b0, b1, b2, b3;
    b0 = MCU_Read(address);
    b1 = MCU_Read(address + 1);
    b2 = MCU_Read(address + 2);
    b3 = MCU_Read(address + 3);
    return (b0 << 24) + (b1 << 16) + (b2 << 8) + b3;
  }

  inline void MCU_Write16(uint32_t address, uint16_t value) {
    address &= ~1;
    MCU_Write(address, value >> 8);
    MCU_Write(address + 1, value & 0xff);
  }

  inline void MCU_ReadInstruction() {
    uint8_t operand = MCU_ReadCodeAdvance();

    MCU_Operand_Table[operand](this, operand);

    if (mcu.sr & STATUS_T) {
      MCU_Interrupt_Exception(EXCEPTION_SOURCE_TRACE);
    }
  }

  inline void MCU_Interrupt_SetRequest(const uint32_t interrupt,
                                       const uint32_t value) {
    mcu.interrupt_pending[interrupt] = value;
  }

  inline void MCU_Interrupt_Exception(const uint32_t exception) {
    mcu.exception_pending = exception;
  }

  inline void MCU_Interrupt_TRAPA(const uint32_t vector) {
    mcu.trapa_pending[vector] = 1;
  }

  inline void MCU_Interrupt_StartVector(const uint32_t vector,
                                        const int32_t mask) {
    uint32_t address = MCU_GetVectorAddress(vector);
    MCU_PushStack(mcu.pc);
    MCU_PushStack(mcu.cp);
    MCU_PushStack(mcu.sr);
    mcu.sr &= ~STATUS_T;
    if (mask >= 0) {
      mcu.sr &= ~STATUS_INT_MASK;
      mcu.sr |= mask << 8;
    }
    mcu.sleep = 0;
    mcu.cp = address >> 16;
    mcu.pc = address;
  }
};
