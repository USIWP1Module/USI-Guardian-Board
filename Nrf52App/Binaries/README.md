These nRF52 firmware binaries are based on samples from Nordic Semiconductor ASA. 
See the LICENSE.txt in this directory, and for more background, see the README.md in the 
parent directory for this sample.

# nRF52 Softdevice
s132_nrf52_6.1.0_softdevice.hex

# nRF52 firmware binaries
nrf52832_WiFiSetupByBT.hex

# Combined nRF52 Softdevice and firmware binaries
nrf52832_softdevice_6.1.0_WiFiSetupByBT.hex

Follow below command to flash sotdevice and firmware
# Flash nRF52 softdevice and WiFi Setup By BT hex file
	nrfjprog -f nrf52 --program nrf52832_softdevice_6.1.0_WiFiSetupByBT.hex --sectorerase
	nrfjprog -f nrf52 --reset

