# Ground console tests: a fake onboard computer on localhost, no board, no network.
#   make check   build the host updater (the console's image check runs it, REQ-114), then run
#                test_console.py, test_ground_checks.py and test_send_frame.py
#                (needs src/controller/build/images from `make -C src/controller`)
PYTHON ?= python3

check:
	$(MAKE) -C ../../src/onboard-computer/updater
	$(PYTHON) test_console.py
	$(PYTHON) test_ground_checks.py
	$(PYTHON) test_send_frame.py

.PHONY: check
