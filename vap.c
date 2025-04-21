// Copyright 2021 Josh Bailey (josh@vandervecken.com)

// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "regid.h"
#include "vessel.h"
#include <6502.h>
#include <c64.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef FULL
#define VAP_BASE_NAME "VAP-FULL"
#else
#define VAP_BASE_NAME "VAP"
#endif

#ifdef POLL
#define VAP_NAME VAP_BASE_NAME "-POLL"
#else
#define VAP_NAME VAP_BASE_NAME
#endif

const char VAP_VERSION[] = VAP_NAME VERSION;

#define SCREENMEM ((volatile unsigned char *)0x0400)
#define COUNT ((volatile unsigned char *)(0x0400 + 40))
#define COUNT2 ((volatile unsigned char *)(0x0400 + 80))
#define SIDBASE ((volatile unsigned char *)0xd400)
#define SIDBASE2 ((volatile unsigned char *)0xd420)
#define R6510 (*(volatile unsigned char *)0x01)
#define SIDREGSIZE 28
#define NMI_VECTOR (*((volatile uint16_t *)0xfffa))
#define IRQ_VECTOR (*((volatile uint16_t *)0xfffe))

#define MIDI_CLOCK 0xF8
#define SYSEX_START 0xf0
#define ASID_MANID 0x2d
#define SYSEX_STOP 0xf7
#define NOTEOFF16 0x8f
#define NOTEOFF15 0x8e

#define CLOCK_ACK VW(MIDI_CLOCK)

enum ASID_CMD {
  ASID_CMD_START = 0x4c,
  ASID_CMD_STOP = 0x4d,
  ASID_CMD_UPDATE = 0x4e,
  ASID_CMD_UPDATE2 = 0x50,
  ASID_CMD_UPDATE_BOTH = 0x51,
  ASID_CMD_RUN_BUFFER = 0x52,
  ASID_CMD_LOAD_BUFFER = 0x53,
  ASID_CMD_ADDR_BUFFER = 0x54,
  ASID_CMD_LOAD_RECT_BUFFER = 0x55,
  ASID_CMD_ADDR_RECT_BUFFER = 0x56,
  ASID_CMD_FILL_BUFFER = 0x57,
  ASID_CMD_FILL_RECT_BUFFER = 0x58,
  ASID_CMD_COPY_BUFFER = 0x59,
  ASID_CMD_COPY_RECT_BUFFER = 0x5a,
  ASID_CMD_REU_STASH_BUFFER = 0x5b,
  ASID_CMD_REU_FETCH_BUFFER = 0x5c,
  ASID_CMD_REU_FILL_BUFFER = 0x5d,
  ASID_CMD_REU_STASH_BUFFER_RECT = 0x5e,
  ASID_CMD_REU_FETCH_BUFFER_RECT = 0x5f,
  ASID_CMD_REU_FILL_BUFFER_RECT = 0x60,
  // TODO: REU fetch to rectangle.
  ASID_CMD_UPDATE_REG = 0x6c,
  ASID_CMD_UPDATE2_REG = 0x6d,
};

#define ACK_VIC_IRQ asm("asl %[r]" : : [r] "i"((const uint16_t) & (VIC.irr)));
#define ACK_CIA_IRQ(X) asm("bit %[r]" : : [r] "i"((const uint16_t) & (X)));
#define ACK_CIA1_IRQ ACK_CIA_IRQ(CIA1.icr)
#define ACK_CIA2_IRQ ACK_CIA_IRQ(CIA2.icr)
const unsigned char sidregs = 25;

// unsigned char buf[256] = {};
unsigned char cmd = 0;
unsigned char reg = 0;
volatile unsigned char ch = 0;
volatile unsigned char nmi_in = 0;

unsigned char sidshadow[sizeof(regidmap)] = {};
unsigned char sidshadow2[sizeof(regidmap)] = {};

void noop() {}
void (*datahandler)(void) = &noop;
void (*stophandler)(void) = &noop;
void (*const asidstopcmdhandler[])(void);

