# Portable Riviera Engine Core integration. Set CORE_ROOT before including.
CORE_ROOT ?= $(dir $(lastword $(MAKEFILE_LIST)))
CORE_ROOT := $(abspath $(CORE_ROOT))
CORE_SOURCES := $(shell find $(CORE_ROOT)/src -name '*.cpp' -type f | sort)
CORE_HEADERS := $(shell find $(CORE_ROOT)/include -type f \( -name '*.hpp' -o -name '*.h' \) | sort)
CORE_XENIUM_HEADERS := $(shell find $(CORE_ROOT)/third_party/xenium -type f \( -name '*.hpp' -o -name '*.h' \) | sort)
CORE_INCLUDE_FLAGS := -I$(CORE_ROOT)/include -isystem $(CORE_ROOT)/third_party/xenium
CORE_THIRD_PARTY_NOTICES := $(CORE_ROOT)/third_party/NOTICES.txt
