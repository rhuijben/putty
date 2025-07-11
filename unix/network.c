/*
 * Unix networking abstraction.
 */

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <sys/un.h>
#include <pwd.h>
#include <grp.h>

#include "putty.h"
#include "network.h"
#include "tree234.h"

/* Solaris needs <sys/sockio.h> for SIOCATMARK. */
#ifndef SIOCATMARK
#include <sys/sockio.h>
#endif

#ifndef X11_UNIX_PATH
# define X11_UNIX_PATH "/tmp/.X11-unix/X"
#endif

/*
 * Access to sockaddr types without breaking C strict aliasing rules.
 */
union sockaddr_union {
    struct sockaddr_storage storage;
    struct sockaddr sa;
    struct sockaddr_in sin;
#ifndef NO_IPV6
    struct sockaddr_in6 sin6;
#endif
    struct sockaddr_un su;
};

/*
 * Mutable state that goes with a SockAddr: stores information
 * about where in the list of candidate IP(v*) addresses we've
 * currently got to.
 */
typedef struct SockAddrStep_tag SockAddrStep;
struct SockAddrStep_tag {
#ifndef NO_IPV6
    struct addrinfo *ai;               /* steps along addr->ais */
#endif
    int curraddr;
};

typedef struct NetSocket NetSocket;
struct NetSocket {
    const char *error;
    int s;
    Plug *plug;
    bufchain output_data;
    bool connected;                    /* irrelevant for listening sockets */
    bool writable;
    bool frozen; /* this causes readability notifications to be ignored */
    bool localhost_only;               /* for listening sockets */
    char oobdata[1];
    size_t sending_oob;
    bool oobpending;        /* is there OOB data available to read? */
    bool oobinline;
    enum { EOF_NO, EOF_PENDING, EOF_SENT } outgoingeof;
    bool incomingeof;
    int pending_error;                 /* in case send() returns error */
    bool listener;
    bool nodelay, keepalive;           /* for connect()-type sockets */
    bool privport;
    int port;                          /* and again */
    SockAddr *addr;
    SockAddrStep step;
    /*
     * We sometimes need pairs of Socket structures to be linked:
     * if we are listening on the same IPv6 and v4 port, for
     * example. So here we define `parent' and `child' pointers to
     * track this link.
     */
    NetSocket *parent, *child;

    Socket sock;
};

struct SockAddr {
    int refcount;
    const char *error;
    enum { UNRESOLVED, UNIX, IP } superfamily;
#ifndef NO_IPV6
    struct addrinfo *ais;              /* Addresses IPv6 style. */
#else
    unsigned long *addresses;          /* Addresses IPv4 style. */
    int naddresses;
#endif
    char hostname[512];                /* Store an unresolved host name. */
};

/*
 * Which address family this address belongs to. AF_INET for IPv4;
 * AF_INET6 for IPv6; AF_UNSPEC indicates that name resolution has
 * not been done and a simple host name is held in this SockAddr
 * structure.
 */
#ifndef NO_IPV6
#define SOCKADDR_FAMILY(addr, step) \
    ((addr)->superfamily == UNRESOLVED ? AF_UNSPEC : \
     (addr)->superfamily == UNIX ? AF_UNIX : \
     (step).ai ? (step).ai->ai_family : AF_INET)
#else
/* Here we gratuitously reference 'step' to avoid gcc warnings about
 * 'set but not used' when compiling -DNO_IPV6 */
#define SOCKADDR_FAMILY(addr, step) \
    ((addr)->superfamily == UNRESOLVED ? AF_UNSPEC : \
     (addr)->superfamily == UNIX ? AF_UNIX : \
     (step).curraddr ? AF_INET : AF_INET)
#endif

/*
 * Start a SockAddrStep structure to step through multiple
 * addresses.
 */
#ifndef NO_IPV6
#define START_STEP(addr, step) \
    ((step).ai = (addr)->ais, (step).curraddr = 0)
#else
#define START_STEP(addr, step) \
    ((step).curraddr = 0)
#endif

static tree234 *sktree;

static void uxsel_tell(NetSocket *s);

static int cmpfortree(void *av, void *bv)
{
    NetSocket *a = (NetSocket *) av, *b = (NetSocket *) bv;
    int as = a->s, bs = b->s;
    if (as < bs)
        return -1;
    if (as > bs)
        return +1;
    if (a < b)
        return -1;
    if (a > b)
        return +1;
    return 0;
}

static int cmpforsearch(void *av, void *bv)
{
    NetSocket *b = (NetSocket *) bv;
    int as = *(int *)av, bs = b->s;
    if (as < bs)
        return -1;
    if (as > bs)
        return +1;
    return 0;
}

void sk_init(void)
{
    sktree = newtree234(cmpfortree);
}

void sk_cleanup(void)
{
    NetSocket *s;
    int i;

    if (sktree) {
        for (i = 0; (s = index234(sktree, i)) != NULL; i++) {
            close(s->s);
        }
    }
}

SockAddr *sk_namelookup(const char *host, char **canonicalname,
                        int address_family)
{
    *canonicalname = NULL;

    if (host[0] == '/') {
        *canonicalname = dupstr(host);
        return unix_sock_addr(host);
    }

    SockAddr *addr = snew(SockAddr);
    memset(addr, 0, sizeof(SockAddr));
    addr->superfamily = UNRESOLVED;
    addr->refcount = 1;

#ifndef NO_IPV6
    /*
     * Use getaddrinfo, as long as it's available. This should handle
     * both IPv4 and IPv6 address literals, and hostnames, in one
     * unified API.
     */
    {
        struct addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = (address_family == ADDRTYPE_IPV4 ? AF_INET :
                           address_family == ADDRTYPE_IPV6 ? AF_INET6 :
                           AF_UNSPEC);
        hints.ai_flags = AI_CANONNAME;
        hints.ai_socktype = SOCK_STREAM;

        /* strip [] on IPv6 address literals */
        char *trimmed_host = host_strduptrim(host);
        int err = getaddrinfo(trimmed_host, NULL, &hints, &addr->ais);
        sfree(trimmed_host);

        if (addr->ais) {
            addr->superfamily = IP;
            if (addr->ais->ai_canonname)
                *canonicalname = dupstr(addr->ais->ai_canonname);
            else
                *canonicalname = dupstr(host);
        } else {
            addr->error = gai_strerror(err);
        }
        return addr;
    }

#else
    /*
     * Failing that (if IPv6 support was not compiled in), try the
     * old-fashioned approach, which is to start by manually checking
     * for an IPv4 literal and then use gethostbyname.
     */
    unsigned long a = inet_addr(host);
    if (a != (unsigned long) INADDR_NONE) {
        addr->addresses = snew(unsigned long);
        addr->naddresses = 1;
        addr->addresses[0] = ntohl(a);
        addr->superfamily = IP;
        *canonicalname = dupstr(host);
        return addr;
    }

    struct hostent *h = gethostbyname(host);
    if (h) {
        addr->superfamily = IP;

        size_t n;
        for (n = 0; h->h_addr_list[n]; n++);
        addr->addresses = snewn(n, unsigned long);
        addr->naddresses = n;
        for (n = 0; n < addr->naddresses; n++) {
            uint32_t a;
            memcpy(&a, h->h_addr_list[n], sizeof(a));
            addr->addresses[n] = ntohl(a);
        }

        *canonicalname = dupstr(h->h_name);
    } else {
        addr->error = hstrerror(h_errno);
    }
    return addr;
#endif
}

