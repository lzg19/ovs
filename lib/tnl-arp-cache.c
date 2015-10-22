/*
 * Copyright (c) 2014, 2015 Nicira, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <config.h>
#include <inttypes.h>
#include <netinet/icmp6.h>
#include <stdlib.h>

#include "bitmap.h"
#include "cmap.h"
#include "coverage.h"
#include "dpif-netdev.h"
#include "dynamic-string.h"
#include "errno.h"
#include "flow.h"
#include "netdev.h"
#include "ovs-thread.h"
#include "packets.h"
#include "poll-loop.h"
#include "seq.h"
#include "socket-util.h"
#include "timeval.h"
#include "tnl-arp-cache.h"
#include "unaligned.h"
#include "unixctl.h"
#include "util.h"
#include "openvswitch/vlog.h"


/* In seconds */
#define ARP_ENTRY_DEFAULT_IDLE_TIME  (15 * 60)

struct tnl_arp_entry {
    struct cmap_node cmap_node;
    struct in6_addr ip;
    struct eth_addr mac;
    time_t expires;             /* Expiration time. */
    char br_name[IFNAMSIZ];
};

static struct cmap table;
static struct ovs_mutex mutex = OVS_MUTEX_INITIALIZER;

static uint32_t
tnl_arp_hash(const struct in6_addr *ip)
{
    return hash_bytes(ip->s6_addr, 16, 0);
}

static struct tnl_arp_entry *
tnl_arp_lookup__(const char br_name[IFNAMSIZ], const struct in6_addr *dst)
{
    struct tnl_arp_entry *arp;
    uint32_t hash;

    hash = tnl_arp_hash(dst);
    CMAP_FOR_EACH_WITH_HASH (arp, cmap_node, hash, &table) {
        if (ipv6_addr_equals(&arp->ip, dst) && !strcmp(arp->br_name, br_name)) {
            arp->expires = time_now() + ARP_ENTRY_DEFAULT_IDLE_TIME;
            return arp;
        }
    }
    return NULL;
}

int
tnl_arp_lookup(const char br_name[IFNAMSIZ], ovs_be32 dst,
               struct eth_addr *mac)
{
    struct tnl_arp_entry *arp;
    int res = ENOENT;
    struct in6_addr dst6;

    in6_addr_set_mapped_ipv4(&dst6, dst);

    arp = tnl_arp_lookup__(br_name, &dst6);
    if (arp) {
        *mac = arp->mac;
        res = 0;
    }

    return res;
}

int
tnl_nd_lookup(const char br_name[IFNAMSIZ], const struct in6_addr *dst,
              struct eth_addr *mac)
{
    struct tnl_arp_entry *arp;
    int res = ENOENT;

    arp = tnl_arp_lookup__(br_name, dst);
    if (arp) {
        *mac = arp->mac;
        res = 0;
    }
    return res;
}

static void
arp_entry_free(struct tnl_arp_entry *arp)
{
    free(arp);
}

static void
tnl_arp_delete(struct tnl_arp_entry *arp)
{
    uint32_t hash = tnl_arp_hash(&arp->ip);
    cmap_remove(&table, &arp->cmap_node, hash);
    ovsrcu_postpone(arp_entry_free, arp);
}

static void
tnl_arp_set__(const char name[IFNAMSIZ], const struct in6_addr *dst,
              const struct eth_addr mac)
{
    ovs_mutex_lock(&mutex);
    struct tnl_arp_entry *arp = tnl_arp_lookup__(name, dst);
    if (arp) {
        if (eth_addr_equals(arp->mac, mac)) {
            arp->expires = time_now() + ARP_ENTRY_DEFAULT_IDLE_TIME;
            ovs_mutex_unlock(&mutex);
            return;
        }
        tnl_arp_delete(arp);
        seq_change(tnl_conf_seq);
    }

    arp = xmalloc(sizeof *arp);

    arp->ip = *dst;
    arp->mac = mac;
    arp->expires = time_now() + ARP_ENTRY_DEFAULT_IDLE_TIME;
    ovs_strlcpy(arp->br_name, name, sizeof arp->br_name);
    cmap_insert(&table, &arp->cmap_node, tnl_arp_hash(&arp->ip));
    ovs_mutex_unlock(&mutex);
}

static void
tnl_arp_set(const char name[IFNAMSIZ], ovs_be32 dst,
            const struct eth_addr mac)
{
    struct in6_addr dst6;

    in6_addr_set_mapped_ipv4(&dst6, dst);
    tnl_arp_set__(name, &dst6, mac);
}

int
tnl_arp_snoop(const struct flow *flow, struct flow_wildcards *wc,
              const char name[IFNAMSIZ])
{
    if (flow->dl_type != htons(ETH_TYPE_ARP)) {
        return EINVAL;
    }

    /* Exact Match on all ARP flows. */
    memset(&wc->masks.nw_proto, 0xff, sizeof wc->masks.nw_proto);
    memset(&wc->masks.nw_src, 0xff, sizeof wc->masks.nw_src);
    memset(&wc->masks.arp_sha, 0xff, sizeof wc->masks.arp_sha);

    tnl_arp_set(name, flow->nw_src, flow->arp_sha);
    return 0;
}

