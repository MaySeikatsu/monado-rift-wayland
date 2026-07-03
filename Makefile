# Copyright 2025, monado-rift-wayland contributors.
# SPDX-License-Identifier: BSL-1.0
# Convenience wrapper around build.sh and the setup scripts.

.PHONY: all build install udev runtime steamvr clean

all: build

build:
	./build.sh

install:
	./build.sh --install

udev:
	./scripts/install-udev-rules.sh

runtime:
	./scripts/set-openxr-runtime.sh

steamvr:
	./scripts/register-steamvr-plugin.sh

clean:
	rm -rf build