SockAddr *sk_nonamelookup(const char *host)
{
    SockAddr *addr = snew(SockAddr);
    addr->error = NULL;
    addr->superfamily = UNRESOLVED;
    strncpy(addr->hostname, host, lenof(addr->hostname));
    addr->hostname[lenof(addr->hostname)-1] = '\0';
#ifndef NO_IPV6
    addr->ais = NULL;
#else
    addr->addresses = NULL;
#endif
    addr->refcount = 1;
    return addr;
}

static bool sk_nextaddr(SockAddr *addr, SockAddrStep *step)
{
#ifndef NO_IPV6
    if (step->ai && step->ai->ai_next) {
        step->ai = step->ai->ai_next;
        return true;
    } else
        return false;
#else
    if (step->curraddr+1 < addr->naddresses) {
        step->curraddr++;
        return true;
    } else {
        return false;
    }
#endif
}

void sk_getaddr(SockAddr *addr, char *buf, int buflen)
{
    if (addr->superfamily == UNRESOLVED || addr->superfamily == UNIX) {
        strncpy(buf, addr->hostname, buflen);
        buf[buflen-1] = '\0';
    } else {
#ifndef NO_IPV6
        if (getnameinfo(addr->ais->ai_addr, addr->ais->ai_addrlen, buf, buflen,
                        NULL, 0, NI_NUMERICHOST) != 0) {
            buf[0] = '\0';
            strncat(buf, "<unknown>", buflen - 1);
        }
#else
        struct in_addr a;
        SockAddrStep step;
        START_STEP(addr, step);
        assert(SOCKADDR_FAMILY(addr, step) == AF_INET);
        a.s_addr = htonl(addr->addresses[0]);
        strncpy(buf, inet_ntoa(a), buflen);
        buf[buflen-1] = '\0';
#endif
    }
}

/*
 * This constructs a SockAddr that points at one specific sub-address
 * of a parent SockAddr. The returned SockAddr does not own all its
 * own memory: it points into the old one's data structures, so it
 * MUST NOT be used after the old one is freed, and it MUST NOT be
 * passed to sk_addr_free. (The latter is why it's returned by value
 * rather than dynamically allocated - that should clue in anyone
 * writing a call to it that something is weird about it.)
 */
static SockAddr sk_extractaddr_tmp(
    SockAddr *addr, const SockAddrStep *step)
{
    SockAddr toret;
    toret = *addr;                    /* structure copy */
    toret.refcount = 1;

    if (addr->superfamily == IP) {
#ifndef NO_IPV6
        toret.ais = step->ai;
#else
        assert(SOCKADDR_FAMILY(addr, *step) == AF_INET);
        toret.addresses += step->curraddr;
#endif
    }

    return toret;
}

bool sk_addr_needs_port(SockAddr *addr)
{
    if (addr->superfamily == UNRESOLVED || addr->superfamily == UNIX) {
        return false;
    } else {
        return true;
    }
}

bool sk_hostname_is_local(const char *name)
{
    return !strcmp(name, "localhost") ||
           !strcmp(name, "::1") ||
           !strncmp(name, "127.", 4);
}

#define ipv4_is_loopback(addr) \
    (((addr).s_addr & htonl(0xff000000)) == htonl(0x7f000000))

static bool sockaddr_is_loopback(struct sockaddr *sa)
{
    union sockaddr_union *u = (union sockaddr_union *)sa;
    switch (u->sa.sa_family) {
      case AF_INET:
        return ipv4_is_loopback(u->sin.sin_addr);
#ifndef NO_IPV6
      case AF_INET6:
        return IN6_IS_ADDR_LOOPBACK(&u->sin6.sin6_addr);
#endif
      case AF_UNIX:
        return true;
      default:
        return false;
    }
}

bool sk_address_is_local(SockAddr *addr)
{
    if (addr->superfamily == UNRESOLVED)
        return false;                  /* we don't know; assume not */
    else if (addr->superfamily == UNIX)
        return true;
    else {
#ifndef NO_IPV6
        return sockaddr_is_loopback(addr->ais->ai_addr);
#else
        struct in_addr a;
        SockAddrStep step;
        START_STEP(addr, step);
        assert(SOCKADDR_FAMILY(addr, step) == AF_INET);
        a.s_addr = htonl(addr->addresses[0]);
        return ipv4_is_loopback(a);
#endif
    }
}

bool sk_address_is_special_local(SockAddr *addr)
{
    return addr->superfamily == UNIX;
}

int sk_addrtype(SockAddr *addr)
{
    SockAddrStep step;
    int family;
    START_STEP(addr, step);
    family = SOCKADDR_FAMILY(addr, step);

    return (family == AF_INET ? ADDRTYPE_IPV4 :
#ifndef NO_IPV6
            family == AF_INET6 ? ADDRTYPE_IPV6 :
#endif
            ADDRTYPE_NAME);
}

