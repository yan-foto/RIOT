# name of your application
APPLICATION = ndn-lite-consumer

# If no BOARD is found in the environment, use this default:
BOARD ?= native

# This has to be the absolute path to the RIOT base directory:
RIOTBASE ?= $(CURDIR)/../..

USEPKG += ndn-lite

# Modules to include:
USEMODULE += netdev_default

# Comment this out to disable code in RIOT that does safety checking
# which is not needed in a production environment but helps in the
# development process:
DEVELHELP ?= 1

# Change this to 0 show compiler invocation lines by default:
QUIET ?= 1

# Define thread sizes
CFLAGS += -DTHREAD_STACKSIZE_MAIN=4096
CFLAGS += -DTHREAD_STACKSIZE_NDN_LITE=8192
CFLAGS += -DTHREAD_STACKSIZE_REGISTRATION=4096

include $(RIOTBASE)/Makefile.include
