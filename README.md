# ST-VD56G3 Linux driver

## Installation

### Driver

Compile the driver using the Makefile

```
make
```

Place it in the kernel modules folder.

```
sudo cp st-vd55g3.ko /lib/modules/$(uname -r)
```

Resolve modules dependencies.

```
sudo depmod -a
```

### Device tree

 Compile the device tree overlay matching your platform and plugin board from the `dts` folder.

```
sudo dtc <device-tree>.dts -o /boot/overlays/<device-tree>.dtbo
```

Set the device tree overlay in your platform. This may differ from platform to platform. Please refer to your platform documentation.

This is how to set the device tree overlay for Raspberry Pi.

```
echo "dtoverlay=<device-tree>" | sudo tee -a /boot/config.txt
```

Finally, run `sudo reboot` to test your changes.


## Usage

Run any video capture application to stream from the sensor.
QV4L2 testbench is a well featured test application.

```
sudo apt install qv4l2
qv4l2
```


## Features

Framesizes :

```
$ v4l2-ctl --list-framesizes=GREY
ioctl: VIDIOC_ENUM_FRAMESIZES
	Size: Discrete 1124x1364
	Size: Discrete 1120x1360
	Size: Discrete 1024x1280
	Size: Discrete 1024x768
	Size: Discrete 768x1024
	Size: Discrete 720x1280
	Size: Discrete 640x480
	Size: Discrete 480x640
	Size: Discrete 320x240
```

Controls :

```
$ v4l2-ctl -L

User Controls

                       exposure 0x00980911 (int)    : min=0 max=4261 step=1 default=1420 value=2742 flags=inactive, volatile
                horizontal_flip 0x00980914 (bool)   : default=0 value=0 flags=modify-layout
                  vertical_flip 0x00980915 (bool)   : default=0 value=0 flags=modify-layout
         temperature_in_celsius 0x00981920 (int)    : min=-1024 max=1023 step=1 default=0 value=0 flags=read-only, volatile
          ae_light_level_target 0x00981921 (int)    : min=0 max=100 step=1 default=30 value=30
       ae_convg_step_proportion 0x00981922 (int)    : min=0 max=256 step=1 default=140 value=140
       ae_convg_leak_proportion 0x00981923 (int)    : min=0 max=32768 step=1 default=11468 value=11468
      dark_calibration_pedestal 0x00981924 (int)    : min=0 max=255 step=1 default=64 value=64

Camera Controls

                  auto_exposure 0x009a0901 (menu)   : min=0 max=1 default=0 value=0 (Auto Mode) flags=update
                                0: Auto Mode
                                1: Manual Mode
             auto_exposure_bias 0x009a0913 (intmenu): min=0 max=16 default=8 value=8 (0 0x0)
                                0: -4000 (0xfffffffffffff060)
                                1: -3500 (0xfffffffffffff254)
                                2: -3000 (0xfffffffffffff448)
                                3: -2500 (0xfffffffffffff63c)
                                4: -2000 (0xfffffffffffff830)
                                5: -1500 (0xfffffffffffffa24)
                                6: -1000 (0xfffffffffffffc18)
                                7: -500 (0xfffffffffffffe0c)
                                8: 0 (0x0)
                                9: 500 (0x1f4)
                                10: 1000 (0x3e8)
                                11: 1500 (0x5dc)
                                12: 2000 (0x7d0)
                                13: 2500 (0x9c4)
                                14: 3000 (0xbb8)
                                15: 3500 (0xdac)
                                16: 4000 (0xfa0)
                        3a_lock 0x009a091b (bitmask): max=0x00000007 default=0x00000000 value=0

Flash Controls

                       led_mode 0x009c0901 (menu)   : min=0 max=1 default=0 value=0 (Off)
                                0: Off
                                1: Flash

Image Source Controls

              vertical_blanking 0x009e0901 (int)    : min=110 max=64767 step=1 default=1400 value=3568
            horizontal_blanking 0x009e0902 (int)    : min=212 max=212 step=1 default=212 value=212 flags=read-only
                  analogue_gain 0x009e0903 (int)    : min=0 max=28 step=1 default=0 value=0 flags=inactive, volatile

Image Processing Controls

                 link_frequency 0x009f0901 (intmenu): min=0 max=0 default=0 value=0 (402000000 0x17f60880) flags=read-only
                                0: 402000000 (0x17f60880)
                     pixel_rate 0x009f0902 (int64)  : min=160800000 max=160800000 step=1 default=160800000 value=160800000 flags=read-only
                   test_pattern 0x009f0903 (menu)   : min=0 max=7 default=0 value=0 (Disabled)
                                0: Disabled
                                1: Solid
                                2: Colorbar
                                3: Gradbar
                                4: Hgrey
                                5: Vgrey
                                6: Dgrey
                                7: PN28
                   digital_gain 0x009f0905 (int)    : min=256 max=2048 step=1 default=256 value=256 flags=inactive, volatile
