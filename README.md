# Open re-implementation of the Quansheng UV-K5/K6/5R v2.1.27 firmware a.k.a NACK!

I added and change a few things fell free to read over the main features and manual.

This repository is a merge of [OneOfEleven custom firmware](https://github.com/OneOfEleven/uv-k5-firmware-custom) with [fagci spectrum analizer](https://github.com/fagci/uv-k5-firmware-fagci-mod/tree/refactor) plus my few changes.<br>
All is a cloned and customized version of DualTachyon's open firmware found [here](https://github.com/DualTachyon/uv-k5-firmware) ... a cool achievement !

> [!TIP]
> There is a work done by others on forks of this repository. I encourage you to take a look at those too. [SEE HERE](https://github.com/egzumer/uv-k5-firmware-custom/discussions/485)


> [!WARNING]  
> Use this firmware at your own risk (entirely). There is absolutely no guarantee that it will work in any way shape or form on your radio(s), it may even brick your radio(s), in which case, you'd need to buy another radio.
Anyway, have fun. I would also recommend clearing the eeprom :)

## Table of Contents

* [Main Features](#main-features)
* [Manual](#manual)
* [Radio Performance](#radio-performance)
* [Building](#building)
* [Credits](#credits)
* [Other sources of information](#other-sources-of-information)
* [License](#license)

## Main features:

Nallo's Ack firmware or "NACK!" is (an excuse to mess with my radios, honestly xD) a firmware for the Quansheng UV5K centered on automatic transmissions, acknowledgment of reception and knowing the state of up to 4 other radios (there isn't a softlock making it impossible to use more, however it isn't supported as it might not work as intended).

I have used 99.53% of available memory (61148/61439 bytes), maybe I could have squeezed in another small function but ig this will do.

* What's new on NACK? (More info in the manual)
  * SqlAck -> Send a confirmation when the radio's squelch closes after being opened.
  * SNR REQ -> Request a Signal to Noise Ratio test from another radio.
  * Emergency -> Allows you to send an "emergency type packet" which will make the other radios sound.
  * Beacon -> Automatically transmit a tone every 3-15-30 seconds.
  * RangeN -> RangeNet (I couldn't come up with a better name, sorry ;-;) allows you to automatically get reports of signals from other radios and beeps when one doesn't answer, allowing you to know when one has left each other's range.
 

* I also moved the Sql option to number one on the menu.
* And I also upped the time it takes for the menu to close to 3 minutes and 5 minutes in the special menus.

- What have I deleted?
  * Scan Ranges
  * Copy chanel to VFO
  * MDC option in roger
  * SOS flashglight (seriously, who could possibly need this in an emergency?) and I also removed the flashing light when you manually turn it on, it was very annoying.
  * DTMF Calling and related functions
  * NOAA (as my software uses the spectrum analyser)
  * Aircopy (just say the damn frecuency??)
  * Voice (I had to take out that small lady who leaves inside the quansheng and tells you everything)
  * Alarm (again, why would you use this??)
  * Tx1750 (this one might have been usefull but ig you can always wistle xD)

 
  
* many of OneOfEleven mods:
   * AM fix, huge improvement in reception quality
   * long press buttons functions replicating F+ action
   * fast scanning
   * channel name editing in the menu
   * channel name + frequency display option
   * shortcut for scan-list assignment (long press `5 NOAA`)
   * scan-list toggle (long press `* Scan` while scanning)
   * configurable button function selectable from menu
   * battery percentage/voltage on status bar, selectable from menu
   * longer backlight times
   * mic bar
   * RSSI s-meter
   * more frequency steps
   * squelch more sensitive
* fagci spectrum analyzer (**F+5** to turn on)
* some other mods introduced by me:
   * SSB demodulation (adopted from fagci)
   * backlight dimming
   * battery voltage calibration from menu
   * better battery percentage calculation, selectable for 1600mAh or 2200mAh
   * more configurable button functions
   * long press MENU as another configurable button
   * better DCS/CTCSS scanning in the menu (`* SCAN` while in RX DCS/CTCSS menu item)
   * Piotr022 style s-meter
   * restore initial freq/channel when scanning stopped with EXIT, remember last found transmission with MENU button
   * reordered and renamed menu entries
   * LCD interference crash fix
   * many others...

 ## Manual

Up to date manual is **NOT** available in the [Wiki section](https://github.com/egzumer/uv-k5-firmware-custom/wiki) as i added my own functions, but fear not, I'll explain them in a detailed way here.

It's worth noting that this radio doesn't tx in ssb or am.

Menu number 49, "ID", lets you set your ID for this radio. Think about it as your radio's name that will be broadcast to other compatible radios. It can (and it's preferred to) be 4 characters long and can contain numbers. Example: AB12. Press the menu key to advance to the next character and exit to go to the previous one. If you are on the first one and press exit, you will leave without applying your changes. If you are on the last one and press menu, you will save the ID.

Menu number 50, "SqlAck", when enabled, if the sql is opened for more than 700ms it'll wait for it to close and then wait between 3 to 5s and tx either a tone or your ID. It has different modes:

* Mute: the radio will NOT tx any automatic acks
* OFF: the radio will not tx when the sql is closed
* Beep: the radio will tx two tones (70ms on, 45ms off, times two) when the squelch is closed
* ID: the radio will tx your ID when the squelch is closed

This function is useful cause it lets you know when you are hitting that radio's squelch (that can be later modified so it only lets through transmissions you can understand). If the channel is occupied it will keep trying for a maximum of 10s at random intervals. After one ack has been correctly transmitted, none will follow for 20s, even if the squelch is closed again.




How does everything work? I'm glad you asked:

## Radio performance

Please note that the Quansheng UV-Kx radios are not professional quality transceivers, their
performance is strictly limited. The RX front end has no track-tuned band pass filtering
at all, and so are wide band/wide open to any and all signals over a large frequency range.

Using the radio in high intensity RF environments will most likely make reception anything but
easy (AM mode will suffer far more than FM ever will), the receiver simply doesn't have a
great dynamic range, which results in distorted AM audio with stronger RX'ed signals.
There is nothing more anyone can do in firmware/software to improve that, once the RX gain
adjustment I do (AM fix) reaches the hardwares limit, your AM RX audio will be all but
non-existent (just like Quansheng's firmware).
On the other hand, FM RX audio will/should be fine.

But, they are nice toys for the price, fun to play with.



## Building

### Github Codespace build method

This is the least demanding option as you don't have to install enything on your computer. All you need is Github account.

1. Go to https://github.com/egzumer/uv-k5-firmware-custom
1. Click green `Code` button
1. Change tab from `Local` to `Codespace`
1. Click green `Create codespace on main` button

<img src="images/codespace1.png" width=700 />

5. Open `Makefile`
1. Edit build options, save `Makefile` changes
1. Run `./compile-with-docker.sh` in terminal window
1. Open folder `compiled-firmware`
1. Right click `firmware.packed.bin`
1. Click `Download`, now you should have a firmware on your computer that you can proceed to flash on your radio. You can use [online flasher](https://egzumer.github.io/uvtools)

<img src="images/codespace2.png" width=700 />

### Docker build method

If you have docker installed you can use [compile-with-docker.bat](./compile-with-docker.bat) (Windows) or [compile-with-docker.sh](./compile-with-docker.sh) (Linux/Mac), the output files are created in `compiled-firmware` folder. This method gives significantly smaller binaries, I've seen differences up to 1kb, so it can fit more functionalities this way. The challenge can be (or not) installing docker itself.

### Windows environment build method

1. Open windows command line and run:
    ```
    winget install -e -h git.git Python.Python.3.8 GnuWin32.Make
    winget install -e -h Arm.GnuArmEmbeddedToolchain -v "10 2021.10"
    ```
2. Close command line, open a new one and run:
    ```
    pip install --user --upgrade pip
    pip install crcmod
    mkdir c:\projects & cd /D c:/projects
    git clone https://github.com/egzumer/uv-k5-firmware-custom.git
    ```
3. From now on you can build the firmware by going to `c:\projects\uv-k5-firmware-custom` and running `win_make.bat` or by running a command line:
    ```
    cd /D c:\projects\uv-k5-firmware-custom
    win_make.bat
    ```
4. To reset the repository and pull new changes run (!!! it will delete all your changes !!!):
    ```
    cd /D c:\projects\uv-k5-firmware-custom
    git reset --hard & git clean -fd & git pull
    ```

I've left some notes in the win_make.bat file to maybe help with stuff.

## Credits

Many thanks to various people on Telegram for putting up with me during this effort and helping:

* [OneOfEleven](https://github.com/OneOfEleven)
* [DualTachyon](https://github.com/DualTachyon)
* [Mikhail](https://github.com/fagci)
* [Andrej](https://github.com/Tunas1337)
* [Manuel](https://github.com/manujedi)
* @wagner
* @Lohtse Shar
* [@Matoz](https://github.com/spm81)
* @Davide
* @Ismo OH2FTG
* [OneOfEleven](https://github.com/OneOfEleven)
* @d1ced95
* and others I forget

Nack was made by Nallo with help of chatgpt.

## Other sources of information

[ludwich66 - Quansheng UV-K5 Wiki](https://github.com/ludwich66/Quansheng_UV-K5_Wiki/wiki)<br>
[amnemonic - tools and sources of information](https://github.com/amnemonic/Quansheng_UV-K5_Firmware)

## License

Copyright 2023 Dual Tachyon
https://github.com/DualTachyon

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.


Video showing the AM fix working ..

<video src="/images/AM_fix.mp4"></video>

<video src="https://github.com/OneOfEleven/uv-k5-firmware-custom/assets/51590168/2a3a9cdc-97da-4966-bf0d-1ce6ad09779c"></video>
