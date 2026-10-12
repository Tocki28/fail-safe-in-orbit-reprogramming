# Ground console tests: a fake onboard computer on localhost, no board, no network.
#   make check   run test_console.py (needs src/controller/build/images from `make -C src/controller`)
PYTHON ?= $(HOME)/lodestar-prep/.venv/bin/python

check:
	$(PYTHON) test_console.py

.PHONY: check
