LIBTXMS_SOURCE_DIR ?= $(abspath ../libtxms)

.PHONY: all test sanitize module clean
all:
	cmake -S . -B build -DLIBTXMS_SOURCE_DIR="$(LIBTXMS_SOURCE_DIR)" $(CMAKE_ARGS)
	cmake --build build -j
test: all
	ctest --test-dir build --output-on-failure
sanitize:
	cmake -S . -B build-sanitize -DTXMS_SANITIZE=ON
	cmake --build build-sanitize -j
	ctest --test-dir build-sanitize --output-on-failure
module: all
	test -n "$(KAMAILIO_SRC)"
	mkdir -p "$(KAMAILIO_SRC)/src/modules/txms"
	cp module/txms_mod.c module/Makefile "$(KAMAILIO_SRC)/src/modules/txms/"
	$(MAKE) -C "$(KAMAILIO_SRC)/src" modules modules=modules/txms TXMS_ROOT="$(CURDIR)" TXMS_BUILD="$(CURDIR)/build" LIBTXMS_ROOT="$(LIBTXMS_SOURCE_DIR)"
clean:
	rm -rf build build-sanitize
