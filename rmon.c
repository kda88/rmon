#include <netlink/netlink.h>
#include <netlink/route/route.h>
#include <netlink/route/link.h>
#include <netlink/route/addr.h>
#include <netlink/msg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char destination[INET6_ADDRSTRLEN];
    int ifindex;
    char gateway[INET6_ADDRSTRLEN];
    int metric;
} RouteInfo;

typedef struct HashNode {
    RouteInfo route;
    struct HashNode *next;
} HashNode;

typedef struct {
    HashNode **buckets;
    size_t size;
} HashTable;

HashTable *hash_table_init(size_t size)
{
    HashTable *ht;
    
    ht = malloc(sizeof(HashTable));
    if (!ht)
	    return NULL;
    ht->size = size;
    ht->buckets = calloc(size, sizeof(HashNode *));
    if (!ht->buckets) {
        free(ht);
        return NULL;
    }
    return ht;
}

unsigned int hash_function(const char *destination, int ifindex, size_t size)
{
    unsigned int hash = 0;

    for (int i = 0; destination[i]; i++)
        hash = hash * 31 + destination[i];
    hash += ifindex;

    return hash % size;
}

void hash_table_insert(HashTable *ht, RouteInfo *route)
{
    unsigned int index = hash_function(route->destination, route->ifindex, ht->size);
    HashNode *node = malloc(sizeof(HashNode));
    if (!node)
	    return;
    node->route = *route;
    node->next = ht->buckets[index];
    ht->buckets[index] = node;
}

void hash_table_remove(HashTable *ht, const char *destination, int ifindex)
{
    unsigned int index = hash_function(destination, ifindex, ht->size);
    HashNode *current = ht->buckets[index];
    HashNode *prev = NULL;

    while (current) {
        if (strcmp(current->route.destination, destination) == 0 && 
            current->route.ifindex == ifindex) {
            if (prev)
                prev->next = current->next;
            else
                ht->buckets[index] = current->next;
            free(current);
            return;
        }
        prev = current;
        current = current->next;
    }
}

void hash_table_find_by_ifindex(HashTable *ht, int ifindex, void (*callback)(RouteInfo *))
{
    for (size_t i = 0; i < ht->size; i++) {
        HashNode *current = ht->buckets[i];
        while (current) {
            if (current->route.ifindex == ifindex) {
                callback(&current->route);
            }
            current = current->next;
        }
    }
}

void hash_table_free(HashTable *ht)
{
    if (!ht)
	    return;

    for (size_t i = 0; i < ht->size; i++) {
        HashNode *current = ht->buckets[i];

        while (current) {
            HashNode *next = current->next;
            free(current);
            current = next;
        }
    }

    free(ht->buckets);
    free(ht);
}

static void print_invalidated_route(RouteInfo *route)
{
    printf("Route invalidated: destination: %s oif: %d gateway: %s metric: %d\n",
           route->destination, route->ifindex, route->gateway, route->metric);
}

static int route_callback(struct nl_msg *msg, void *arg)
{
    struct rtnl_route *route;
    HashTable *ht = (HashTable *)arg;
    struct nlmsghdr *nlh = nlmsg_hdr(msg);
    struct nl_addr *dst;
    char dst_str[INET6_ADDRSTRLEN] = "unknown";
    char gw_str[INET6_ADDRSTRLEN] = "none";
    int err;
    int ifindex = -1;
    int metric;
    struct rtnl_nexthop *nh;

    if (nlh->nlmsg_type != RTM_NEWROUTE && nlh->nlmsg_type != RTM_DELROUTE) {
        return NL_SKIP;
    }

    route = rtnl_route_alloc();
    if (!route) {
        fprintf(stderr, "Failed to allocate route object\n");
        return NL_OK;
    }

    err = rtnl_route_parse(nlh, &route);
    if (err < 0) {
        fprintf(stderr, "Failed to parse route: %s\n", nl_geterror(err));
        rtnl_route_put(route);
        return NL_OK;
    }

    dst = rtnl_route_get_dst(route);
    if (dst)
        nl_addr2str(dst, dst_str, sizeof(dst_str));

    nh = rtnl_route_nexthop_n(route, 0);
    if (nh) {
        ifindex = rtnl_route_nh_get_ifindex(nh);
        struct nl_addr *gw = rtnl_route_nh_get_gateway(nh);
        if (gw)
            nl_addr2str(gw, gw_str, sizeof(gw_str));
    }

    metric = rtnl_route_get_priority(route);

    if (nlh->nlmsg_type == RTM_NEWROUTE) {
        RouteInfo new_route = {0};
        strncpy(new_route.destination, dst_str, sizeof(new_route.destination) - 1);
        new_route.destination[sizeof(new_route.destination) - 1] = '\0';
        new_route.ifindex = ifindex;
        strncpy(new_route.gateway, gw_str, sizeof(new_route.gateway) - 1);
        new_route.gateway[sizeof(new_route.gateway) - 1] = '\0';
        new_route.metric = metric;
        hash_table_insert(ht, &new_route);
        printf("Route added: destination: %s oif: %d gateway: %s metric: %d\n",
               dst_str, ifindex, gw_str, metric);
    } else if (nlh->nlmsg_type == RTM_DELROUTE) {
        printf("Route deleted: destination: %s oif: %d gateway: %s metric: %d\n",
               dst_str, ifindex, gw_str, metric);
        hash_table_remove(ht, dst_str, ifindex);
    }

    rtnl_route_put(route);
    return NL_OK;
}

