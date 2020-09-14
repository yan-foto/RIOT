PKG_NAME=ndn-lite
PKG_URL=https://github.com/named-data-iot/ndn-lite
# There is no release management for ndn-lite so the latest
# known working reference is used here.
PKG_VERSION=d60a114a4c4e8422577ab8ae3817f79eed948254
PKG_LICENSE=LGPLv3

TDIR = $(RIOTPKG)/$(PKG_NAME)
PDIR = $(PKG_SOURCE_DIR)

include $(RIOTBASE)/pkg/pkg.mk

NDN_MODULES =  \
	ndn_lite_util \
	ndn_lite_encode \
	ndn_lite_forwarder \
	ndn_lite_security \
	ndn_lite_security_default-backend \
	ndn_lite_security_seclib_micro-ecc \
	ndn_lite_security_seclib_tinycrypt \
    #

CONTRIB_MODULES = \
	ndn_lite_contrib_l2 \
	ndn_lite_contrib_netface \
	ndn_lite_contrib_security \
	ndn_lite_contrib_util \
	# ndn_lite_contrib_udp \
	#

.PHONY: ndn_lite_% ndn_lite_security_% ndn_lite_contrib_%

all: $(NDN_MODULES) $(CONTRIB_MODULES)

ndn_lite_%:
	MODULE=$@ "$(MAKE)" -C $(PDIR)/$* -f $(TDIR)/Makefile.ndn-lite

ndn_lite_security_%:
	MODULE=$@ "$(MAKE)" -C $(PDIR)/security/$* -f $(TDIR)/Makefile.ndn-lite

ndn_lite_security_seclib_%:
	MODULE=$@ "$(MAKE)" -C $(PDIR)/security/default-backend/sec-lib/$* -f $(TDIR)/Makefile.ndn-lite

ndn_lite_contrib_%:
	MODULE=$@ "$(MAKE)" -C $(TDIR)/contrib/$* -f $(TDIR)/Makefile.ndn-lite
