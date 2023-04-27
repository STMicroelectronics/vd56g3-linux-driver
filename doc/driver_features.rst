===============
Driver Features
===============

The VD56G3 linux driver support both :

    - VD56G3 - Monochrome version
    - VD66GY - RGB Bayer version


Supported Modes
===============
.. kernel-doc:: ./../src/st-vd56g3.c
    :doc: Supported Modes
    :no-header:


Supported Output
================

The driver supports 8bits and 10bits outputs in both Monochrome and RGB variants.

Media Bus codes for Mono variant :

- `MEDIA_BUS_FMT_Y8_1X8 <https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/subdev-formats.html?highlight=media_bus_fmt_y8_1x8#packed-yuv-formats>`_
- `MEDIA_BUS_FMT_Y10_1X10 <https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/subdev-formats.html?highlight=media_bus_fmt_y10_1x10#packed-yuv-formats>`_

Media Bus codes for RGB variant :

- `MEDIA_BUS_FMT_SGRBG8_1X8 <https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/subdev-formats.html?highlight=media_bus_fmt_sgrbg8_1x8#bayer-formats>`_
- `MEDIA_BUS_FMT_SGRBG10_1X10 <https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/subdev-formats.html?highlight=media_bus_fmt_sgrbg10_1x10#bayer-formats>`_
- Or equivalent bayer format depending of the H/V flip variations (RGGB, BGGR, GBRG)


Supported Controls
==================

The driver exposes and supports 20 V4L2 Controls :

- 14 standard controls belonging to existing V4L2 control classes
- 6 vd56g3-specific controls, defined for custom features of the sensor

================================= ========================================================================================
 **Standard V4L2 Controls**        **Description [Related V4L2 Control Class]**
================================= ========================================================================================
 ``V4L2_CID_PIXEL_RATE``           Pixel Rate [`Image Process control class`_]
 ``V4L2_CID_LINK_FREQ``            Link Frequency [`Image Process control class`_]
 ``V4L2_CID_HBLANK``               Horizontal Blanking [`Image Source control class`_]
 ``V4L2_CID_VBLANK``               Vertical Blanking [`Image Source control class`_]
 ``V4L2_CID_VFLIP``                Vertical Flip [`User control class`_]
 ``V4L2_CID_HFLIP``                Horizontal Flip [`User control class`_]
 ``V4L2_CID_TEST_PATTERN``         Test Pattern [`Image Process control class`_]
 ``V4L2_CID_EXPOSURE_AUTO``        Auto Exposure [`Camera control class`_]
 ``V4L2_CID_3A_LOCK``              Lock/Unlock Auto Exposure settings [`Camera control class`_]
 ``V4L2_CID_AUTO_EXPOSURE_BIAS``   Auto Exposure Compensation [`Camera control class`_]
 ``V4L2_CID_ANALOGUE_GAIN``        Analogue Gain (only available in Manual exposure mode) [`Image Source control class`_]
 ``V4L2_CID_DIGITAL_GAIN``         Digital Gain (only available in Manual exposure mode) [`Image Process control class`_]
 ``V4L2_CID_EXPOSURE``             Exposure (only available in Manual exposure mode) [`User control class`_]
 ``V4L2_CID_FLASH_LED_MODE``       Enable/Disable LED (when 'st,leds' property defined in DT)  [`Flash control class`_]
================================= ========================================================================================


=================================== =====================================
 **VD56G3 Custom V4L2 Controls**     **Description**
=================================== =====================================
 ``V4L2_CID_TEMPERATURE``            `Temperature Control`_
 ``V4L2_CID_AE_TARGET_PERCENTAGE``   `AE - Light level target (%)`_
 ``V4L2_CID_AE_STEP_PROPORTION``     `AE - Convergence step proportion`_
 ``V4L2_CID_AE_LEAK_PROPORTION``     `AE - Convergence leak proportion`_
 ``V4L2_CID_DARKCAL_PEDESTAL``       `Dark Calibration Pedestal`_
 ``V4L2_CID_SLAVE_MODE``             `VT Slave Mode Control`_
=================================== =====================================

.. _User control class: https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/control.html
.. _Camera control class: https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/ext-ctrls-camera.html
.. _Image Source control class: https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/ext-ctrls-image-source.html
.. _Image Process control class: https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/ext-ctrls-image-process.html
.. _Flash control class: https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/ext-ctrls-flash.html

Custom Controls IDs
-------------------

In order to be able to programmatically interact with these VD56G3-custom controls, their IDs definition are reproduced below.

.. kernel-doc::  ./../src/st-vd56g3.c
   :snippets:  Custom-CIDs
   :language:  c

Custom Controls Definitions
---------------------------
.. kernel-doc:: ./../src/st-vd56g3.c
    :doc: Temperature Control
.. kernel-doc:: ./../src/st-vd56g3.c
    :doc: AE - Light level target (%)
.. kernel-doc:: ./../src/st-vd56g3.c
    :doc: AE - Convergence step proportion
.. kernel-doc:: ./../src/st-vd56g3.c
    :doc: AE - Convergence leak proportion
.. kernel-doc:: ./../src/st-vd56g3.c
    :doc: Dark Calibration Pedestal
.. kernel-doc:: ./../src/st-vd56g3.c
    :doc: VT Slave Mode Control