static int link_callback(struct nl_msg *msg, void *arg)
{
    HashTable *ht = (HashTable *)arg;
    struct nlmsghdr *nlh = nlmsg_hdr(msg);
    struct ifinfomsg *ifi;
    int ifindex;

    if (nlh->nlmsg_type != RTM_NEWLINK && nlh->nlmsg_type != RTM_DELLINK) {
        return NL_SKIP;
    }

    ifi = nlmsg_data(nlh);
    if (!ifi) {
        fprintf(stderr, "Failed to get link data\n");
        return NL_OK;
    }

    ifindex = ifi->ifi_index;

    if (nlh->nlmsg_type == RTM_NEWLINK)
        printf("Link added, index: %d\n", ifindex);
    else if (nlh->nlmsg_type == RTM_DELLINK) {
        printf("Link deleted, index: %d\n", ifindex);
        hash_table_find_by_ifindex(ht, ifindex, print_invalidated_route);
    }

    return NL_OK;
}

static int addr_callback(struct nl_msg *msg, void *arg)
{
    HashTable *ht = (HashTable *)arg;
    struct nlmsghdr *nlh = nlmsg_hdr(msg);
    struct ifaddrmsg *ifa;
    int ifindex;
    struct nl_addr *addr = NULL;
    struct rtattr *rta = NULL;
    int len;

    if (nlh->nlmsg_type != RTM_DELADDR)
        return NL_SKIP;

    ifa = nlmsg_data(nlh);
    if (!ifa) {
        fprintf(stderr, "Failed to get address data\n");
        return NL_OK;
    }

    ifindex = ifa->ifa_index;
    len = NLMSG_PAYLOAD(nlh, sizeof(*ifa));

    if (len > 0) {
        rta = (struct rtattr *)(((char *)ifa) + NLMSG_ALIGN(sizeof(*ifa)));
        while (RTA_OK(rta, len)) {
            if (rta->rta_type == IFA_LOCAL || rta->rta_type == IFA_ADDRESS) {
                addr = nl_addr_build(ifa->ifa_family, RTA_DATA(rta), RTA_PAYLOAD(rta));
                break;
            }
            rta = RTA_NEXT(rta, len);
        }
    }

    if (addr) {
        char addr_str[INET6_ADDRSTRLEN] = {0};

        nl_addr2str(addr, addr_str, sizeof(addr_str));
        printf("Address deleted: %s on interface %d\n", addr_str, ifindex);
        hash_table_find_by_ifindex(ht, ifindex, print_invalidated_route);
        nl_addr_put(addr);
    }

    return NL_OK;
}

#define HASH_SIZE 128