void sk_addrcopy(SockAddr *addr, char *buf)
{
    SockAddrStep step;
    int family;
    START_STEP(addr, step);
    family = SOCKADDR_FAMILY(addr, step);

#ifndef NO_IPV6
    if (family == AF_INET)
        memcpy(buf, &((struct sockaddr_in *)step.ai->ai_addr)->sin_addr,
               sizeof(struct in_addr));
    else if (family == AF_INET6)
        memcpy(buf, &((struct sockaddr_in6 *)step.ai->ai_addr)->sin6_addr,
               sizeof(struct in6_addr));
    else
        unreachable("bad address family in sk_addrcopy");
#else
    struct in_addr a;

    assert(family == AF_INET);
    a.s_addr = htonl(addr->addresses[step.curraddr]);
    memcpy(buf, (char*) &a.s_addr, 4);
#endif
}

void sk_addr_free(SockAddr *addr)
{
    if (--addr->refcount > 0)
        return;
#ifndef NO_IPV6
    if (addr->ais != NULL)
        freeaddrinfo(addr->ais);
#else
    sfree(addr->addresses);
#endif
    sfree(addr);
}

SockAddr *sk_addr_dup(SockAddr *addr)
{
    addr->refcount++;
    return addr;
}

static Plug *sk_net_plug(Socket *sock, Plug *p)
{
    NetSocket *s = container_of(sock, NetSocket, sock);
    Plug *ret = s->plug;
    if (p)
        s->plug = p;
    return ret;
}

static void sk_net_close(Socket *s);
static size_t sk_net_write(Socket *s, const void *data, size_t len);
static size_t sk_net_write_oob(Socket *s, const void *data, size_t len);
static void sk_net_write_eof(Socket *s);
static void sk_net_set_frozen(Socket *s, bool is_frozen);
static SocketEndpointInfo *sk_net_endpoint_info(Socket *s, bool peer);
static const char *sk_net_socket_error(Socket *s);

static const SocketVtable NetSocket_sockvt = {
    .plug = sk_net_plug,
    .close = sk_net_close,
    .write = sk_net_write,
    .write_oob = sk_net_write_oob,
    .write_eof = sk_net_write_eof,
    .set_frozen = sk_net_set_frozen,
    .socket_error = sk_net_socket_error,
    .endpoint_info = sk_net_endpoint_info,
};

static Socket *sk_net_accept(accept_ctx_t ctx, Plug *plug)
{
    int sockfd = ctx.i;
    NetSocket *s;

    /*
     * Create NetSocket structure.
     */
    s = snew(NetSocket);
    s->sock.vt = &NetSocket_sockvt;
    s->error = NULL;
    s->plug = plug;
    bufchain_init(&s->output_data);
    s->writable = true;              /* to start with */
    s->sending_oob = 0;
    s->frozen = true;
    s->localhost_only = false;    /* unused, but best init anyway */
    s->pending_error = 0;
    s->oobpending = false;
    s->outgoingeof = EOF_NO;
    s->incomingeof = false;
    s->listener = false;
    s->parent = s->child = NULL;
    s->addr = NULL;
    s->connected = true;

    s->s = sockfd;

    if (s->s < 0) {
        s->error = strerror(errno);
        return &s->sock;
    }

    s->oobinline = false;

    uxsel_tell(s);
    add234(sktree, s);

    return &s->sock;
}

static int try_connect(NetSocket *sock)
{
    int s;
    union sockaddr_union u;
    const union sockaddr_union *sa;
    int err = 0;
    short localport;
    int salen, family;

    /*
     * Remove the socket from the tree before we overwrite its
     * internal socket id, because that forms part of the tree's
     * sorting criterion. We'll add it back before exiting this
     * function, whether we changed anything or not.
     */
    del234(sktree, sock);

    if (sock->s >= 0)
        close(sock->s);

    {
        SockAddr thisaddr = sk_extractaddr_tmp(
            sock->addr, &sock->step);
        plug_log(sock->plug, &sock->sock, PLUGLOG_CONNECT_TRYING,
                 &thisaddr, sock->port, NULL, 0);
    }

    /*
     * Open socket.
     */
    family = SOCKADDR_FAMILY(sock->addr, sock->step);
    assert(family != AF_UNSPEC);
    s = socket(family, SOCK_STREAM, 0);
    sock->s = s;

    if (s < 0) {
        err = errno;
        goto ret;
    }

    cloexec(s);

    if (sock->oobinline) {
        int b = 1;
        if (setsockopt(s, SOL_SOCKET, SO_OOBINLINE,
                       (void *) &b, sizeof(b)) < 0) {
            err = errno;
            close(s);
            goto ret;
        }
    }

    if (sock->nodelay && family != AF_UNIX) {
        int b = 1;
        if (setsockopt(s, IPPROTO_TCP, TCP_NODELAY,
                       (void *) &b, sizeof(b)) < 0) {
            err = errno;
            close(s);
            goto ret;
        }
    }

    if (sock->keepalive) {
        int b = 1;
        if (setsockopt(s, SOL_SOCKET, SO_KEEPALIVE,
                       (void *) &b, sizeof(b)) < 0) {
            err = errno;
            close(s);
            goto ret;
        }
    }

    /*
     * Bind to local address.
     */
    if (sock->privport)
        localport = 1023;              /* count from 1023 downwards */
    else
        localport = 0;                 /* just use port 0 (ie kernel picks) */

    /* BSD IP stacks need sockaddr_in zeroed before filling in */
    memset(&u,'\0',sizeof(u));

    /* We don't try to bind to a local address for UNIX domain sockets.  (Why
     * do we bother doing the bind when localport == 0 anyway?) */
    if (family != AF_UNIX) {
        /* Loop round trying to bind */
        while (1) {
            int retcode;

#ifndef NO_IPV6
            if (family == AF_INET6) {
                /* XXX use getaddrinfo to get a local address? */
                u.sin6.sin6_family = AF_INET6;
                u.sin6.sin6_addr = in6addr_any;
                u.sin6.sin6_port = htons(localport);
                retcode = bind(s, &u.sa, sizeof(u.sin6));
            } else
#endif
            {
                assert(family == AF_INET);
                u.sin.sin_family = AF_INET;
                u.sin.sin_addr.s_addr = htonl(INADDR_ANY);
                u.sin.sin_port = htons(localport);
                retcode = bind(s, &u.sa, sizeof(u.sin));
            }
            if (retcode >= 0) {
                err = 0;
                break;                 /* done */
            } else {
                err = errno;
                if (err != EADDRINUSE) /* failed, for a bad reason */
                    break;
            }

            if (localport == 0)
                break;                   /* we're only looping once */
            localport--;
            if (localport == 0)
                break;                   /* we might have got to the end */
        }

        if (err)
            goto ret;
    }

    /*
     * Connect to remote address.
     */
    switch(family) {
#ifndef NO_IPV6
      case AF_INET:
        /* XXX would be better to have got getaddrinfo() to fill in the port. */
        ((struct sockaddr_in *)sock->step.ai->ai_addr)->sin_port =
            htons(sock->port);
        sa = (const union sockaddr_union *)sock->step.ai->ai_addr;
        salen = sock->step.ai->ai_addrlen;
        break;
      case AF_INET6:
        ((struct sockaddr_in *)sock->step.ai->ai_addr)->sin_port =
            htons(sock->port);
        sa = (const union sockaddr_union *)sock->step.ai->ai_addr;
        salen = sock->step.ai->ai_addrlen;
        break;
#else
      case AF_INET:
        u.sin.sin_family = AF_INET;
        u.sin.sin_addr.s_addr = htonl(sock->addr->addresses[sock->step.curraddr]);
        u.sin.sin_port = htons((short) sock->port);
        sa = &u;
        salen = sizeof u.sin;
        break;
#endif
      case AF_UNIX:
        assert(strlen(sock->addr->hostname) < sizeof u.su.sun_path);
        u.su.sun_family = AF_UNIX;
        strcpy(u.su.sun_path, sock->addr->hostname);
        sa = &u;
        salen = sizeof u.su;
        break;

      default:
        unreachable("unknown address family");
        exit(1); /* XXX: GCC doesn't understand assert() on some systems. */
    }

    nonblock(s);

    if ((connect(s, &(sa->sa), salen)) < 0) {
        if ( errno != EINPROGRESS ) {
            err = errno;
            goto ret;
        }
    } else {
        /*
         * If we _don't_ get EWOULDBLOCK, the connect has completed
         * and we should set the socket as connected and writable.
         */
        sock->connected = true;
        sock->writable = true;

        SockAddr thisaddr = sk_extractaddr_tmp(sock->addr, &sock->step);
        plug_log(sock->plug, &sock->sock, PLUGLOG_CONNECT_SUCCESS,
                 &thisaddr, sock->port, NULL, 0);
    }

    uxsel_tell(sock);

  ret:

    /*
     * No matter what happened, put the socket back in the tree.
     */
    add234(sktree, sock);

    if (err) {
        SockAddr thisaddr = sk_extractaddr_tmp(
            sock->addr, &sock->step);
        plug_log(sock->plug, &sock->sock, PLUGLOG_CONNECT_FAILED,
                 &thisaddr, sock->port, strerror(err), err);
    }
    return err;
}