int
tnl_nd_snoop(const struct flow *flow, struct flow_wildcards *wc,
              const char name[IFNAMSIZ])
{
    if (flow->dl_type != htons(ETH_TYPE_IPV6) ||
        flow->nw_proto != IPPROTO_ICMPV6 ||
        flow->tp_dst != htons(0) ||
        flow->tp_src != htons(ND_NEIGHBOR_ADVERT)) {
        return EINVAL;
    }

    memset(&wc->masks.ipv6_src, 0xff, sizeof wc->masks.ipv6_src);
    memset(&wc->masks.ipv6_dst, 0xff, sizeof wc->masks.ipv6_dst);
    memset(&wc->masks.nd_target, 0xff, sizeof wc->masks.nd_target);
    memset(&wc->masks.arp_tha, 0xff, sizeof wc->masks.arp_tha);

    tnl_arp_set__(name, &flow->nd_target, flow->arp_tha);
    return 0;
}

void
tnl_arp_cache_run(void)
{
    struct tnl_arp_entry *arp;
    bool changed = false;

    ovs_mutex_lock(&mutex);
    CMAP_FOR_EACH(arp, cmap_node, &table) {
        if (arp->expires <= time_now()) {
            tnl_arp_delete(arp);
            changed = true;
        }
    }
    ovs_mutex_unlock(&mutex);

    if (changed) {
        seq_change(tnl_conf_seq);
    }
}

static void
tnl_arp_cache_flush(struct unixctl_conn *conn, int argc OVS_UNUSED,
                    const char *argv[] OVS_UNUSED, void *aux OVS_UNUSED)
{
    struct tnl_arp_entry *arp;
    bool changed = false;

    ovs_mutex_lock(&mutex);
    CMAP_FOR_EACH(arp, cmap_node, &table) {
        tnl_arp_delete(arp);
        changed = true;
    }
    ovs_mutex_unlock(&mutex);
    if (changed) {
        seq_change(tnl_conf_seq);
    }
    unixctl_command_reply(conn, "OK");
}

static int
lookup_any(const char *host_name, struct in6_addr *address)
{
    if (addr_is_ipv6(host_name)) {
        return lookup_ipv6(host_name, address);
    } else {
        int r;
        struct in_addr ip;
        r = lookup_ip(host_name, &ip);
        if (r == 0) {
            in6_addr_set_mapped_ipv4(address, ip.s_addr);
        }
        return r;
    }
    return ENOENT;
}

static void
tnl_arp_cache_add(struct unixctl_conn *conn, int argc OVS_UNUSED,
                  const char *argv[], void *aux OVS_UNUSED)
{
    const char *br_name = argv[1];
    struct eth_addr mac;
    struct in6_addr ip6;

    if (lookup_any(argv[2], &ip6) != 0) {
        unixctl_command_reply_error(conn, "bad IP address");
        return;
    }

    if (!eth_addr_from_string(argv[3], &mac)) {
        unixctl_command_reply_error(conn, "bad MAC address");
        return;
    }

    tnl_arp_set__(br_name, &ip6, mac);
    unixctl_command_reply(conn, "OK");
}

static void
tnl_arp_cache_show(struct unixctl_conn *conn, int argc OVS_UNUSED,
                   const char *argv[] OVS_UNUSED, void *aux OVS_UNUSED)
{
    struct ds ds = DS_EMPTY_INITIALIZER;
    struct tnl_arp_entry *arp;

    ds_put_cstr(&ds, "IP                                            MAC                 Bridge\n");
    ds_put_cstr(&ds, "==========================================================================\n");
    ovs_mutex_lock(&mutex);
    CMAP_FOR_EACH(arp, cmap_node, &table) {
        int start_len, need_ws;

        start_len = ds.length;
        print_ipv6_mapped(&ds, &arp->ip);

        need_ws = INET6_ADDRSTRLEN - (ds.length - start_len);
        ds_put_char_multiple(&ds, ' ', need_ws);

        ds_put_format(&ds, ETH_ADDR_FMT"   %s\n",
                      ETH_ADDR_ARGS(arp->mac), arp->br_name);

    }
    ovs_mutex_unlock(&mutex);
    unixctl_command_reply(conn, ds_cstr(&ds));
    ds_destroy(&ds);
}

void
tnl_arp_cache_init(void)
{
    cmap_init(&table);

    unixctl_command_register("tnl/arp/show", "", 0, 0, tnl_arp_cache_show, NULL);
    unixctl_command_register("tnl/arp/set", "BRIDGE IP MAC", 3, 3, tnl_arp_cache_add, NULL);
    unixctl_command_register("tnl/arp/flush", "", 0, 0, tnl_arp_cache_flush, NULL);
}