int main(int argc, char **argv)
{
    HashTable *ht;
    struct nl_sock *route_sock;
    struct nl_sock *link_sock;
    struct nl_sock *addr_sock;

    ht = hash_table_init(HASH_SIZE);
    if (!ht) {
        fprintf(stderr, "Can't init a hash table\n");
        return EXIT_FAILURE;
    }

    route_sock = nl_socket_alloc();
    if (!route_sock) {
        fprintf(stderr, "Unable to allocate a route socket\n");
        hash_table_free(ht);
        return EXIT_FAILURE;
    }
    nl_socket_disable_seq_check(route_sock);

    link_sock = nl_socket_alloc();
    if (!link_sock) {
        fprintf(stderr, "Unable to allocate a link socket\n");
        nl_socket_free(route_sock);
        hash_table_free(ht);
        return EXIT_FAILURE;
    }
    nl_socket_disable_seq_check(link_sock);

    addr_sock = nl_socket_alloc();
    if (!addr_sock) {
        fprintf(stderr, "Unable to allocate an address socket\n");
        nl_socket_free(route_sock);
        nl_socket_free(link_sock);
        hash_table_free(ht);
        return EXIT_FAILURE;
    }
    nl_socket_disable_seq_check(addr_sock);

    if (nl_connect(route_sock, NETLINK_ROUTE) < 0 ||
        nl_connect(link_sock, NETLINK_ROUTE) < 0 ||
        nl_connect(addr_sock, NETLINK_ROUTE) < 0) {
        fprintf(stderr, "Unable to connect to a socket: %s\n", nl_geterror(-1));
        nl_socket_free(route_sock);
        nl_socket_free(link_sock);
        nl_socket_free(addr_sock);
        hash_table_free(ht);
        return EXIT_FAILURE;
    }

    if (nl_socket_add_membership(route_sock, RTNLGRP_IPV4_ROUTE) < 0) {
        fprintf(stderr, "Unable to join IPv4 route group\n");
        goto cleanup;
    }
    printf("Subscribed to IPv4 route group\n");

    if (nl_socket_add_membership(link_sock, RTNLGRP_LINK) < 0) {
        fprintf(stderr, "Unable to join link group\n");
        goto cleanup;
    }
    printf("Subscribed to link group\n");

    if (nl_socket_add_membership(addr_sock, RTNLGRP_IPV4_IFADDR) < 0) {
        fprintf(stderr, "Unable to join IPv4 address group\n");
        goto cleanup;
    }
    printf("Subscribed to IPv4 address group\n");

    nl_socket_modify_cb(route_sock, NL_CB_VALID, NL_CB_CUSTOM, route_callback, ht);
    nl_socket_modify_cb(link_sock, NL_CB_VALID, NL_CB_CUSTOM, link_callback, ht);
    nl_socket_modify_cb(addr_sock, NL_CB_VALID, NL_CB_CUSTOM, addr_callback, ht);

    while (1) {
        fd_set fds;
        int max_fd = -1;

        FD_ZERO(&fds);
        int route_fd = nl_socket_get_fd(route_sock);
        int link_fd = nl_socket_get_fd(link_sock);
        int addr_fd = nl_socket_get_fd(addr_sock);

        FD_SET(route_fd, &fds);
        FD_SET(link_fd, &fds);
        FD_SET(addr_fd, &fds);
        max_fd = (route_fd > link_fd ? route_fd : link_fd);
        max_fd = (max_fd > addr_fd ? max_fd : addr_fd);

        int ret = select(max_fd + 1, &fds, NULL, NULL, NULL);
        if (ret < 0) {
            perror("select failed");
            break;
        }

        if (FD_ISSET(route_fd, &fds)) {
            if (nl_recvmsgs_default(route_sock) < 0) {
                fprintf(stderr, "Failed to receive route messages: %s\n", nl_geterror(-1));
            }
        }
        if (FD_ISSET(link_fd, &fds)) {
            if (nl_recvmsgs_default(link_sock) < 0) {
                fprintf(stderr, "Failed to receive link messages: %s\n", nl_geterror(-1));
            }
        }
        if (FD_ISSET(addr_fd, &fds)) {
            if (nl_recvmsgs_default(addr_sock) < 0) {
                fprintf(stderr, "Failed to receive addr messages: %s\n", nl_geterror(-1));
            }
        }
    }

cleanup:
    nl_socket_free(route_sock);
    nl_socket_free(link_sock);
    nl_socket_free(addr_sock);
    hash_table_free(ht);
    return EXIT_SUCCESS;
}