Socket *sk_new(SockAddr *addr, int port, bool privport, bool oobinline,
               bool nodelay, bool keepalive, Plug *plug)
{
    NetSocket *s;
    int err;

    /*
     * Create NetSocket structure.
     */
    s = snew(NetSocket);
    s->sock.vt = &NetSocket_sockvt;
    s->error = NULL;
    s->plug = plug;
    bufchain_init(&s->output_data);
    s->connected = false;            /* to start with */
    s->writable = false;             /* to start with */
    s->sending_oob = 0;
    s->frozen = false;
    s->localhost_only = false;    /* unused, but best init anyway */
    s->pending_error = 0;
    s->parent = s->child = NULL;
    s->oobpending = false;
    s->outgoingeof = EOF_NO;
    s->incomingeof = false;
    s->listener = false;
    s->addr = addr;
    START_STEP(s->addr, s->step);
    s->s = -1;
    s->oobinline = oobinline;
    s->nodelay = nodelay;
    s->keepalive = keepalive;
    s->privport = privport;
    s->port = port;

    do {
        err = try_connect(s);
    } while (err && sk_nextaddr(s->addr, &s->step));

    if (err)
        s->error = strerror(err);

    return &s->sock;
}

Socket *sk_newlistener(const char *srcaddr, int port, Plug *plug,
                       bool local_host_only, int orig_address_family)
{
    int fd;
#ifndef NO_IPV6
    struct addrinfo hints, *ai = NULL;
    char portstr[6];
#endif
    union sockaddr_union u;
    union sockaddr_union *addr;
    int addrlen;
    NetSocket *s;
    int retcode;
    int address_family;
    int on = 1;

    /*
     * Create NetSocket structure.
     */
    s = snew(NetSocket);
    s->sock.vt = &NetSocket_sockvt;
    s->error = NULL;
    s->plug = plug;
    bufchain_init(&s->output_data);
    s->writable = false;             /* to start with */
    s->sending_oob = 0;
    s->frozen = false;
    s->localhost_only = local_host_only;
    s->pending_error = 0;
    s->parent = s->child = NULL;
    s->oobpending = false;
    s->outgoingeof = EOF_NO;
    s->incomingeof = false;
    s->listener = true;
    s->addr = NULL;
    s->s = -1;

    /*
     * Translate address_family from platform-independent constants
     * into local reality.
     */
    address_family = (orig_address_family == ADDRTYPE_IPV4 ? AF_INET :
#ifndef NO_IPV6
                      orig_address_family == ADDRTYPE_IPV6 ? AF_INET6 :
#endif
                      AF_UNSPEC);

#ifndef NO_IPV6
    /* Let's default to IPv6.
     * If the stack doesn't support IPv6, we will fall back to IPv4. */
    if (address_family == AF_UNSPEC) address_family = AF_INET6;
#else
    /* No other choice, default to IPv4 */
    if (address_family == AF_UNSPEC)  address_family = AF_INET;
#endif

    /*
     * Open socket.
     */
    fd = socket(address_family, SOCK_STREAM, 0);

#ifndef NO_IPV6
    /* If the host doesn't support IPv6 try fallback to IPv4. */
    if (fd < 0 && address_family == AF_INET6) {
        address_family = AF_INET;
        fd = socket(address_family, SOCK_STREAM, 0);
    }
#endif

    if (fd < 0) {
        s->error = strerror(errno);
        return &s->sock;
    }

    cloexec(fd);

    s->oobinline = false;

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                   (const char *)&on, sizeof(on)) < 0) {
        s->error = strerror(errno);
        close(fd);
        return &s->sock;
    }

    retcode = -1;
    addr = NULL; addrlen = -1;         /* placate optimiser */

    if (srcaddr != NULL) {
#ifndef NO_IPV6
        hints.ai_flags = AI_NUMERICHOST;
        hints.ai_family = address_family;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = 0;
        hints.ai_addrlen = 0;
        hints.ai_addr = NULL;
        hints.ai_canonname = NULL;
        hints.ai_next = NULL;
        assert(port >= 0 && port <= 99999);
        sprintf(portstr, "%d", port);
        {
            char *trimmed_addr = host_strduptrim(srcaddr);
            retcode = getaddrinfo(trimmed_addr, portstr, &hints, &ai);
            sfree(trimmed_addr);
        }
        if (retcode == 0) {
            addr = (union sockaddr_union *)ai->ai_addr;
            addrlen = ai->ai_addrlen;
        }
#else
        memset(&u,'\0',sizeof u);
        u.sin.sin_family = AF_INET;
        u.sin.sin_port = htons(port);
        u.sin.sin_addr.s_addr = inet_addr(srcaddr);
        if (u.sin.sin_addr.s_addr != (in_addr_t)(-1)) {
            /* Override localhost_only with specified listen addr. */
            s->localhost_only = ipv4_is_loopback(u.sin.sin_addr);
        }
        addr = &u;
        addrlen = sizeof(u.sin);
        retcode = 0;
#endif
    }

    if (retcode != 0) {
        memset(&u,'\0',sizeof u);
#ifndef NO_IPV6
        if (address_family == AF_INET6) {
            u.sin6.sin6_family = AF_INET6;
            u.sin6.sin6_port = htons(port);
            if (local_host_only)
                u.sin6.sin6_addr = in6addr_loopback;
            else
                u.sin6.sin6_addr = in6addr_any;
            addr = &u;
            addrlen = sizeof(u.sin6);
        } else
#endif
        {
            u.sin.sin_family = AF_INET;
            u.sin.sin_port = htons(port);
            if (local_host_only)
                u.sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            else
                u.sin.sin_addr.s_addr = htonl(INADDR_ANY);
            addr = &u;
            addrlen = sizeof(u.sin);
        }
    }

    retcode = bind(fd, &addr->sa, addrlen);

