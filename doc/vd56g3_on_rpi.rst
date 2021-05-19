.. _vd56g3_on_rpi:

======================
VD56G3 on Raspberry PI
======================

Two configurations based on different VD56G3 pcbs are described in this section :

    A. RPI + ST Raspberry to SPIDER adapter (PCB1637-00B) + ST FOX Demo OLGA80 (PCB1706B)
    B. RPI + ST FOX MiniPlugin OLGA80 (PCB1718A)

Required Hardware
=================

- Raspberry PI 3 or 4 + SD card + Power adapter
- Peripherals: Screen, Mouse, Keyboard, cables, etc.
- **Inverted Flex cable**
- Fox sensor; 2 options availables:
    
    A. ST Raspberry to SPIDER adapter (PCB1637-00B) + ST FOX Demo OLGA80 (PCB1706B)

        .. figure:: img/rpi_pcb1637b_pcb1706b.jpg
            :width: 60%
            :align: center

            RPI + rpi2spider board (PCB1637) + Fox Sensor (PCB1706B)

    B. ST FOX MiniPlugin OLGA80 (PCB1718A)

        .. figure:: img/rpi_pcb1718a.jpg
            :width: 60%
            :align: center

            RPI + Fox MiniPlugin (PCB1718A)


Step-by-step guide
==================

#. Install linux on the RPI

    #. Retrieve lastest `raspbian image <https://downloads.raspberrypi.org/raspios_armhf/images/raspios_armhf-2020-12-04/2020-12-02-raspios-buster-armhf.zip>`_ .
    #. Flash the downloaded .img file on the micro SD card

        - On Windows, you can use `Win32 Disk Imager <https://sourceforge.net/projects/win32diskimager/>`_
        - On Linux, you can use ``dd`` with the following command line::

            dd if=2020-12-02-raspios-buster-armhf.img of=/dev/<your_sdcard_device> bs=1M


#. Linux customization on first boot

    .. Warning::
        Depending of your network infrastructure, you may be required to setup a proxy.
    
    #. Follow the RPI wizard to configure: locales, default password, network connection, etc.

    #. By the end of the wizard, update your system.

    #. Install linux kernel headers and devicetree compiler::

        sudo apt install raspberrypi-kernel-headers device-tree-compiler

    #. You can also add interesting packages (will be used in next steps) ::

        sudo apt install vim qv4l2 yavta
        

#. Install last Fox driver 

    #. Retrieve the driver from the git repository::

        git clone ssh://gitolite@codex.cro.st.com/imgfox/linux/driver/vd56g3.git

    #. Build then install the driver as a module::

        cd vd56g3
        make
        sudo cp st-vd56g3.ko /lib/modules/`uname -r`/kernel/drivers/media/i2c/.
        sudo depmod -a


#. Update the Device Tree to describe the new Fox Setup

    .. Note ::
        The Raspberry ecosystem make big use of device tree overlays. On RPI, the overlays are precompiled and are located in the ``/boot/overlays`` folder.

    The device tree is used to describe hardware components of the system. It must be updated with hardware changes.
    
    In the present quickstart, two HW configurations are offered to connect the Fox sensor to the RPI. Because there is some differences with the pinout of the 2 solutions, it's important to use the correct device tree overlay.

    #. Get the overlay source:

        The `vd56g3_rpi2spider_overlay.dts`_  overlay file corresponding to the setup A. (RPI + PCB1637-00B + PCB1706B) is available in the ``doc`` subdirectory of the ``vd56g3`` git repository.

        The content of the ``vd56g3_rpi2spider_overlay.dts`` is reproduced below: 

        .. literalinclude:: ./vd56g3_rpi2spider_overlay.dts

        .. Important ::
            For the setup B. (FOX MiniPlugin - PCB1718A), the ``fox_ep`` endpoint must be updated to reflect the CSI lane inversion. See below the addition of the property ``lane-polarities = <1 1 1>;`` ::

                    fox_ep: endpoint {
                        clock-lanes = <0>;
                        data-lanes = <1 2>;
                        remote-endpoint = <&csi1_ep>;
                        clock-noncontinuous;
                        link-frequencies = 
                            /bits/ 64 <201000000 402000000 804000000>;
                        lane-polarities = <1 1 1>;
                    };

        .. _vd56g3_rpi2spider_overlay.dts: https://codex.cro.st.com/plugins/git/imgfox/linux/driver/vd56g3?a=blob_plain&h=0bc0515d738fea4f38ccc9c46f315c05acd9ad4e&f=doc%2Fvd56g3_rpi2spider_overlay.dts&noheader=1

    #. Overlay compilation and installation ::

        # Compile using the device tree compiler
        dtc -o vd56g3.dtbo vd56g3_rpi2spider_overlay.dts
        # Copy the vd56g3 compiled overlay
        sudo cp vd56g3.dtbo /boot/overlays/.
        # Enable vd56g3 overlay
        sudo sh -c 'echo "dtoverlay=vd56g3" >> /boot/config.txt'
    
    #. Finally the RPI must be rebooted. At startup the driver status can be checked in dmesg output::

            dmesg | grep vd56g3


#. Make the sensor stream ! 

    .. Note :: 
        On the RPI platform, the media pipeline is automatically setup by the system.

    Please see the :ref:`sensor_streaming` section for a more detailed description of streaming applications.

    An additional alternative based on a ``yavta fork`` is described below. This version make use of RPI GPU features for pixel format conversion and it enables to display the frames on the screen ::

        # Retrieve the yavta fork
        git clone https://github.com/6by9/yavta.git

        # Build It
        cd yavta/
        make

        # Stream on your display @1124x1364, 60FPS
        ./yavta --capture --size 1124x1364 --format SRGGB8 /dev/video0 --time-per-frame 1/60 --mmal