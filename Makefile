VERSION := $(shell git describe --tags)
CFLAGS := -Wall -O3 -fnonreentrant -flto -DVERSION=\"${VERSION}\"
SOURCES := vap.c vap-full.h regid.h vessel.h Makefile
PRGS := vap-poll.prg vap.prg vap-full.prg vap-full-poll.prg

# Toolchain runs in containers; nothing is installed in /usr/local.
# Override MOS_CC/C1541 to use host installs instead.
MOS_IMAGE ?= ghcr.io/anarkiwi/docker-mos-llvm-sdk:v23.0.1
VICE_IMAGE ?= anarkiwi/asid-vice:3.10.0.0
DOCKER_RUN := docker run --rm -u $(shell id -u):$(shell id -g) \
    -v $(CURDIR):/work -w /work
MOS_CC ?= $(DOCKER_RUN) $(MOS_IMAGE) mos-c64-clang
# HOME must be writable by the calling uid or VICE logs errors creating its
# config, cache and state directories.
C1541 ?= $(DOCKER_RUN) -e HOME=/tmp --entrypoint c1541 $(VICE_IMAGE)

all: vap.d64 $(PRGS)

vap.crt: vap.prg
	./prg2crt.py vap.prg vap.crt

vap.prg: $(SOURCES)
	$(MOS_CC) $(CFLAGS) -o $@ $<

vap-poll.prg: $(SOURCES)
	$(MOS_CC) $(CFLAGS) -DPOLL -o $@ $<

vap-full.prg: $(SOURCES)
	$(MOS_CC) $(CFLAGS) -DFULL -o $@ $<

vap-full-poll.prg: $(SOURCES)
	$(MOS_CC) $(CFLAGS) -DFULL -DPOLL -o $@ $<

vap.d64: $(PRGS)
	@echo version ${VERSION}
	$(C1541) -format diskname,id d64 vap.d64 -attach vap.d64 \
            -write vap.prg vap \
            -write vap-poll.prg vap-poll \
            -write vap-full.prg vap-full \
            -write vap-full-poll.prg vap-full-poll

clean:
	rm -f $(PRGS) vap.d64 vap.crt *.o *.elf

upload: all
	ncftpput -p "" -v c64 /Temp $(PRGS)

upload-crt: all
	ncftpput -p "" -Cv c64 vap.crt /Flash/carts/vap.crt
