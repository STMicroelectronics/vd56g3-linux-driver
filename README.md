# ST VD56G3 Linux driver

## Supported Devices

This driver support the following devices : 

- VD56G3: Monochrome sensor
- VD66GY: Color sensor
- VD16GZ: RGB-NIR sensor

### Disclaimer

This driver only provides minimal support for the VD16GZ (RGB-NIR) device.

Currently, there is no RGB-NIR media bus code defined in the Linux V4L2 framework.
As a temporary workaround, when the VD16GZ sensor is detected, the driver advertises Y8/Y10 media bus codes to allow basic functionality.

Users should be aware that this does not accurately represent the sensor's actual pixel pattern and may affect image processing or color interpretation.


## Installation

### Driver

Compile the driver using the Makefile

```
make
```

Place it in the kernel modules folder.

```
sudo cp vd56g3.ko /lib/modules/$(uname -r)
```

Resolve modules dependencies.

```
sudo depmod -a
```

### Device tree

Compile the device tree overlay matching your platform and plugin board from the `dts` folder.

```
sudo dtc <device-tree>.dts -o /boot/firmware/overlays/<device-tree>.dtbo
```

Set the device tree overlay in your platform. This may differ from platform to platform. Please refer to your platform documentation.

This is how to set the device tree overlay for Raspberry Pi.

```
echo "dtoverlay=<device-tree>" | sudo tee -a /boot/firmware/config.txt
```

Finally, run `sudo reboot` to test your changes.
