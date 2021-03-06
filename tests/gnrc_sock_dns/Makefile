include ../Makefile.tests_common

RIOTBASE ?= $(CURDIR)/../..

# chronos, hamilton and ruuvitag boards don't support ethos
BOARD_BLACKLIST := chronos hamilton ruuvitag

export TAP ?= tap0

USEMODULE += sock_dns
USEMODULE += gnrc_sock_udp
USEMODULE += gnrc_ipv6_default
USEMODULE += gnrc_ipv6_nib_dns
# use Ethernet as link-layer protocol
ifeq (native,$(BOARD))
  USEMODULE += netdev_tap

  TERMFLAGS ?= $(TAP)
else
  USEMODULE += stdio_ethos

  ETHOS_BAUDRATE ?= 115200
  CFLAGS += -DETHOS_BAUDRATE=$(ETHOS_BAUDRATE)
  TERMDEPS += ethos
  TERMPROG ?= sudo $(RIOTTOOLS)/ethos/ethos
  TERMFLAGS ?= $(TAP) $(PORT) $(ETHOS_BAUDRATE)
endif
USEMODULE += auto_init_gnrc_netif

USEMODULE += shell
USEMODULE += shell_commands

USEMODULE += posix_inet

LOW_MEMORY_BOARDS := nucleo-f334r8 msb-430 msb-430h

ifeq ($(BOARD),$(filter $(BOARD),$(LOW_MEMORY_BOARDS)))
    CFLAGS += -DGNRC_PKTBUF_SIZE=512 -DGNRC_NETIF_IPV6_ADDRS_NUMOF=2 \
              -DGNRC_NETIF_IPV6_GROUPS_NUMOF=2 -DGNRC_IPV6_NIB_NUMOF=1 \
              -DNRC_IPV6_NIB_OFFL_NUMOF=1
endif

# The test requires some setup and to be run as root
# So it cannot currently be run
TEST_ON_CI_BLACKLIST += all

.PHONY: ethos

ethos:
	$(Q)env -u CC -u CFLAGS make -C $(RIOTTOOLS)/ethos

include $(RIOTBASE)/Makefile.include