volatile struct {
  unsigned char start;
  unsigned char manid;
  unsigned char cmd;
  unsigned char mask[4];
  unsigned char msb[4];
  unsigned char lsb[sizeof(regidmap)];
} asidupdate;

typedef volatile struct asidregupdate {
  unsigned char start;
  unsigned char manid;
  unsigned char cmd;
  unsigned char updates[sizeof(regidmap)];
} asidregupdatetype;

asidregupdatetype *const asidregupdatep = (asidregupdatetype *const)&asidupdate;

void asidstop() {
  (*asidstopcmdhandler[cmd])();
  CLOCK_ACK;
  datahandler = &noop;
  stophandler = &noop;
}

void setasidstop() { stophandler = &asidstop; }

void handle_loadupdate() { ((unsigned char *)&asidupdate)[reg++] = ch; }

#ifdef FULL
#include "vap-full.h"
#endif

#define SHADOWREG(b, shadow, i) b[i] = shadow[i];

#define SIDFROMSHADOW(b, shadow, i)                                            \
  SHADOWREG(b, shadow, 0 + i);                                                 \
  SHADOWREG(b, shadow, 1 + i);                                                 \
  SHADOWREG(b, shadow, 2 + i);                                                 \
  SHADOWREG(b, shadow, 3 + i);                                                 \
  SHADOWREG(b, shadow, 5 + i);                                                 \
  SHADOWREG(b, shadow, 6 + i);                                                 \
  SHADOWREG(b, shadow, 4 + i);

inline void sidfromshadow(unsigned char *shadow, volatile unsigned char *b) {
  SIDFROMSHADOW(b, shadow, 0);
  SIDFROMSHADOW(b, shadow, 7);
  SIDFROMSHADOW(b, shadow, 14);
  SHADOWREG(b, shadow, 21);
  SHADOWREG(b, shadow, 22);
  SHADOWREG(b, shadow, 23);
  SHADOWREG(b, shadow, 24);
}

inline void asidupdateregsid(unsigned char *shadow) {
  for (unsigned char j = 0; asidregupdatep->updates[j] != SYSEX_STOP; j += 2) {
    if (asidregupdatep->updates[j] & 0x40) {
      (asidregupdatep->updates[j + 1]) |= 0x80;
      (asidregupdatep->updates[j]) ^= 0x40;
    }
    shadow[asidregupdatep->updates[j]] = asidregupdatep->updates[j + 1];
  }
}

void asidupdatesid(unsigned char *shadow) {
  unsigned char lsbp = 0;
  unsigned char i = 0;
  unsigned char j = 0;
  unsigned char regid = 0;
#pragma unroll
  for (i = 0; i < 4; ++i, regid += 7) {
    unsigned char mask = asidupdate.mask[i];
    if (mask) {
      unsigned char msb = asidupdate.msb[i];
      for (j = 0; j < 7; ++j) {
        unsigned char bit = 1 << j;
        if (mask & bit) {
          unsigned char reg = regidmap[regid + j];
          unsigned char val = asidupdate.lsb[lsbp++];
          if (msb & bit) {
            val |= 0x80;
          }
          shadow[reg] = val;
        }
      }
    }
  }
}

void initsid(void) {
  unsigned char i = 0;
  for (i = 0; i < SIDREGSIZE; ++i) {
    sidshadow[i] = 0;
  }
  sidfromshadow(sidshadow, SIDBASE);
  sidfromshadow(sidshadow, SIDBASE2);
}

void updatesid() {
  asidupdatesid(sidshadow);
  sidfromshadow(sidshadow, SIDBASE);
}

void updatesid2() {
  asidupdatesid(sidshadow2);
  sidfromshadow(sidshadow2, SIDBASE2);
}

void updatebothsid() {
  updatesid();
  sidfromshadow(sidshadow, SIDBASE2);
}

