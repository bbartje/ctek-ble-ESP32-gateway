# CTEK Battery Sense ESP32-gateway

Firmware voor een ESP32 of ESP32-C3 SuperMini die CTEK Battery Sense-monitors
na elkaar uitleest en met MQTT Discovery aan Home Assistant aanbiedt.

## Gebruiken

1. Installeer [PlatformIO Core](https://platformio.org/install/cli) of de
	PlatformIO-extensie voor VS Code.
2. Bouw en flash een standaard ESP32:

	```sh
	pio run -e esp32dev -t upload
	```

	Voor een ESP32-C3 SuperMini:

	```sh
	pio run -e esp32c3 -t upload
	```

	Selecteer zo nodig de juiste seriële poort met `--upload-port /dev/ttyUSB0`
	of `--upload-port /dev/ttyACM0`. De flashbare image staat in
	`.pio/build/<environment>/firmware.bin`; dit is de applicatie-image. De
	PlatformIO-upload flasht ook de benodigde bootloader- en partitie-images.
3. Verbind na het opstarten met Wi-Fi-AP `CTEK-Gateway-Setup`
	(wachtwoord `cteksetup`) en open `http://192.168.4.1`.
4. Vul Wi-Fi en de Home Assistant MQTT-broker in. Scan vervolgens de CTEK-
	apparaten en voeg elk apparaat toe met zijn eigen verbindingscode van exact
	16 tekens. De code staat doorgaans op het label of de barcode van de sensor.
5. Home Assistant ontdekt per monitor automatisch een voltage- en
	temperatuursensor. De gateway gebruikt maximaal acht monitors en verbindt
	telkens met één monitor tegelijk.

Na de eerste configuratie is de webpagina bereikbaar op het IP-adres dat de
router aan de gateway heeft toegekend. Bij een mislukte Wi-Fi-verbinding start
de configuratie-AP opnieuw.

## Protocolbevindingen

De meegeleverde CTEK-app is gedecompileerd voor de BLE/GATT-informatie. De
gateway gebruikt de CTEK-service `812CA8ED-A177-4D74-AEFA-70098C5416AF`, de
unlock-characteristiek `A1D67C01-FE3F-01BF-7D49-7DBDC43B02A6` en de
meetmeldingen op `A337B4EB-2544-4F94-87C9-F7732E5FE250`. De app maakt de
unlockwaarde met MD5 over een vaste 16-byte salt gevolgd door de ASCII-code.
BLE-advertenties dienen alleen om apparaten te ontdekken; voltage en
temperatuur vereisen een actieve GATT-verbinding, unlock-write,
voltage-notificaties en een aparte read van de temperatuurkarakteristiek
`B5634099-97C4-4F37-ADF3-771246E77754`. Na het inschakelen van notificaties
vraagt de gateway expliciet het laatste beschikbare meetrecord op.
Iedere meetrecord is drie bytes: een little-endian spanningswaarde gedeeld door
2048, gevolgd door een temperatuurbyte gedeeld door 2. Alleen apparaten met
BLE-adresprefix `2C:FC:E4` worden als CTEK Battery Sense aangeboden.

Protocolreferentie en aanvullende Home Assistant-integratie:
[bbartje/CTEK-BLE](https://github.com/bbartje/CTEK-BLE).

BLE-verbinding en sensormodel kunnen tussen hardware-/firmwareversies verschillen.
De protocolimplementatie is afgeleid uit de app-bytecode, maar pairing,
meetfrequentie en bereik moeten met de gebruikte sensorrevisie bevestigd worden.