# name of your application
APPLICATION = ndn-lite-consumer

# If no BOARD is found in the environment, use this default:
BOARD ?= native

BOARD_INSUFFICIENT_MEMORY := airfy-beacon chronos msb-430 msb-430h nrf51dongle \
                          nrf6310 nucleo-f103 nucleo-f334 pca10000 pca10005 spark-core \
                          stm32f0discovery telosb weio wsn430-v1_3b wsn430-v1_4 \
                          yunjia-nrf51822 z1

# This has to be the absolute path to the RIOT base directory:
RIOTBASE ?= $(CURDIR)/../..

USEPKG += ndn-lite

# Modules to include:
USEMODULE += gnrc_netdev_default
USEMODULE += auto_init_gnrc_netif

# Comment this out to disable code in RIOT that does safety checking
# which is not needed in a production environment but helps in the
# development process:
DEVELHELP ?= 1

# Change this to 0 show compiler invocation lines by default:
QUIET ?= 1

# Define thread sizes
CFLAGS += -DTHREAD_STACKSIZE_MAIN=2048
CFLAGS += -DTHREAD_STACKSIZE_NDN_LITE=8192
CFLAGS += -DTHREAD_STACKSIZE_REGISTRATION=4096

include $(RIOTBASE)/Makefile.include