#define UPDATEREGVAL(B)                                                        \
  if (reg & (1 << 6)) {                                                        \
    reg &= ((1 << 6) - 1);                                                     \
    ch |= 0x80;                                                                \
  }                                                                            \
  sidshadow[reg] = ch;                                                         \
  B[reg] = ch;

#define UPDATESHADOW(S, R, V, B)                                               \
  void R();                                                                    \
  void V() {                                                                   \
    UPDATEREGVAL(B);                                                           \
    datahandler = &R;                                                          \
  }                                                                            \
  void R() {                                                                   \
    reg = ch;                                                                  \
    datahandler = &V;                                                          \
  }                                                                            \
  void S() {                                                                   \
    datahandler = &R;                                                          \
    setasidstop();                                                             \
  }

UPDATESHADOW(start_handle_reg, handle_reg, handle_val, SIDBASE);
UPDATESHADOW(start_handle_reg2, handle_reg2, handle_val2, SIDBASE2);

void handle_single_reg();

void handle_single_val() {
  UPDATEREGVAL(SIDBASE);
  datahandler = &handle_single_reg;
}

void handle_single_reg() {
  reg = ch;
  datahandler = &handle_single_val;
}

void handle_single_reg2();

void handle_single_val2() {
  UPDATEREGVAL(SIDBASE2);
  datahandler = &handle_single_reg2;
}

void handle_single_reg2() {
  reg = ch;
  datahandler = &handle_single_val2;
}

void handlestart() {
  VIC.ctrl1 &= 0b11101111;
  setasidstop();
}

void handlestop() {
  VIC.ctrl1 |= 0b10000;
  initsid();
  setasidstop();
}

void handleupdate() {
  reg = 0;
  datahandler = &handle_loadupdate;
  setasidstop();
}

#ifdef FULL
#define HANDLE_FULL(X) X
#else
#define HANDLE_FULL(X) &noop
#endif