#ifndef NO_IPV6
    if (ai)
        freeaddrinfo(ai);
#endif

    if (retcode < 0) {
        close(fd);
        s->error = strerror(errno);
        return &s->sock;
    }

    if (listen(fd, SOMAXCONN) < 0) {
        close(fd);
        s->error = strerror(errno);
        return &s->sock;
    }

#ifndef NO_IPV6
    /*
     * If we were given ADDRTYPE_UNSPEC, we must also create an
     * IPv4 listening socket and link it to this one.
     */
    if (address_family == AF_INET6 && orig_address_family == ADDRTYPE_UNSPEC) {
        NetSocket *other;

        other = container_of(
            sk_newlistener(srcaddr, port, plug,
                           local_host_only, ADDRTYPE_IPV4),
            NetSocket, sock);

        if (other) {
            if (!other->error) {
                other->parent = s;
                s->child = other;
            } else {
                /* If we couldn't create a listening socket on IPv4 as well
                 * as IPv6, we must return an error overall. */
                close(fd);
                sfree(s);
                return &other->sock;
            }
        }
    }
#endif

    s->s = fd;

    uxsel_tell(s);
    add234(sktree, s);

    return &s->sock;
}

static void sk_net_close(Socket *sock)
{
    NetSocket *s = container_of(sock, NetSocket, sock);

    if (s->child)
        sk_net_close(&s->child->sock);

    bufchain_clear(&s->output_data);

    del234(sktree, s);
    if (s->s >= 0) {
        uxsel_del(s->s);
        close(s->s);
    }
    if (s->addr)
        sk_addr_free(s->addr);
    delete_callbacks_for_context(s);
    sfree(s);
}

void *sk_getxdmdata(Socket *sock, int *lenp)
{
    NetSocket *s;
    union sockaddr_union u;
    socklen_t addrlen;
    char *buf;
    static unsigned int unix_addr = 0xFFFFFFFF;

    /*
     * We must check that this socket really _is_ a NetSocket before
     * downcasting it.
     */
    if (sock->vt != &NetSocket_sockvt)
        return NULL;                   /* failure */
    s = container_of(sock, NetSocket, sock);

    addrlen = sizeof(u);
    if (getsockname(s->s, &u.sa, &addrlen) < 0)
        return NULL;
    switch(u.sa.sa_family) {
      case AF_INET:
        *lenp = 6;
        buf = snewn(*lenp, char);
        PUT_32BIT_MSB_FIRST(buf, ntohl(u.sin.sin_addr.s_addr));
        PUT_16BIT_MSB_FIRST(buf+4, ntohs(u.sin.sin_port));
        break;
#ifndef NO_IPV6
      case AF_INET6:
        *lenp = 6;
        buf = snewn(*lenp, char);
        if (IN6_IS_ADDR_V4MAPPED(&u.sin6.sin6_addr)) {
            memcpy(buf, u.sin6.sin6_addr.s6_addr + 12, 4);
            PUT_16BIT_MSB_FIRST(buf+4, ntohs(u.sin6.sin6_port));
        } else
            /* This is stupid, but it's what XLib does. */
            memset(buf, 0, 6);
        break;
#endif
      case AF_UNIX:
        *lenp = 6;
        buf = snewn(*lenp, char);
        PUT_32BIT_MSB_FIRST(buf, unix_addr--);
        PUT_16BIT_MSB_FIRST(buf+4, getpid());
        break;

        /* XXX IPV6 */

      default:
        return NULL;
    }

    return buf;
}

void plug_closing_errno(Plug *plug, int error)
{
    PlugCloseType type = PLUGCLOSE_ERROR;
    if (error == EPIPE)
        type = PLUGCLOSE_BROKEN_PIPE;
    plug_closing(plug, type, strerror(error));
}

/*
 * Deal with socket errors detected in try_send().
 */
static void socket_error_callback(void *vs)
{
    NetSocket *s = (NetSocket *)vs;

    /*
     * Just in case other socket work has caused this socket to vanish
     * or become somehow non-erroneous before this callback arrived...
     */
    if (!find234(sktree, s, NULL) || !s->pending_error)
        return;

    /*
     * An error has occurred on this socket. Pass it to the plug.
     */
    plug_closing_errno(s->plug, s->pending_error);
}

/*
 * The function which tries to send on a socket once it's deemed
 * writable.
 */
