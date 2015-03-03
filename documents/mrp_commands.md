# MRP commands for Talker

In order to configure streams and vlans using the MRP daemon, special instructions
have to be sent to mrpd on port `7500` (default port).

Please note that new lines, whitespace or comments in brackets do *NOT* exist in
the actual commands.
These are just inserted for clarification.

## Advertise a new stream: S++

Command:
```
S++:S=1122334455660123, (Stream ID, 8 bytes)
    A=91E0F0000e800000, (Destination MAC, 6 bytes + 2 padding)
    V=0002, (VLAN ID, 2 bytes)
    Z=00001500, (packet size as integer)
    I=00000001, (TSpec interval as integer)
    P=00300000, (priority and rank)
    L=00003900  (latency as integer)
```
* Stream ID has to be unique. It corresponds to the hexadecimal format without
  the leading "0x".
* The destination MAC has to be unique per advertised stream. Otherwise, an AVB
  switch will discard it.
* Priority and rank:
  * Bit field: `3 bits priority | 1 bit rank | 4 bits reserved`
  * Meaning of rank: rank = 0 -> emergency traffic, rank = 1 -> normal


## Unadvertise an existing stream: S--

Command:
```
S--:S=1122334455660123, Stream ID (8 bytes)
    A=91E0F0000e800000, Destination MAC (6 bytes + 2 padding)
    V=0002, (VLAN ID, 2 bytes)
    Z=00001500, (packet size as integer)
    I=00000001, (TSpec interval as integer)
    P=00300000, (priority and rank)
    L=00003900  (latency as integer)
```

The issued command is almost the same as the one for advertising a stream. The
only difference is `S--`.

## Wait for listener: S??

Command:
```
S??
```


# MRP commands for Listener

## Report domain status: S+D

Command:
```
S+D:
```

## Join Stream, send Ready: S+L

Command:
```
S+L:L=1122334455660123, (Stream ID, 8 bytes)
    D=2 (Substate)
```

## Leave stream: S-L

Command:
```
S-L:
```

# General notes

### IGB: transmit, receive queues and class priority

Class-dependent send interval:
* 125 usec for class A
* 250 usec for class B

The transmit queues are assigned as follows:
* tx-queue-0 for AVB Class A
* tx-queue-1 for AVB Class B
* tx-queue-2/3 are reserved for normal traffic

Incoming traffic is hardcoded to rx-queue-0. The reason is that the receiving side
for AVB traffic gathers the packets by the standard Linux sockets.

See: https://github.com/AVnu/Open-AVB/pull/3#issuecomment-18956741

Qav shaper is enabled by default when using igb_xmit(). It is set when igb_set_class_bandwidth() is called.

### Advertising multiple streams

* Unique stream ID
* Unique destination address
* mrp_register_domain has to be called separately for class A and class B