void (*const asidstartcmdhandler[])(void) = {
    &noop,                                // 0
    &noop,                                // 1
    &noop,                                // 2
    &noop,                                // 3
    &noop,                                // 4
    &noop,                                // 5
    &noop,                                // 6
    &noop,                                // 7
    &noop,                                // 8
    &noop,                                // 9
    &noop,                                // a
    &noop,                                // b
    &noop,                                // c
    &noop,                                // d
    &noop,                                // e
    &noop,                                // f
    &noop,                                // 10
    &noop,                                // 11
    &noop,                                // 12
    &noop,                                // 13
    &noop,                                // 14
    &noop,                                // 15
    &noop,                                // 16
    &noop,                                // 17
    &noop,                                // 18
    &noop,                                // 19
    &noop,                                // 1a
    &noop,                                // 1b
    &noop,                                // 1c
    &noop,                                // 1d
    &noop,                                // 1e
    &noop,                                // 1f
    &noop,                                // 20
    &noop,                                // 21
    &noop,                                // 22
    &noop,                                // 23
    &noop,                                // 24
    &noop,                                // 25
    &noop,                                // 26
    &noop,                                // 27
    &noop,                                // 28
    &noop,                                // 29
    &noop,                                // 2a
    &noop,                                // 2b
    &noop,                                // 2c
    &noop,                                // 2d
    &noop,                                // 2e
    &noop,                                // 2f
    &noop,                                // 30
    &noop,                                // 31
    &noop,                                // 32
    &noop,                                // 33
    &noop,                                // 34
    &noop,                                // 35
    &noop,                                // 36
    &noop,                                // 37
    &noop,                                // 38
    &noop,                                // 39
    &noop,                                // 3a
    &noop,                                // 3b
    &noop,                                // 3c
    &noop,                                // 3d
    &noop,                                // 3e
    &noop,                                // 3f
    &noop,                                // 40
    &noop,                                // 41
    &noop,                                // 42
    &noop,                                // 43
    &noop,                                // 44
    &noop,                                // 45
    &noop,                                // 46
    &noop,                                // 47
    &noop,                                // 48
    &noop,                                // 49
    &noop,                                // 4a
    &noop,                                // 4b
    &handlestart,                         // 4c ASID_CMD_START
    &handlestop,                          // 4d ASID_CMD_STOP
    &handleupdate,                        // 4e ASID_CMD_UPDATE
    &noop,                                // 4f
    &handleupdate,                        // 50 ASID_CMD_UPDATE2
    &handleupdate,                        // 51 ASID_CMD_UPDATE_BOTH
    HANDLE_FULL(&setasidstop),            // 52 ASID_CMD_RUN_BUFFER
    HANDLE_FULL(&start_handle_load),      // 53 ASID_CMD_LOAD_BUFFER
    HANDLE_FULL(&start_handle_addr),      // 54 ASID_CMD_ADDR_BUFFER
    HANDLE_FULL(&start_handle_load_rect), // 55 ASID_CMD_LOAD_RECT_BUFFER
    HANDLE_FULL(&start_handle_addr_rect), // 56 ASID_CMD_ADDR_RECT_BUFFER
    HANDLE_FULL(&start_handle_fill),      // 57 ASID_CMD_FILL_BUFFER
    HANDLE_FULL(&start_handle_fill),      // 58 ASID_CMD_FILL_RECT_BUFFER
    HANDLE_FULL(&start_handle_copy),      // 59 ASID_CMD_COPY_BUFFER
    HANDLE_FULL(&start_handle_copy),      // 5a ASID_CMD_COPY_RECT_BUFFER
    HANDLE_FULL(&start_handle_reu),       // 5b ASID_CMD_REU_STASH_BUFFER
    HANDLE_FULL(&start_handle_reu),       // 5c ASID_CMD_REU_FETCH_BUFFER
    HANDLE_FULL(&start_handle_reu_fill),  // 5d ASID_CMD_REU_FILL_BUFFER
    HANDLE_FULL(&start_handle_reu),       // 5e ASID_CMD_REU_STASH_BUFFER_RECT
    HANDLE_FULL(&start_handle_reu),       // 5f ASID_CMD_REU_FETCH_BUFFER_RECT
    HANDLE_FULL(&start_handle_reu_fill),  // 60 ASID_CMD_REU_FILL_BUFFER_RECT
    &noop,                                // 61
    &noop,                                // 62
    &noop,                                // 63
    &noop,                                // 64
    &noop,                                // 65
    &noop,                                // 66
    &noop,                                // 67
    &noop,                                // 68
    &noop,                                // 69
    &noop,                                // 6a
    &noop,                                // 6b
    &start_handle_reg,                    // 6c ASID_CMD_UPDATE_REG
    &start_handle_reg2,                   // 6d ASID_CMD_UPDATE2_REG
    &noop,                                // 6e
    &noop,                                // 6f
    &noop,                                // 70
    &noop,                                // 71
    &noop,                                // 72
    &noop,                                // 73
    &noop,                                // 74
    &noop,                                // 75
    &noop,                                // 76
    &noop,                                // 77
    &noop,                                // 78
    &noop,                                // 79
    &noop,                                // 7a
    &noop,                                // 7b
    &noop,                                // 7c
    &noop,                                // 7d
    &noop,                                // 7e
    &noop,                                // 7f
};