void try_send(NetSocket *s)
{
    while (s->sending_oob || bufchain_size(&s->output_data) > 0) {
        int nsent;
        int err;
        const void *data;
        size_t len;
        int urgentflag;

        if (s->sending_oob) {
            urgentflag = MSG_OOB;
            len = s->sending_oob;
            data = &s->oobdata;
        } else {
            urgentflag = 0;
            ptrlen bufdata = bufchain_prefix(&s->output_data);
            data = bufdata.ptr;
            len = bufdata.len;
        }
        nsent = send(s->s, data, len, MSG_NOSIGNAL | urgentflag);
        noise_ultralight(NOISE_SOURCE_IOLEN, nsent);
        if (nsent <= 0) {
            err = (nsent < 0 ? errno : 0);
            if (err == EWOULDBLOCK) {
                /*
                 * Perfectly normal: we've sent all we can for the moment.
                 */
                s->writable = false;
                return;
            } else {
                /*
                 * We unfortunately can't just call plug_closing(),
                 * because it's quite likely that we're currently
                 * _in_ a call from the code we'd be calling back
                 * to, so we'd have to make half the SSH code
                 * reentrant. Instead we flag a pending error on
                 * the socket, to be dealt with (by calling
                 * plug_closing()) at some suitable future moment.
                 */
                s->pending_error = err;
                /*
                 * Immediately cease selecting on this socket, so that
                 * we don't tight-loop repeatedly trying to do
                 * whatever it was that went wrong.
                 */
                uxsel_tell(s);
                /*
                 * Arrange to be called back from the top level to
                 * deal with the error condition on this socket.
                 */
                queue_toplevel_callback(socket_error_callback, s);
                return;
            }
        } else {
            if (s->sending_oob) {
                if (nsent < len) {
                    memmove(s->oobdata, s->oobdata+nsent, len-nsent);
                    s->sending_oob = len - nsent;
                } else {
                    s->sending_oob = 0;
                }
            } else {
                bufchain_consume(&s->output_data, nsent);
            }
        }
    }

    /*
     * If we reach here, we've finished sending everything we might
     * have needed to send. Send EOF, if we need to.
     */
    if (s->outgoingeof == EOF_PENDING) {
        shutdown(s->s, SHUT_WR);
        s->outgoingeof = EOF_SENT;
    }

    /*
     * Also update the select status, because we don't need to select
     * for writing any more.
     */
    uxsel_tell(s);
}

static size_t sk_net_write(Socket *sock, const void *buf, size_t len)
{
    NetSocket *s = container_of(sock, NetSocket, sock);

    assert(s->outgoingeof == EOF_NO);

    /*
     * Add the data to the buffer list on the socket.
     */
    bufchain_add(&s->output_data, buf, len);

    /*
     * Now try sending from the start of the buffer list.
     */
    if (s->writable)
        try_send(s);

    /*
     * Update the select() status to correctly reflect whether or
     * not we should be selecting for write.
     */
    uxsel_tell(s);

    return bufchain_size(&s->output_data);
}

static size_t sk_net_write_oob(Socket *sock, const void *buf, size_t len)
{
    NetSocket *s = container_of(sock, NetSocket, sock);

    assert(s->outgoingeof == EOF_NO);

    /*
     * Replace the buffer list on the socket with the data.
     */
    bufchain_clear(&s->output_data);
    assert(len <= sizeof(s->oobdata));
    memcpy(s->oobdata, buf, len);
    s->sending_oob = len;

    /*
     * Now try sending from the start of the buffer list.
     */
    if (s->writable)
        try_send(s);

    /*
     * Update the select() status to correctly reflect whether or
     * not we should be selecting for write.
     */
    uxsel_tell(s);

    return s->sending_oob;
}

static void sk_net_write_eof(Socket *sock)
{
    NetSocket *s = container_of(sock, NetSocket, sock);

    assert(s->outgoingeof == EOF_NO);

    /*
     * Mark the socket as pending outgoing EOF.
     */
    s->outgoingeof = EOF_PENDING;

    /*
     * Now try sending from the start of the buffer list.
     */
    if (s->writable)
        try_send(s);

    /*
     * Update the select() status to correctly reflect whether or
     * not we should be selecting for write.
     */
    uxsel_tell(s);
}

