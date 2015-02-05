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
* The Destination Mac has to be unique per advertised stream. Otherwise, the AVB
  switch will discard it.
* Priority and rank:
  * Bit field: `3 bits priority | 1 bit rank | 4 bits reserved`
  * Meaning of rank: rank = 0 -> emergency traffic, rank = 1 -> normal


## Unadvertise an exisiting stream: S--

Command:
```
S--:S=0x1122334455660123, Stream ID (8 bytes)
    A=0x91E0F0000e800000, Destination MAC (6 bytes + 2 padding)
    V=0002, (VLAN ID, 2 bytes)
    Z=00001500, (packet size as integer)
    I=00000001, (TSpec interval as integer)
    P=00300000, (priority and rank)
    L=00003900  (latency as integer)
```

The issued command is almost the same as the one for advertising a stream. The
only difference is `S--`.


# MRP commands for Listener

## Report domain status

Command:
```
S+D:
```

## Join stream: send Ready

Command:
```
S+L:L=1122334455660123, (Stream ID, 8 bytes)
    D=2 (Substate)
```

## Leave stream

Command:
```
S-L:
```
