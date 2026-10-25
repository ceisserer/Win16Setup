# Win16Setup build (GNU make, Open Watcom 2.0).
#
#   make                  build/SETUP.EXE
#   make ICON=my.ico      ... with another icon (32x32, 16 colours)
#   make example          build/example/: the example product packaged
#                         (ZIP, 1.44 MB floppy image, SHA256SUMS)
#   make O=dir            build into dir
#
# Open Watcom: set WATCOM (default /opt/watcom).

WATCOM ?= /opt/watcom
export WATCOM
export PATH := $(WATCOM)/binl64:$(WATCOM)/binl:$(PATH)
export INCLUDE := $(WATCOM)/h:$(WATCOM)/h/win

O    ?= build
ICON ?= src/w16setup.ico
CFLAGS = -bt=windows -ml -2 -os -s -w4 -we -zq -zW

.PHONY: all example clean
all: $(O)/SETUP.EXE

$(O)/setup.obj: src/setup.c src/setup.h
	@mkdir -p $(O)
	wcc $(CFLAGS) -fo=$@ $<

# setup.rc includes "setup.ico": the copy in $(O)
$(O)/setup.ico: $(ICON)
	@mkdir -p $(O)
	cp $< $@

$(O)/setup.res: src/setup.rc src/setup.h $(O)/setup.ico
	wrc -q -r -bt=windows -i=$(O) -i=src -fo=$@ $<

$(O)/SETUP.EXE: $(O)/setup.obj $(O)/setup.res src/setup.lnk
	wlink @src/setup.lnk name $@ option map=$(O)/SETUP.MAP, resource=$(O)/setup.res file $(O)/setup.obj

# ---- example product ----
$(O)/hello.obj: example/hello.c
	@mkdir -p $(O)
	wcc $(CFLAGS) -fo=$@ $<

$(O)/HELLO.EXE: $(O)/hello.obj
	wlink system windows name $@ option stack=8k, heapsize=2k file $<

example: $(O)/SETUP.EXE $(O)/HELLO.EXE
	rm -rf $(O)/example/files
	mkdir -p $(O)/example/files/GREET
	cp $(O)/HELLO.EXE $(O)/example/files/
	sed "s/$$/\r/" example/README.TXT > $(O)/example/files/README.TXT
	cp example/GREET/*.INI $(O)/example/files/GREET/
	python3 tools/w16pack.py pack --inf example/SETUP.INF --files $(O)/example/files --setup $(O)/SETUP.EXE \
		--out $(O)/example --name hello-1.0 --label HELLO

clean:
	rm -rf $(O)