void (*const asidstopcmdhandler[])(void) = {
    &noop,                        // 0
    &noop,                        // 1
    &noop,                        // 2
    &noop,                        // 3
    &noop,                        // 4
    &noop,                        // 5
    &noop,                        // 6
    &noop,                        // 7
    &noop,                        // 8
    &noop,                        // 9
    &noop,                        // a
    &noop,                        // b
    &noop,                        // c
    &noop,                        // d
    &noop,                        // e
    &noop,                        // f
    &noop,                        // 10
    &noop,                        // 11
    &noop,                        // 12
    &noop,                        // 13
    &noop,                        // 14
    &noop,                        // 15
    &noop,                        // 16
    &noop,                        // 17
    &noop,                        // 18
    &noop,                        // 19
    &noop,                        // 1a
    &noop,                        // 1b
    &noop,                        // 1c
    &noop,                        // 1d
    &noop,                        // 1e
    &noop,                        // 1f
    &noop,                        // 20
    &noop,                        // 21
    &noop,                        // 22
    &noop,                        // 23
    &noop,                        // 24
    &noop,                        // 25
    &noop,                        // 26
    &noop,                        // 27
    &noop,                        // 28
    &noop,                        // 29
    &noop,                        // 2a
    &noop,                        // 2b
    &noop,                        // 2c
    &noop,                        // 2d
    &noop,                        // 2e
    &noop,                        // 2f
    &noop,                        // 30
    &noop,                        // 31
    &noop,                        // 32
    &noop,                        // 33
    &noop,                        // 34
    &noop,                        // 35
    &noop,                        // 36
    &noop,                        // 37
    &noop,                        // 38
    &noop,                        // 39
    &noop,                        // 3a
    &noop,                        // 3b
    &noop,                        // 3c
    &noop,                        // 3d
    &noop,                        // 3e
    &noop,                        // 3f
    &noop,                        // 40
    &noop,                        // 41
    &noop,                        // 42
    &noop,                        // 43
    &noop,                        // 44
    &noop,                        // 45
    &noop,                        // 46
    &noop,                        // 47
    &noop,                        // 48
    &noop,                        // 49
    &noop,                        // 4a
    &noop,                        // 4b
    &initsid,                     // 4c ASID_CMD_START
    &initsid,                     // 4d ASID_CMD_STOP
    &updatesid,                   // 4e ASID_CMD_UPDATE
    &noop,                        // 4f
    &updatesid2,                  // 50 ASID_CMD_UPDATE2
    &updatebothsid,               // 51 ASID_CMD_UPDATE_BOTH
    HANDLE_FULL(&indirect),       // 52 ASID_CMD_RUN_BUFFER
    &noop,                        // 53 ASID_CMD_LOAD_BUFFER
    &noop,                        // 54 ASID_CMD_ADDR_BUFFER
    &noop,                        // 55 ASID_CMD_LOAD_RECT_BUFFER
    HANDLE_FULL(&calcrect),       // 56 ASID_CMD_ADDR_RECT_BUFFER
    HANDLE_FULL(&fillbuffer),     // 57 ASID_CMD_FILL_BUFFER
    HANDLE_FULL(&fillrectbuffer), // 58 ASID_CMD_FILL_RECT_BUFFER
    HANDLE_FULL(&copybuffer),     // 59 ASID_CMD_COPY_BUFFER
    HANDLE_FULL(&copyrectbuffer), // 5a ASID_CMD_COPY_RECT_BUFFER
    HANDLE_FULL(&reustash),       // 5b ASID_CMD_REU_STASH_BUFFER
    HANDLE_FULL(&reufetch),       // 5c ASID_CMD_REU_FETCH_BUFFER
    HANDLE_FULL(&reufetch),       // 5d ASID_CMD_REU_FILL_BUFFER
    HANDLE_FULL(&reustashrect),   // 5e ASID_CMD_REU_STASH_BUFFER_RECT
    HANDLE_FULL(&reufetchrect),   // 5f ASID_CMD_REU_FETCH_BUFFER_RECT
    HANDLE_FULL(&reufetchrect),   // 60 ASID_CMD_REU_FILL_BUFFER_RECT
    &noop,                        // 61
    &noop,                        // 62
    &noop,                        // 63
    &noop,                        // 64
    &noop,                        // 65
    &noop,                        // 66
    &noop,                        // 67
    &noop,                        // 68
    &noop,                        // 69
    &noop,                        // 6a
    &noop,                        // 6b
    &noop,                        // 6c ASID_CMD_UPDATE_REG
    &noop,                        // 6d ASID_CMD_UPDATE2_REG
    &noop,                        // 6e
    &noop,                        // 6f
    &noop,                        // 70
    &noop,                        // 71
    &noop,                        // 72
    &noop,                        // 73
    &noop,                        // 74
    &noop,                        // 75
    &noop,                        // 76
    &noop,                        // 77
    &noop,                        // 78
    &noop,                        // 79
    &noop,                        // 7a
    &noop,                        // 7b
    &noop,                        // 7c
    &noop,                        // 7d
    &noop,                        // 7e
    &noop,                        // 7f
};

