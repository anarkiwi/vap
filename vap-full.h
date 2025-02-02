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

inline void initfull() {
  memset(&rectconfig, 0, sizeof(rectconfig));
  memset(&fillconfig, 0, sizeof(fillconfig));
  memset(&copyconfig, 0, sizeof(copyconfig));
}

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
