ifeq ($(PIN_ROOT),)
  $(error ERROR: You must specify PIN_ROOT)
endif

CONFIG_ROOT := $(PIN_ROOT)/source/tools/Config

include $(CONFIG_ROOT)/makefile.config
include Makefile.rules
include $(TOOLS_ROOT)/Config/makefile.default.rules

