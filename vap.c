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

#ifdef POLL
#define VAP_NAME "VAP-POLL"
#else
#define VAP_NAME "VAP"
#endif

const char VAP_VERSION[] = VAP_NAME VERSION;

#define RUN_BUFFER 0xc000
#define REU_COMMAND (*((volatile unsigned char *)0xdf01))
#define REU_CONTROL (*((volatile unsigned char *)0xdf0a))
#define REU_HOST_BASE ((volatile uint16_t *)0xdf02)
#define REU_ADDR_BASE ((volatile unsigned char *)0xdf04)
#define REU_TRANSFER_LEN ((volatile uint16_t *)0xdf07)
#define SCREENMEM ((volatile unsigned char *)0x0400)
#define SIDBASE ((volatile unsigned char *)0xd400)
#define SIDBASE2 ((volatile unsigned char *)0xd420)
#define R6510 (*(volatile unsigned char *)0x01)
#define SIDREGSIZE 28
#define UNFIXED_REU_ADDRESSES 0x0
#define FIX_REU_ADDRESS 0x40
#define FIX_HOST_ADDRESS 0x80
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

unsigned char buf[255] = {};
unsigned char cmd = 0;
unsigned char reg = 0;
unsigned char ch = 0;
unsigned char loadmsb = 0;
unsigned char loadmask = 0;
unsigned char col = 0;
unsigned char nmi_in = 0;
unsigned char updatep = 0;

volatile unsigned char *bufferaddr = (volatile unsigned char *)RUN_BUFFER;
volatile unsigned char *loadbuffer = 0;

unsigned char sidshadow[sizeof(regidmap)] = {};
unsigned char sidshadow2[sizeof(regidmap)] = {};

void noop() {}
void (*datahandler)(void) = &noop;
void (*stophandler)(void) = &noop;
void (*const asidstopcmdhandler[])(void);

volatile struct {
  unsigned char mask[4];
  unsigned char msb[4];
  unsigned char lsb[sizeof(regidmap)];
} asidupdate;

void asidstop() {
  (*asidstopcmdhandler[cmd])();
  CLOCK_ACK;
  datahandler = &noop;
  stophandler = &noop;
}

void setasidstop() { stophandler = &asidstop; }

void handle_loadupdate() { ((unsigned char *)&asidupdate)[updatep++] = ch; }

struct {
  unsigned char start; // number of positions to skip to new row (e.g. 40)
  unsigned char size;  // size of a row
  unsigned char inc;   // number of rows to increment
  uint16_t skip;       // number of addresses to skip per row
} rectconfig;

struct {
  unsigned char val;
  uint16_t count;
} fillconfig;

struct {
  unsigned char *from;
  uint16_t count;
} copyconfig;

inline void rect_skip() {
  if (!--col) {
    col = rectconfig.size;
    loadbuffer += rectconfig.skip;
  }
}

inline void rect_init() { col = rectconfig.size; }

inline void handle_load_ch(void (*const x)(void)) {
  if (loadmsb) {
    if (loadmask & 0x01) {
      ch |= 0x80;
    }
    loadmask >>= 1;
    *loadbuffer = ch;
    ++loadbuffer;
    --loadmsb;
    if (x) {
      x();
    }
  } else {
    loadmsb = 7;
    loadmask = ch;
  }
}

inline void handle_fill_buffer(void (*const x)(void), void (*const y)(void)) {
  uint16_t j = fillconfig.count;
  loadbuffer = bufferaddr;
  if (x) {
    x();
  }
  while (j--) {
    *(loadbuffer++) = fillconfig.val;
    if (y) {
      y();
    }
  }
}

inline void handle_copy_buffer(void (*const x)(void), void (*const y)(void)) {
  loadbuffer = bufferaddr;
  if (x) {
    x();
  }
  while (copyconfig.count--) {
    *(loadbuffer++) = *(copyconfig.from++);
    if (y) {
      y();
    }
  }
}

