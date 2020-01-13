#include <config.h>

#include "viralloc.h"
#include "virstring.h"
#include "virnetdev.h"
#include "virnetdevtap.h"
#include "internal.h"

#define VIR_FROM_THIS VIR_FROM_BHYVE

void virMacAddrGenerate(const unsigned char prefix[VIR_MAC_PREFIX_BUFLEN],
                        virMacAddrPtr addr)
{
    addr->addr[0] = prefix[0];
    addr->addr[1] = prefix[1];
    addr->addr[2] = prefix[2];
    addr->addr[3] = 0;
    addr->addr[4] = 0;
    addr->addr[5] = 0;
}

int virNetDevTapCreateInBridgePort(const char *brname G_GNUC_UNUSED,
                                   char **ifname,
                                   const virMacAddr *macaddr G_GNUC_UNUSED,
                                   const unsigned char *vmuuid G_GNUC_UNUSED,
                                   const char *tunpath G_GNUC_UNUSED,
                                   int *tapfd G_GNUC_UNUSED,
                                   size_t tapfdSize G_GNUC_UNUSED,
                                   const virNetDevVPortProfile *virtPortProfile G_GNUC_UNUSED,
                                   const virNetDevVlan *virtVlan G_GNUC_UNUSED,
                                   virNetDevCoalescePtr coalesce G_GNUC_UNUSED,
                                   unsigned int mtu G_GNUC_UNUSED,
                                   unsigned int *actualMTU G_GNUC_UNUSED,
                                   unsigned int fakeflags G_GNUC_UNUSED)
{
    VIR_FREE(*ifname);
    *ifname = g_strdup("vnet0");
    return 0;
}

char *virNetDevTapGetRealDeviceName(char *name G_GNUC_UNUSED)
{
    char *fakename;

    fakename = g_strdup("faketapdev");
    return fakename;
}

int virNetDevSetOnline(const char *ifname G_GNUC_UNUSED,
                       bool online G_GNUC_UNUSED)
{
    return 0;
}

int bind(int sockfd G_GNUC_UNUSED,
         const struct sockaddr *addr G_GNUC_UNUSED,
         socklen_t addrlen G_GNUC_UNUSED)
{
    return 0;
}