static void net_select_result(int fd, int event)
{
    int ret;
    char buf[20480];                   /* nice big buffer for plenty of speed */
    NetSocket *s;
    bool atmark = true;

    /* Find the Socket structure */
    s = find234(sktree, &fd, cmpforsearch);
    if (!s)
        return;                /* boggle */

    noise_ultralight(NOISE_SOURCE_IOID, fd);

    switch (event) {
      case SELECT_X:                   /* exceptional */
        if (!s->oobinline) {
            /*
             * On a non-oobinline socket, this indicates that we
             * can immediately perform an OOB read and get back OOB
             * data, which we will send to the back end with
             * type==2 (urgent data).
             */
            ret = recv(s->s, buf, sizeof(buf), MSG_OOB);
            noise_ultralight(NOISE_SOURCE_IOLEN, ret);
            if (ret == 0) {
                plug_closing_error(s->plug, "Internal networking trouble");
            } else if (ret < 0) {
                plug_closing_errno(s->plug, errno);
            } else {
                /*
                 * Receiving actual data on a socket means we can
                 * stop falling back through the candidate
                 * addresses to connect to.
                 */
                if (s->addr) {
                    sk_addr_free(s->addr);
                    s->addr = NULL;
                }
                plug_receive(s->plug, 2, buf, ret);
            }
            break;
        }

        /*
         * If we reach here, this is an oobinline socket, which
         * means we should set s->oobpending and then deal with it
         * when we get called for the readability event (which
         * should also occur).
         */
        s->oobpending = true;
        break;
      case SELECT_R:                   /* readable; also acceptance */
        if (s->listener) {
            /*
             * On a listening socket, the readability event means a
             * connection is ready to be accepted.
             */
            union sockaddr_union su;
            socklen_t addrlen = sizeof(su);
            accept_ctx_t actx;
            int t;  /* socket of connection */

            memset(&su, 0, addrlen);
            t = accept(s->s, &su.sa, &addrlen);
            if (t < 0) {
                break;
            }

            nonblock(t);
            actx.i = t;

            if ((!s->addr || s->addr->superfamily != UNIX) &&
                s->localhost_only && !sockaddr_is_loopback(&su.sa)) {
                close(t);              /* someone let nonlocal through?! */
            } else if (plug_accepting(s->plug, sk_net_accept, actx)) {
                close(t);              /* denied or error */
            }
            break;
        }

        /*
         * If we reach here, this is not a listening socket, so
         * readability really means readability.
         */

        /* In the case the socket is still frozen, we don't even bother */
        if (s->frozen)
            break;

        /*
         * We have received data on the socket. For an oobinline
         * socket, this might be data _before_ an urgent pointer,
         * in which case we send it to the back end with type==1
         * (data prior to urgent).
         */
        if (s->oobinline && s->oobpending) {
            int atmark_from_ioctl;
            if (ioctl(s->s, SIOCATMARK, &atmark_from_ioctl) == 0) {
                atmark = atmark_from_ioctl;
                if (atmark)
                    s->oobpending = false; /* clear this indicator */
            }
        } else
            atmark = true;

        ret = recv(s->s, buf, s->oobpending ? 1 : sizeof(buf), 0);
        noise_ultralight(NOISE_SOURCE_IOLEN, ret);
        if (ret < 0) {
            if (errno == EWOULDBLOCK) {
                break;
            }
        }
        if (ret < 0) {
            plug_closing_errno(s->plug, errno);
        } else if (0 == ret) {
            s->incomingeof = true;     /* stop trying to read now */
            uxsel_tell(s);
            plug_closing_normal(s->plug);
        } else {
            /*
             * Receiving actual data on a socket means we can
             * stop falling back through the candidate
             * addresses to connect to.
             */
            if (s->addr) {
                sk_addr_free(s->addr);
                s->addr = NULL;
            }
            plug_receive(s->plug, atmark ? 0 : 1, buf, ret);
        }
        break;
      case SELECT_W:                   /* writable */
        if (!s->connected) {
            /*
             * select/poll reports a socket as _writable_ when an
             * asynchronous connect() attempt either completes or
             * fails. So first we must find out which.
             */
            {
                int err;
                socklen_t errlen = sizeof(err);
                char *errmsg = NULL;
                if (getsockopt(s->s, SOL_SOCKET, SO_ERROR, &err, &errlen)<0) {
                    errmsg = dupprintf("getsockopt(SO_ERROR): %s",
                                       strerror(errno));
                    err = errno;       /* got to put something in here */
                } else if (err != 0) {
                    errmsg = dupstr(strerror(err));
                }
                if (errmsg) {
                    /*
                     * The asynchronous connection attempt failed.
                     * Report the problem via plug_log, and try again
                     * with the next candidate address, if we have
                     * more than one.
                     */
                    SockAddr thisaddr;
                    assert(s->addr);

                    thisaddr = sk_extractaddr_tmp(s->addr, &s->step);
                    plug_log(s->plug, &s->sock, PLUGLOG_CONNECT_FAILED,
                             &thisaddr, s->port, errmsg, err);

                    while (err && s->addr && sk_nextaddr(s->addr, &s->step)) {
                        err = try_connect(s);
                    }
                    if (err) {
                        plug_closing_errno(s->plug, err);
                        return;      /* socket is now presumably defunct */
                    }
                    if (!s->connected)
                        return;      /* another async attempt in progress */
                } else {
                    /*
                     * The connection attempt succeeded.
                     */
                    SockAddr thisaddr = sk_extractaddr_tmp(s->addr, &s->step);
                    plug_log(s->plug, &s->sock, PLUGLOG_CONNECT_SUCCESS,
                             &thisaddr, s->port, NULL, 0);
                }
            }

            /*
             * If we get here, we've managed to make a connection.
             */
            if (s->addr) {
                sk_addr_free(s->addr);
                s->addr = NULL;
            }
            s->connected = true;
            s->writable = true;
            uxsel_tell(s);
        } else {
            size_t bufsize_before, bufsize_after;
            s->writable = true;
            bufsize_before = s->sending_oob + bufchain_size(&s->output_data);
            try_send(s);
            bufsize_after = s->sending_oob + bufchain_size(&s->output_data);
            if (bufsize_after < bufsize_before)
                plug_sent(s->plug, bufsize_after);
        }
        break;
    }
}

/*
 * Special error values are returned from sk_namelookup and sk_new
 * if there's a problem. These functions extract an error message,
 * or return NULL if there's no problem.
 */
const char *sk_addr_error(SockAddr *addr)
{
    return addr->error;
}
static const char *sk_net_socket_error(Socket *sock)
{
    NetSocket *s = container_of(sock, NetSocket, sock);
    return s->error;
}

static void sk_net_set_frozen(Socket *sock, bool is_frozen)
{
    NetSocket *s = container_of(sock, NetSocket, sock);
    if (s->frozen == is_frozen)
        return;
    s->frozen = is_frozen;
    uxsel_tell(s);
}

static SocketEndpointInfo *sk_net_endpoint_info(Socket *sock, bool peer)
{
    NetSocket *s = container_of(sock, NetSocket, sock);
    union sockaddr_union addr;
    socklen_t addrlen = sizeof(addr);
#ifndef NO_IPV6
    char buf[INET6_ADDRSTRLEN];
#endif
    SocketEndpointInfo *pi;

    {
        int retd = (peer ? getpeername(s->s, &addr.sa, &addrlen) :
                    getsockname(s->s, &addr.sa, &addrlen));
        if (retd < 0)
            return NULL;
    }

    pi = snew(SocketEndpointInfo);
    pi->addressfamily = ADDRTYPE_UNSPEC;
    pi->addr_text = NULL;
    pi->port = -1;
    pi->log_text = NULL;

    if (addr.storage.ss_family == AF_INET) {
        pi->addressfamily = ADDRTYPE_IPV4;
        memcpy(pi->addr_bin.ipv4, &addr.sin.sin_addr, 4);
        pi->port = ntohs(addr.sin.sin_port);
        pi->addr_text = dupstr(inet_ntoa(addr.sin.sin_addr));
        pi->log_text = dupprintf("%s:%d", pi->addr_text, pi->port);

#ifndef NO_IPV6
    } else if (addr.storage.ss_family == AF_INET6) {
        pi->addressfamily = ADDRTYPE_IPV6;
        memcpy(pi->addr_bin.ipv6, &addr.sin6.sin6_addr, 16);
        pi->port = ntohs(addr.sin6.sin6_port);
        pi->addr_text = dupstr(
            inet_ntop(AF_INET6, &addr.sin6.sin6_addr, buf, sizeof(buf)));
        pi->log_text = dupprintf("[%s]:%d", pi->addr_text, pi->port);
#endif

    } else if (addr.storage.ss_family == AF_UNIX) {
        pi->addressfamily = ADDRTYPE_LOCAL;

        /*
         * For Unix sockets, the source address is unlikely to be
         * helpful, so we leave addr_txt NULL (and we certainly can't
         * fill in port, obviously). Instead, we try SO_PEERCRED and
         * try to get the source pid, and put that in the log text.
         */
        int pid, uid, gid;
        if (so_peercred(s->s, &pid, &uid, &gid)) {
            char uidbuf[64], gidbuf[64];
            sprintf(uidbuf, "%d", uid);
            sprintf(gidbuf, "%d", gid);
            struct passwd *pw = getpwuid(uid);
            struct group *gr = getgrgid(gid);
            pi->log_text = dupprintf("pid %d (%s:%s)", pid,
                                     pw ? pw->pw_name : uidbuf,
                                     gr ? gr->gr_name : gidbuf);
        }
    } else {
        sfree(pi);
        return NULL;
    }

    return pi;
}

