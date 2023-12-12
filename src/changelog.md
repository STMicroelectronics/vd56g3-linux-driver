# Changelog

## [0.11.0] - 2023-12-12
- Add Fox Fastboot (cut3) support
- Proper handling of Mono/RGB variants through compatible string
- Status Linues are now disabled by default
- Default V4L2_CID_FLASH_LED_MODE ctrl to OFF
- Better handling of maximum framerate (now resolution-dependant)
- Proper split between native and default (recommended) resolution
- Drop 240x320 resolution
- Improved backward compatibility (down to k4.9)

## [0.10.0] - 2023-05-29
- Add support of kernel 6.1
- Backward compatibility with all LTS kernels (down to k4.14)

## [0.9.0] - 2023-04-27
- Support of variable framerate (instead of discrete)
- Drop Cut1 support
- Bump to FMW patch 2.28
- Enable RGB support
- Power Management runtime support
- Rework of V4L2 ctrls (enable auto-cluster when possible)
- Support of Led Strobe mode based on "st,leds" devicetree property + V4L2_CID_FLASH_LED_MODE control
- Support of Vsync Output based on "st,out-sync" devicetree property
- Support of VT Slave Mode input based on "st,in-sync" devicetree property + V4L2_CID_SLAVE_MODE control
- Support single or dual MIPI data lane devicetree configuration

## [0.8.0] - 2021-11-16
- Add Changelog

## [0.6.0] - 2021-11-16
- Add Cut2.0 support
- Enable VTRam Patch for Cut2.0 (VT patch 17)
- Bump to FMW patch 2.20
- [FIX] Force EXP_COARSE_INTG_MARGIN value to ensure proper Auto Expo behavior in low light conditions
- Add new V4L2 Controls to handle Auto Expo parameters

## [0.5.0] - 2021-10-28
- Update for Kernel 5.14

## [0.4.0] - 2019-12-13
- Bump to FW patch 1.33

## [0.3.0] - 2019-11-25
- Update supported frame rates
- Add gpio control
- Add formats with isl ouput
- Add temperature control

## [0.2.0] - 2019-10-17
- Fix bad READOUT_CTRL address in documentation
- add 50 fps support

## [0.1.0] - 2019-09-26
- Initial release