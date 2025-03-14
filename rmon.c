/* 
 * Route monitor
 * Copyright (c) 2025 Denis Kirjanov
 * 
 * This program is free software: you can redistribute it and/or modify  
 * it under the terms of the GNU General Public License as published by  
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but 
 * WITHOUT ANY WARRANTY; without even the implied warranty of 
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU 
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License 
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <netlink/netlink.h>
#include <netlink/cache.h>
#include <netlink/route/route.h>
#include <netlink/route/link.h>
#include <netlink/route/addr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check_routes_for_ifindex(struct nl_cache *route_cache, int ifindex)
{
    struct nl_object *obj;
    char dst_str[INET6_ADDRSTRLEN] = "unknown";
    char gw_str[INET6_ADDRSTRLEN] = "none";
    struct rtnl_route *route = NULL;
    struct nl_addr *gw = NULL;
    struct rtnl_nexthop *nh = NULL;
    struct nl_addr *dst = NULL;
    int route_ifindex = -1;
    int metric;

    for (obj = nl_cache_get_first(route_cache); obj; obj = nl_cache_get_next(obj)) {
        route = (struct rtnl_route *)obj;

        if (rtnl_route_get_family(route) != AF_INET)
            continue;

        dst = rtnl_route_get_dst(route);
        if (dst)
            nl_addr2str(dst, dst_str, sizeof(dst_str));

        nh = rtnl_route_nexthop_n(route, 0);
        if (nh) {
            route_ifindex = rtnl_route_nh_get_ifindex(nh);
            gw = rtnl_route_nh_get_gateway(nh);
            if (gw)
                nl_addr2str(gw, gw_str, sizeof(gw_str));
        }

        metric = rtnl_route_get_priority(route);

        if (route_ifindex == ifindex)
            printf("Route invalidated: destination: %s oif: %d gateway: %s metric: %d\n",
                   dst_str, route_ifindex, gw_str, metric);
    }
}

static void route_change(struct nl_cache *cache, struct nl_object *obj, int action, void *data)
{
    struct rtnl_route *route = (struct rtnl_route *)obj;
    char dst_str[INET6_ADDRSTRLEN] = "unknown";
    char gw_str[INET6_ADDRSTRLEN] = "none";
    struct rtnl_nexthop *nh = NULL;
    struct nl_addr *dst = NULL;
    struct nl_addr *gw = NULL;
    int ifindex = -1;
    int metric;

    if (rtnl_route_get_family(route) != AF_INET)
        return;

    dst = rtnl_route_get_dst(route);
    if (dst)
        nl_addr2str(dst, dst_str, sizeof(dst_str));

    nh = rtnl_route_nexthop_n(route, 0);
    if (nh) {
        ifindex = rtnl_route_nh_get_ifindex(nh);
        gw = rtnl_route_nh_get_gateway(nh);
        if (gw)
            nl_addr2str(gw, gw_str, sizeof(gw_str));
    }

    metric = rtnl_route_get_priority(route);

    switch (action) {
    case NL_ACT_NEW:
        printf("Route added: destination: %s oif: %d gateway: %s metric: %d\n",
               dst_str, ifindex, gw_str, metric);
        break;
    case NL_ACT_DEL:
        printf("Route deleted: destination: %s oif: %d gateway: %s metric: %d\n",
               dst_str, ifindex, gw_str, metric);
        break;
    case NL_ACT_CHANGE:
        printf("Route changed: destination: %s oif: %d gateway: %s metric: %d\n",
               dst_str, ifindex, gw_str, metric);
        break;
    }
}

static void link_change(struct nl_cache *cache, struct nl_object *obj, int action, void *data)
{
    struct nl_cache *route_cache = (struct nl_cache *)data;
    struct rtnl_link *link = (struct rtnl_link *)obj;
    int ifindex = rtnl_link_get_ifindex(link);

    switch (action) {
    case NL_ACT_NEW:
        printf("Link added, index: %d\n", ifindex);
        break;
    case NL_ACT_DEL:
        printf("Link deleted, index: %d\n", ifindex);
        check_routes_for_ifindex(route_cache, ifindex);
        break;
    case NL_ACT_CHANGE:
        printf("Link changed, index: %d\n", ifindex);
        break;
    }
}

static void addr_change(struct nl_cache *cache, struct nl_object *obj, int action, void *data)
{
    struct nl_cache *route_cache = (struct nl_cache *)data;
    struct rtnl_addr *addr = (struct rtnl_addr *)obj;
    struct nl_addr *local = NULL;
    char addr_str[INET6_ADDRSTRLEN] = {0};
    int ifindex;

    if (action == NL_ACT_DEL) {
        ifindex = rtnl_addr_get_ifindex(addr);
        local = rtnl_addr_get_local(addr);
        if (local) {
            nl_addr2str(local, addr_str, sizeof(addr_str));
            printf("Address deleted: %s on interface %d\n", addr_str, ifindex);
            check_routes_for_ifindex(route_cache, ifindex);
        }
    }
}

int main(int argc, char **argv)
{
    struct nl_cache_mngr *mngr;
    struct nl_cache *route_cache, *link_cache, *addr_cache;
    int err;

    err = nl_cache_mngr_alloc(NULL, NETLINK_ROUTE, NL_AUTO_PROVIDE, &mngr);
    if (err < 0) {
        fprintf(stderr, "Unable to allocate cache manager: %s\n", nl_geterror(err));
        return EXIT_FAILURE;
    }

    err = nl_cache_mngr_add(mngr, "route/route", route_change, NULL, &route_cache);
    if (err < 0) {
        fprintf(stderr, "Unable to add route cache: %s\n", nl_geterror(err));
        nl_cache_mngr_free(mngr);
        return EXIT_FAILURE;
    }
    printf("Subscribed to route changes\n");

    err = nl_cache_mngr_add(mngr, "route/link", link_change, route_cache, &link_cache);
    if (err < 0) {
        fprintf(stderr, "Unable to add link cache: %s\n", nl_geterror(err));
        nl_cache_mngr_free(mngr);
        return EXIT_FAILURE;
    }
    printf("Subscribed to link changes\n");

    err = nl_cache_mngr_add(mngr, "route/addr", addr_change, route_cache, &addr_cache);
    if (err < 0) {
        fprintf(stderr, "Unable to add addr cache: %s\n", nl_geterror(err));
        nl_cache_mngr_free(mngr);
        return EXIT_FAILURE;
    }
    printf("Subscribed to addr changes\n");

    while (1) {
        err = nl_cache_mngr_poll(mngr, -1);
        if (err < 0) {
            fprintf(stderr, "Polling failed: %s\n", nl_geterror(err));
            break;
        }
    }

    nl_cache_mngr_free(mngr);
    return EXIT_SUCCESS;
}
