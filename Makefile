VERSION := $(shell git describe --tags)
CFLAGS := -Wall -O3 -fnonreentrant -flto -DVERSION=\"${VERSION}\"
SOURCES := vap.c vap-full.h regid.h vessel.h Makefile
PRGS := vap-poll.prg vap.prg vap-full.prg vap-full-poll.prg

all: vap.d64 $(PRGS)

vap.prg: $(SOURCES)
	/usr/local/llvm-mos/bin/mos-c64-clang $(CFLAGS) -o $@ $<

vap-poll.prg: $(SOURCES)
	/usr/local/llvm-mos/bin/mos-c64-clang $(CFLAGS) -DPOLL -o $@ $<

vap-full.prg: $(SOURCES)
	/usr/local/llvm-mos/bin/mos-c64-clang $(CFLAGS) -DFULL -o $@ $<

vap-full-poll.prg: $(SOURCES)
	/usr/local/llvm-mos/bin/mos-c64-clang $(CFLAGS) -DFULL -DPOLL -o $@ $<

vap.d64: $(PRGS)
	@echo version ${VERSION}
	c1541 -format diskname,id d64 vap.d64 -attach vap.d64 \
            -write vap.prg vap \
            -write vap-poll.prg vap-poll \
            -write vap-full.prg vap-full \
            -write vap-full-poll.prg vap-full-poll

clean:
	rm -f $(PRGS) vap.d64 vap.crt *.o *.elf

upload: all
	ncftpput -p "" -v c64 /Temp $(PRGS)

upload-crt: all
	./prg2crt.py vap.prg vap.crt
	ncftpput -p "" -Cv c64 vap.crt /Flash/carts/vap.crt
