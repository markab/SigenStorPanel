# SigenStorPanel

Firmware for a 10.1" wall panel showing live solar, battery, home and grid figures
from a Sigenergy SigenStor system. It runs on the
**Elecrow CrowPanel Advanced 10.1" ESP32-P4 HMI** (1024×600 IPS touchscreen).

It is the big sibling of [SigenStorPuck](https://github.com/markab/SigenStorPuck),
which shows the same data on small round and landscape AMOLED boards, and it shares
that project's data layer. It reads from a SigenStor Display server, straight from the
plant over Modbus TCP, or from Home Assistant.

**Status: early development.** Nothing here runs on hardware yet.
