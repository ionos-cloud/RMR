RMR and BRMR
======================================================================

RMR (Reliable Multicast over RTRS) is a kernel module that provides active-active block-level replication over RDMA. It guarantees delivery of IO to a group of storage nodes and handles resynchronization of data directly between storage nodes without involving the compute client.

BRMR (Block device over RMR) sits on top of RMR and exposes a standard Linux block device (`/dev/brmrX`) backed by an RMR pool. Together, RMR and BRMR provide a single-hop replication and resynchronization solution for RDMA-connected storage clusters.

Project Status: In-development
------------------------------

RMR is under active development and is **not ready for production use**.
Expect:

- Bugs, crashes, and data-loss scenarios that have not yet been fixed
- API, sysfs interface, and on-disk format changes without migration
- Code paths marked `TODO` / `FIXME` that are pending refactor or correction

Not suitable for: production workloads, or storing data you care about.

Building from source
--------------------

RMR and BRMR are out-of-tree Linux kernel modules.

- Supported kernel version: v6.12. The
  [`linux-6.12.y`](https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/log/?h=linux-6.12.y)
  stable branch is recommended.
- Install kernel headers matching the target kernel
  (`linux-headers-$(uname -r)` on Debian/Ubuntu).
- The following RTRS config options must be enabled in the kernel:

  ```
  CONFIG_INFINIBAND_RTRS=m
  CONFIG_INFINIBAND_RTRS_CLIENT=m
  CONFIG_INFINIBAND_RTRS_SERVER=m
  ```

- Run `make` in the source-code repository root.
- Load modules in dependency order: `rtrs-*`, then `rmr-*`, then `brmr-*`.
  Unload in reverse order.

See the User/admin guide to RMR cluster management for a full two-node
bring-up.

License
-------

RMR and BRMR are licensed under GPL-2.0-or-later. See [`COPYING`](./COPYING)
for the full license text.
