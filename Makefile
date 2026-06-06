export GITHASH 		:= $(shell git rev-parse --short HEAD)
export VERSION 		:= 1.0.0
export API_VERSION 	:= 4
export WANT_FLAC 	:= 1
export WANT_MP3 	:= 1
export WANT_WAV 	:= 1

all: overlay nxExt module

clean:
	$(MAKE) -C sys-tune/nxExt clean
	$(MAKE) -C overlay clean
	$(MAKE) -C sys-tune clean
	-rm -r dist
	-rm streamfin-*-*.zip

overlay:
	$(MAKE) -C overlay

nxExt:
	$(MAKE) -C sys-tune/nxExt

module:
	$(MAKE) -C sys-tune

dist: all
	mkdir -p dist/switch/.overlays
	mkdir -p dist/atmosphere/contents/420000000046494E/flags
	touch dist/atmosphere/contents/420000000046494E/flags/boot2.flag
	cp sys-tune/sys-tune.nsp dist/atmosphere/contents/420000000046494E/exefs.nsp
	cp overlay/streamfin-overlay.ovl dist/switch/.overlays/
	cp sys-tune/toolbox.json dist/atmosphere/contents/420000000046494E/
	cd dist; zip -r streamfin-$(VERSION)-$(GITHASH).zip ./**/; cd ../;
	-hactool -t nso sys-tune/sys-tune.nso

.PHONY: all overlay module