void __attribute__((interrupt)) _handle_nmi() {
  ACK_CIA2_IRQ;
  VIC.bordercolor = ++nmi_in;
}

void __attribute__((interrupt)) _handle_irq() {
  ACK_CIA1_IRQ;
}

void initvessel(void) {
  VOUT;
  VRESET;
  VIN;
  VOUT;
  VCMD(4); // config command
#ifdef POLL
  VW(4); // transparent mode
#else
  VW(1 + 4); // transparent mode with NMI
#endif
  VIN;
  VOUT;
}

void set_cia_timer(uint16_t v) {
  SEI();
  CIA1.icr = 0b01111111; // disable all CIA1 interrupts
  ACK_CIA1_IRQ;
  CIA1.cra &= 0b11111110; // disable timer A
  CIA1.icr = 0b10000001;  // enable timer A interrupt
  volatile uint16_t *timer = (uint16_t *)(&CIA1.ta_lo);
  *timer = v;
  CIA1.cra = 0b10000001; // start timer A
  CLI();
}

void init() {
  asm("jsr $e544"); // clear screen
  initsid();
#ifdef FULL
  initfull();
#endif
  const char *c = VAP_VERSION;
  while (*c) {
    putchar(*c++);
  }
  *(uint16_t *)COUNT = 0;
  *COUNT2 = 0;
  SEI();
  R6510 = 0b00000101; // disable kernal + basic, makes new handlers visible
  NMI_VECTOR = (volatile uint16_t) & _handle_nmi;
  IRQ_VECTOR = (volatile uint16_t) & _handle_irq;
  VIC.imr = 0; // disable VIC II interrupts.
  ACK_VIC_IRQ;
  CIA2.icr = 0b10010000; // set CIA2 interrupt source to FLAG2 only
  ACK_CIA2_IRQ;
  // set_cia_timer(19656);
  initvessel();
}

void handle_cmd() {
  cmd = ch;
  datahandler = &noop;
  stophandler = &noop;
  (*asidstartcmdhandler[cmd])();
}

void handle_manid() {
  if (ch == ASID_MANID) {
    datahandler = &handle_cmd;
  } else {
    datahandler = &noop;
  }
}

void midiloop(void) {
#ifndef POLL
  volatile unsigned char nmi_ack = 0;
#endif
  volatile unsigned char i = 0;
  volatile unsigned char c = 0;

  for (;;) {
#ifndef POLL
    if (nmi_in == nmi_ack) {
      continue;
    }
    nmi_ack = nmi_in;
#endif
    unsigned char y = 0;
    for (;;) {
      VIN;
      c = VR;
      if (c == 0) {
        VOUT;
        break;
      }
      unsigned char x = 0;
      while (c--) {
        ch = VR;
        ++x;
        ((unsigned char *)&asidupdate)[i++] = ch;
        if (ch == SYSEX_STOP) {
          i = 0;
          break;
        }
      }
      VOUT;
      (*(uint16_t *)COUNT) += x;
      switch (asidupdate.cmd) {
      case ASID_CMD_UPDATE:
        updatesid();
        ++y;
        break;
      case ASID_CMD_UPDATE_REG:
        asidupdateregsid(sidshadow);
        sidfromshadow(sidshadow, SIDBASE);
        ++y;
        break;
      case ASID_CMD_START:
        *(uint16_t *)COUNT = 0;
        *COUNT2 = 0;
        handlestart();
        break;
      case ASID_CMD_STOP:
        handlestop();
        break;
      }
      if (c == 0) {
        break;
      }
    }
    if (y > *COUNT2) {
      *COUNT2 = y;
    }
  }
}

int main(void) {
  init();
  midiloop();
  return 0;
}