void reufetch() { REU_COMMAND = 0b10010001; }

void reustash() { REU_COMMAND = 0b10010000; }

inline void manage_reurect(void (*const x)(void)) {
  // transfer length must be a multiple of rectconfig.size
  uint16_t j = *REU_TRANSFER_LEN;
  while (j) {
    // Must reset transfer length on every transfer.
    *REU_TRANSFER_LEN = rectconfig.size;
    x();
    j -= rectconfig.size;
    *REU_HOST_BASE += rectconfig.skip;
  }
}

void reustashrect() { manage_reurect(reustash); }

void reufetchrect() { manage_reurect(reufetch); }

void indirect(void) { asm("jmp (bufferaddr)"); }

void fillbuffer() { handle_fill_buffer(NULL, NULL); }

void fillrectbuffer() { handle_fill_buffer(&rect_init, &rect_skip); }

void copybuffer() { handle_copy_buffer(NULL, NULL); }

void copyrectbuffer() { handle_copy_buffer(&rect_init, &rect_skip); }

void handle_load() { handle_load_ch(NULL); }

void handle_rect_load() { handle_load_ch(&rect_skip); }

void start_handle_load() {
  datahandler = &handle_load;
  loadbuffer = bufferaddr;
  setasidstop();
}

void start_handle_load_rect() {
  datahandler = &handle_load;
  loadbuffer = bufferaddr;
  datahandler = &handle_rect_load;
  rect_init();
  setasidstop();
}

void start_handle_addr() {
  datahandler = &handle_load;
  loadbuffer = (unsigned char *)&bufferaddr;
  setasidstop();
}

void calcrect() {
  unsigned char i = rectconfig.inc;
  rectconfig.skip = 0;
  while (i--) {
    rectconfig.skip += rectconfig.start - rectconfig.size;
  }
}

void start_handle_addr_rect() {
  datahandler = &handle_load;
  loadbuffer = (unsigned char *)&rectconfig;
  setasidstop();
}

inline void start_reu(uint8_t control) {
  datahandler = &handle_load;
  loadbuffer = REU_ADDR_BASE;
  REU_CONTROL = control;
  *(uint16_t *)REU_HOST_BASE = (uint16_t)bufferaddr;
  setasidstop();
}

void start_handle_reu() { start_reu(UNFIXED_REU_ADDRESSES); }

void start_handle_reu_fill() { start_reu(FIX_REU_ADDRESS); }

void start_handle_copy() {
  datahandler = &handle_load;
  loadbuffer = (unsigned char *)&copyconfig;
  setasidstop();
}

void start_handle_fill() {
  datahandler = &handle_load;
  loadbuffer = (unsigned char *)&fillconfig;
  setasidstop();
}

inline void sidfromshadow(unsigned char *shadow, volatile unsigned char *b) {
  unsigned char i = 0;
  for (i = 0; i < sidregs; ++i) {
    b[i] = shadow[i];
  }
}

