# TCP Gender Changer Daemon

This is the MVP/skateboard version of a handy tool I needed.
For more information on the concept, see [wikipedia article](https://en.wikipedia.org/wiki/TCP_Gender_Changer). The specific use case it was written for was to forward SSH connections to a machine behind NAT, with *maximum throughput* and *minimal latency*. 

**Warning: This is not a VPN tunnel. It should only be used to tunnel protocols that are in themselves secure!**

Other implementations of the same concept:
 * [tgcd](http://tgcd.sourceforge.net/) (Didn't support binding to a specific interface)
 * [revinetd](http://revinetd.sourceforge.net/) (Could crash when faced with reset connections)

### Wishlist
* IPv6 support
* Host name lookup support (only IPv4 addrs for now)
* Docs
* Logging
* Dropping of privileges

### Building
1. Make sure you have ```cmake```
2. Do: ```cmake . && make```
   ```
