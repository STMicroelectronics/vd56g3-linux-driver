===============
Driver Features
===============

The VD56G3 is an I²C `V4L2 sub-device driver`_. All VD56G3 features are exposed through two V4L2 mechanisms: 

#. **V4L2 Operations**. The V4L2 Framework supports a wide variety of devices. Each V4L2 sub-device driver must declare the common operations actually supported by the device. These operations are related to the type of device: for example, camera-type devices generally implement all the same operations. See `V4L2 Supported Operations`_ section for the list of operations implemented in the VD56G3 driver.

#. **V4L2 Controls**. Controls are particular objects that describes/handles a control properties. In addition to `V4L2 standard controls`_, customs controls can be created for sensor's specific features. See `V4L2 Supported Controls`_ for the list of controls supported by the driver (standards and customs ones).

.. _V4L2 sub-device driver: https://www.kernel.org/doc/html/latest/driver-api/media/v4l2-subdev.html
.. _V4L2 framework: https://www.kernel.org/doc/html/latest/driver-api/media/v4l2-core.html
.. _V4L2 standard controls: https://www.kernel.org/doc/html/latest/driver-api/media/v4l2-controls.html


VD56G3 Supported Modes
======================
.. kernel-doc:: ./../src/st-vd56g3.c
    :doc: Supported Modes
    :no-header:

V4L2 Supported Operations
=========================
.. kernel-doc:: ./../src/st-vd56g3.c
    :doc: V4L2 supported Operations
    :no-header:
    :functions: vd56g3_s_stream, vd56g3_g_frame_interval, vd56g3_s_frame_interval, vd56g3_enum_mbus_code, vd56g3_get_fmt, vd56g3_set_fmt, vd56g3_enum_frame_size, vd56g3_enum_frame_interval

.. _vd56g3_supported_crtls:

V4L2 Supported Controls
=======================
.. kernel-doc:: ./../src/st-vd56g3.c
    :doc: V4L2 supported Controls
    :no-header:

Custom Controls IDs
-------------------
.. kernel-doc::  ./../src/st-vd56g3.c
   :snippets:  Custom-CIDs
   :language:  c

Custom Controls Definitions
---------------------------
.. kernel-doc:: ./../src/st-vd56g3.c
    :doc: GPIO0 mode selection Control
.. kernel-doc:: ./../src/st-vd56g3.c
    :doc: GPIO1 mode selection Control
.. kernel-doc:: ./../src/st-vd56g3.c
    :doc: GPIO2 mode selection Control
.. kernel-doc:: ./../src/st-vd56g3.c
    :doc: GPIO3 mode selection Control
.. kernel-doc:: ./../src/st-vd56g3.c
    :doc: GPIO4 mode selection Control
.. kernel-doc:: ./../src/st-vd56g3.c
    :doc: GPIO5 mode selection Control
.. kernel-doc:: ./../src/st-vd56g3.c
    :doc: GPIO6 mode selection Control
.. kernel-doc:: ./../src/st-vd56g3.c
    :doc: GPIO7 mode selection Control
.. kernel-doc:: ./../src/st-vd56g3.c
    :doc: Temperature Control
.. kernel-doc:: ./../src/st-vd56g3.c
    :doc: AE - Light level target (%)
.. kernel-doc:: ./../src/st-vd56g3.c
    :doc: AE - Control Mode
.. kernel-doc:: ./../src/st-vd56g3.c
    :doc: AE - Flicker Frequency
.. kernel-doc:: ./../src/st-vd56g3.c
    :doc: AE - Convergence step proportion (%)
.. kernel-doc:: ./../src/st-vd56g3.c
    :doc: AE - Convergence leak proportion (per mil)