void asidupdatesid(unsigned char *shadow) {
  unsigned char lsbp = 0;
  unsigned char i = 0;
  unsigned char j = 0;
  unsigned char regid = 0;
#pragma unroll
  for (i = 0; i < 4; ++i) {
    unsigned char mask = asidupdate.mask[i];
    unsigned char msb = asidupdate.msb[i];
    for (j = 0; j < 7; ++j, ++regid) {
      unsigned char bit = 1 << j;
      if (mask & bit) {
        unsigned char reg = regidmap[regid];
        unsigned char val = asidupdate.lsb[lsbp++];
        if (msb & bit) {
          val |= 0x80;
        }
        shadow[reg] = val;
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
  setasidstop();
}

void handleupdate() {
  updatep = 0;
  datahandler = &handle_loadupdate;
  setasidstop();
}

void (*const asidstartcmdhandler[])(void) = {
    &noop,                   // 0
    &noop,                   // 1
    &noop,                   // 2
    &noop,                   // 3
    &noop,                   // 4
    &noop,                   // 5
    &noop,                   // 6
    &noop,                   // 7
    &noop,                   // 8
    &noop,                   // 9
    &noop,                   // a
    &noop,                   // b
    &noop,                   // c
    &noop,                   // d
    &noop,                   // e
    &noop,                   // f
    &noop,                   // 10
    &noop,                   // 11
    &noop,                   // 12
    &noop,                   // 13
    &noop,                   // 14
    &noop,                   // 15
    &noop,                   // 16
    &noop,                   // 17
    &noop,                   // 18
    &noop,                   // 19
    &noop,                   // 1a
    &noop,                   // 1b
    &noop,                   // 1c
    &noop,                   // 1d
    &noop,                   // 1e
    &noop,                   // 1f
    &noop,                   // 20
    &noop,                   // 21
    &noop,                   // 22
    &noop,                   // 23
    &noop,                   // 24
    &noop,                   // 25
    &noop,                   // 26
    &noop,                   // 27
    &noop,                   // 28
    &noop,                   // 29
    &noop,                   // 2a
    &noop,                   // 2b
    &noop,                   // 2c
    &noop,                   // 2d
    &noop,                   // 2e
    &noop,                   // 2f
    &noop,                   // 30
    &noop,                   // 31
    &noop,                   // 32
    &noop,                   // 33
    &noop,                   // 34
    &noop,                   // 35
    &noop,                   // 36
    &noop,                   // 37
    &noop,                   // 38
    &noop,                   // 39
    &noop,                   // 3a
    &noop,                   // 3b
    &noop,                   // 3c
    &noop,                   // 3d
    &noop,                   // 3e
    &noop,                   // 3f
    &noop,                   // 40
    &noop,                   // 41
    &noop,                   // 42
    &noop,                   // 43
    &noop,                   // 44
    &noop,                   // 45
    &noop,                   // 46
    &noop,                   // 47
    &noop,                   // 48
    &noop,                   // 49
    &noop,                   // 4a
    &noop,                   // 4b
    &handlestart,            // 4c ASID_CMD_START
    &handlestop,             // 4d ASID_CMD_STOP
    &handleupdate,           // 4e ASID_CMD_UPDATE
    &noop,                   // 4f
    &handleupdate,           // 50 ASID_CMD_UPDATE2
    &handleupdate,           // 51 ASID_CMD_UPDATE_BOTH
    &setasidstop,            // 52 ASID_CMD_RUN_BUFFER
    &start_handle_load,      // 53 ASID_CMD_LOAD_BUFFER
    &start_handle_addr,      // 54 ASID_CMD_ADDR_BUFFER
    &start_handle_load_rect, // 55 ASID_CMD_LOAD_RECT_BUFFER
    &start_handle_addr_rect, // 56 ASID_CMD_ADDR_RECT_BUFFER
    &start_handle_fill,      // 57 ASID_CMD_FILL_BUFFER
    &start_handle_fill,      // 58 ASID_CMD_FILL_RECT_BUFFER
    &start_handle_copy,      // 59 ASID_CMD_COPY_BUFFER
    &start_handle_copy,      // 5a ASID_CMD_COPY_RECT_BUFFER
    &start_handle_reu,       // 5b ASID_CMD_REU_STASH_BUFFER
    &start_handle_reu,       // 5c ASID_CMD_REU_FETCH_BUFFER
    &start_handle_reu_fill,  // 5d ASID_CMD_REU_FILL_BUFFER
    &start_handle_reu,       // 5e ASID_CMD_REU_STASH_BUFFER_RECT
    &start_handle_reu,       // 5f ASID_CMD_REU_FETCH_BUFFER_RECT
    &start_handle_reu_fill,  // 60 ASID_CMD_REU_FILL_BUFFER_RECT
    &noop,                   // 61
    &noop,                   // 62
    &noop,                   // 63
    &noop,                   // 64
    &noop,                   // 65
    &noop,                   // 66
    &noop,                   // 67
    &noop,                   // 68
    &noop,                   // 69
    &noop,                   // 6a
    &noop,                   // 6b
    &start_handle_reg,       // 6c ASID_CMD_UPDATE_REG
    &start_handle_reg2,      // 6d ASID_CMD_UPDATE2_REG
    &noop,                   // 6e
    &noop,                   // 6f
    &noop,                   // 70
    &noop,                   // 71
    &noop,                   // 72
    &noop,                   // 73
    &noop,                   // 74
    &noop,                   // 75
    &noop,                   // 76
    &noop,                   // 77
    &noop,                   // 78
    &noop,                   // 79
    &noop,                   // 7a
    &noop,                   // 7b
    &noop,                   // 7c
    &noop,                   // 7d
    &noop,                   // 7e
    &noop,                   // 7f
};

void (*const asidstopcmdhandler[])(void) = {
    &noop,           // 0
    &noop,           // 1
    &noop,           // 2
    &noop,           // 3
    &noop,           // 4
    &noop,           // 5
    &noop,           // 6
    &noop,           // 7
    &noop,           // 8
    &noop,           // 9
    &noop,           // a
    &noop,           // b
    &noop,           // c
    &noop,           // d
    &noop,           // e
    &noop,           // f
    &noop,           // 10
    &noop,           // 11
    &noop,           // 12
    &noop,           // 13
    &noop,           // 14
    &noop,           // 15
    &noop,           // 16
    &noop,           // 17
    &noop,           // 18
    &noop,           // 19
    &noop,           // 1a
    &noop,           // 1b
    &noop,           // 1c
    &noop,           // 1d
    &noop,           // 1e
    &noop,           // 1f
    &noop,           // 20
    &noop,           // 21
    &noop,           // 22
    &noop,           // 23
    &noop,           // 24
    &noop,           // 25
    &noop,           // 26
    &noop,           // 27
    &noop,           // 28
    &noop,           // 29
    &noop,           // 2a
    &noop,           // 2b
    &noop,           // 2c
    &noop,           // 2d
    &noop,           // 2e
    &noop,           // 2f
    &noop,           // 30
    &noop,           // 31
    &noop,           // 32
    &noop,           // 33
    &noop,           // 34
    &noop,           // 35
    &noop,           // 36
    &noop,           // 37
    &noop,           // 38
    &noop,           // 39
    &noop,           // 3a
    &noop,           // 3b
    &noop,           // 3c
    &noop,           // 3d
    &noop,           // 3e
    &noop,           // 3f
    &noop,           // 40
    &noop,           // 41
    &noop,           // 42
    &noop,           // 43
    &noop,           // 44
    &noop,           // 45
    &noop,           // 46
    &noop,           // 47
    &noop,           // 48
    &noop,           // 49
    &noop,           // 4a
    &noop,           // 4b
    &initsid,        // 4c ASID_CMD_START
    &initsid,        // 4d ASID_CMD_STOP
    &updatesid,      // 4e ASID_CMD_UPDATE
    &noop,           // 4f
    &updatesid2,     // 50 ASID_CMD_UPDATE2
    &updatebothsid,  // 51 ASID_CMD_UPDATE_BOTH
    &indirect,       // 52 ASID_CMD_RUN_BUFFER
    &noop,           // 53 ASID_CMD_LOAD_BUFFER
    &noop,           // 54 ASID_CMD_ADDR_BUFFER
    &noop,           // 55 ASID_CMD_LOAD_RECT_BUFFER
    &calcrect,       // 56 ASID_CMD_ADDR_RECT_BUFFER
    &fillbuffer,     // 57 ASID_CMD_FILL_BUFFER
    &fillrectbuffer, // 58 ASID_CMD_FILL_RECT_BUFFER
    &copybuffer,     // 59 ASID_CMD_COPY_BUFFER
    &copyrectbuffer, // 5a ASID_CMD_COPY_RECT_BUFFER
    &reustash,       // 5b ASID_CMD_REU_STASH_BUFFER
    &reufetch,       // 5c ASID_CMD_REU_FETCH_BUFFER
    &reufetch,       // 5d ASID_CMD_REU_FILL_BUFFER
    &reustashrect,   // 5e ASID_CMD_REU_STASH_BUFFER_RECT
    &reufetchrect,   // 5f ASID_CMD_REU_FETCH_BUFFER_RECT
    &reufetchrect,   // 60 ASID_CMD_REU_FILL_BUFFER_RECT
    &noop,           // 61
    &noop,           // 62
    &noop,           // 63
    &noop,           // 64
    &noop,           // 65
    &noop,           // 66
    &noop,           // 67
    &noop,           // 68
    &noop,           // 69
    &noop,           // 6a
    &noop,           // 6b
    &noop,           // 6c ASID_CMD_UPDATE_REG
    &noop,           // 6d ASID_CMD_UPDATE2_REG
    &noop,           // 6e
    &noop,           // 6f
    &noop,           // 70
    &noop,           // 71
    &noop,           // 72
    &noop,           // 73
    &noop,           // 74
    &noop,           // 75
    &noop,           // 76
    &noop,           // 77
    &noop,           // 78
    &noop,           // 79
    &noop,           // 7a
    &noop,           // 7b
    &noop,           // 7c
    &noop,           // 7d
    &noop,           // 7e
    &noop,           // 7f
};

void __attribute__((interrupt)) _handle_nmi() {
  ACK_CIA2_IRQ;
  VIC.bordercolor = ++nmi_in;
}

void __attribute__((interrupt)) _handle_irq() { ACK_CIA1_IRQ; }

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

void init(void) {
  asm("jsr $e544"); // clear screen
  initsid();
  memset(&rectconfig, 0, sizeof(rectconfig));
  memset(&fillconfig, 0, sizeof(fillconfig));
  memset(&copyconfig, 0, sizeof(copyconfig));
  const char *c = VAP_VERSION;
  while (*c) {
    putchar(*c++);
  }
  SEI();
  R6510 = 0b00000101; // disable kernal + basic, makes new handlers visible
  NMI_VECTOR = (volatile uint16_t) & _handle_nmi;
  IRQ_VECTOR = (volatile uint16_t) & _handle_irq;
  VIC.imr = 0; // disable VIC II interrupts.
  ACK_VIC_IRQ;
  CIA2.icr = 0b10010000; // set CIA2 interrupt source to FLAG2 only
  ACK_CIA2_IRQ;
  set_cia_timer(19656);
  initvessel();
}

void handle_cmd() {
  cmd = ch;
  loadmsb = 0;
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
  unsigned char i = 0;
  unsigned char c = 0;

  for (;;) {
#ifndef POLL
    if (nmi_in == nmi_ack) {
      continue;
    }
#endif
    VIN;
    c = VR;
    for (i = c; i; --i) {
      buf[i] = VR;
    }
#ifndef POLL
    nmi_ack = nmi_in;
#endif
    VOUT;
    for (i = c; i; --i) {
      ch = buf[i];
      if (ch & 0x80) {
        switch (ch) {
        case SYSEX_STOP:
          (*stophandler)();
          break;
        case SYSEX_START:
          datahandler = &handle_manid;
          break;
        case NOTEOFF16:
          datahandler = &handle_single_reg;
          break;
        case NOTEOFF15:
          datahandler = &handle_single_reg2;
          break;
        default:
          break;
        }
      } else {
        datahandler();
      }
    }
  }
}

int main(void) {
  init();
  midiloop();
  return 0;
}
