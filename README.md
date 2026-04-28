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

License
-------

RMR and BRMR are licensed under GPL-2.0-or-later. See [`COPYING`](./COPYING)
for the full license text.