int sk_net_get_fd(Socket *sock)
{
    /* This function is not fully general: it only works on NetSocket */
    if (sock->vt != &NetSocket_sockvt)
        return -1;                     /* failure */
    NetSocket *s = container_of(sock, NetSocket, sock);
    return s->s;
}

static void uxsel_tell(NetSocket *s)
{
    int rwx = 0;
    if (!s->pending_error) {
        if (s->listener) {
            rwx |= SELECT_R;           /* read == accept */
        } else {
            if (!s->connected)
                rwx |= SELECT_W;       /* write == connect */
            if (s->connected && !s->frozen && !s->incomingeof)
                rwx |= SELECT_R | SELECT_X;
            if (bufchain_size(&s->output_data))
                rwx |= SELECT_W;
        }
    }
    uxsel_set(s->s, rwx, net_select_result);
}

int net_service_lookup(const char *service)
{
    struct servent *se;
    se = getservbyname(service, NULL);
    if (se != NULL)
        return ntohs(se->s_port);
    else
        return 0;
}

char *get_hostname(void)
{
    size_t size = 0;
    char *hostname = NULL;
    do {
        sgrowarray(hostname, size, size);
        if ((gethostname(hostname, size) < 0) && (errno != ENAMETOOLONG)) {
            sfree(hostname);
            hostname = NULL;
            break;
        }
    } while (strlen(hostname) >= size-1);
    return hostname;
}

SockAddr *platform_get_x11_unix_address(const char *sockpath, int displaynum)
{
    SockAddr *addr = snew(SockAddr);
    int n;

    memset(addr, 0, sizeof *addr);
    addr->superfamily = UNIX;
    /*
     * In special circumstances (notably Mac OS X Leopard), we'll
     * have been passed an explicit Unix socket path.
     */
    if (sockpath) {
        n = snprintf(addr->hostname, sizeof addr->hostname,
                     "%s", sockpath);
    } else {
        n = snprintf(addr->hostname, sizeof addr->hostname,
                     "%s%d", X11_UNIX_PATH, displaynum);
    }

    if (n < 0)
        addr->error = "snprintf failed";
    else if (n >= sizeof addr->hostname)
        addr->error = "X11 UNIX name too long";

#ifndef NO_IPV6
    addr->ais = NULL;
#else
    addr->addresses = NULL;
    addr->naddresses = 0;
#endif
    addr->refcount = 1;
    return addr;
}

SockAddr *unix_sock_addr(const char *path)
{
    SockAddr *addr = snew(SockAddr);
    int n;

    memset(addr, 0, sizeof *addr);
    addr->superfamily = UNIX;
    n = snprintf(addr->hostname, sizeof addr->hostname, "%s", path);

    if (n < 0)
        addr->error = "snprintf failed";
    else if (n >= sizeof addr->hostname ||
             n >= sizeof(((struct sockaddr_un *)0)->sun_path))
        addr->error = "socket pathname too long";

#ifndef NO_IPV6
    addr->ais = NULL;
#else
    addr->addresses = NULL;
    addr->naddresses = 0;
#endif
    addr->refcount = 1;
    return addr;
}

Socket *new_unix_listener(SockAddr *listenaddr, Plug *plug)
{
    int fd;
    union sockaddr_union u;
    union sockaddr_union *addr;
    int addrlen;
    NetSocket *s;
    int retcode;

    /*
     * Create NetSocket structure.
     */
    s = snew(NetSocket);
    s->sock.vt = &NetSocket_sockvt;
    s->error = NULL;
    s->plug = plug;
    bufchain_init(&s->output_data);
    s->writable = false;             /* to start with */
    s->sending_oob = 0;
    s->frozen = false;
    s->localhost_only = true;
    s->pending_error = 0;
    s->parent = s->child = NULL;
    s->oobpending = false;
    s->outgoingeof = EOF_NO;
    s->incomingeof = false;
    s->listener = true;
    s->addr = listenaddr;
    s->s = -1;

    assert(listenaddr->superfamily == UNIX);

    /*
     * Open socket.
     */
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        s->error = strerror(errno);
        return &s->sock;
    }

    cloexec(fd);

    s->oobinline = false;

    memset(&u, '\0', sizeof(u));
    u.su.sun_family = AF_UNIX;
#if __GNUC__ >= 8
#   pragma GCC diagnostic push
#   pragma GCC diagnostic ignored "-Wstringop-truncation"
#endif // __GNUC__ >= 8
    strncpy(u.su.sun_path, listenaddr->hostname, sizeof(u.su.sun_path)-1);
#if __GNUC__ >= 8
#   pragma GCC diagnostic pop
#endif // __GNUC__ >= 8
    addr = &u;
    addrlen = sizeof(u.su);

    if (unlink(u.su.sun_path) < 0 && errno != ENOENT) {
        close(fd);
        s->error = strerror(errno);
        return &s->sock;
    }

    retcode = bind(fd, &addr->sa, addrlen);
    if (retcode < 0) {
        close(fd);
        s->error = strerror(errno);
        return &s->sock;
    }

    if (listen(fd, SOMAXCONN) < 0) {
        close(fd);
        s->error = strerror(errno);
        return &s->sock;
    }

    s->s = fd;

    uxsel_tell(s);
    add234(sktree, s);

    return &s->sock;
}
