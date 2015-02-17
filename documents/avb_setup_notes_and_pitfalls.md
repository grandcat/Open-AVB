## Kernel Boot Options

`intel_iommu` boot option *must not* be set. Otherwise, trying to send a packet
with `igb_xmit()` causes a TX hang.
This is the case for the igb_avb kernel driver version 5.2.9.4_AVB.


## IGB Kernel Module for Ethernet Card I210

For more recent kernels like Linux 3.13.*, the network driver for the I210 Ethernet
card from the master branch of the AVnu project fails compiling.

Use the branch `open-avb-next` instead (2015-02-17):
https://github.com/AVnu/Open-AVB/tree/open-avb-next


## Switched Network Setup with Netgear Switch GS724T

To test AVB in a switched network, we use a Netgear switch with the following
properties:

* Model Name: GS724Tv4
* Boot Version: B1.0.0.4
* Software Version: 6.3.1.4

On this switch, VLAN 2 is reserved for VoIP by default. Therfore, attempting to
reserve a stream with MSRP for this VLAN (default is 2 for AVB) is denied by the
switch.

Eliminating this and some other problems with regard to proper AVB support:

* Enable tagging for all ports on VLAN2. Somehow, this setting didn't work
  with the web interface. The attached configuration file will do it.
* Disable `MSRP talker Pruning`, because it is not supported by AVnu network stack
  right now.

For proper setup, just apply the attached configuration file to the switch.
Please ensure that the properties mentioned above match.
