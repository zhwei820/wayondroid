/* `--fake-net`: an emulated loopback + one link for init sessions (netemu.c). */
#pragma once
#include <ucontext.h>
#include "dispatch.h"

/* The fake link's index (lo is 1) and name: a phone's Wi-Fi interface, as
 * the guest poses as a phone on Wi-Fi (rootless_prep has Android report it
 * so). Its sysfs entries in netemu.c spell the name out too. */
#define TAWCROOT_NETEMU_ETH_INDEX 2
#define TAWCROOT_NETEMU_LINK "wlan0"

/* After --init: reset the link table, populate the fake /sys/class/net
 * and /proc/sys/net and bind them, and stack the traps netemu needs. */
long tawcroot_netemu_setup(void);

/* socket(AF_NETLINK, *, NETLINK_ROUTE) in a fake-net session: a stub
 * fd. TAWC_ENOSYS for every other socket. */
long tawcroot_netemu_socket(long domain, long type, long proto);

/* bind/connect/sendto/sendmsg/recvfrom/recvmsg/read/write/getsockname/
 * getpeername/getsockopt on a netlink stub (and bind to the fake
 * address), else TAWC_ENOSYS. */
long tawcroot_netemu_call(const tawcroot_syscall_args *a, ucontext_t *uc);

/* SIOC* interface ioctls, else TAWC_ENOSYS. */
long tawcroot_netemu_ioctl(int fd, unsigned cmd, long arg);

/* Lazy handlers (trapped from setup on). */
long tawcroot_netemu_setsockopt(const tawcroot_syscall_args *a, ucontext_t *uc);
long tawcroot_netemu_read(const tawcroot_syscall_args *a, ucontext_t *uc);
