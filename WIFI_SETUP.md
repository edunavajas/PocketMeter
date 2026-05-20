# PocketMeter WiFi + HTTP - Guía de instalación

Esta guía deja PocketMeter funcionando con el flujo actual: firmware en la ESP32, daemon Python en el PC y panel web servido por la propia placa. El transporte principal de métricas ya no es BLE, sino **WiFi + HTTP**.

## Requisitos previos

- Tener la Waveshare ESP32-S3 AMOLED 2.16 conectada al PC por USB
- Tener instalado PlatformIO
- Conocer tu red WiFi y contraseña
- Tener `python3` disponible en el PC que ejecutará el daemon

## Instalación de PlatformIO

```bash
# Crear entorno virtual e instalar
python3 -m venv /tmp/pio-venv
/tmp/pio-venv/bin/pip install -U platformio
```

## Configuración del WiFi

Edita `firmware/src/config.h` y cambia estas líneas:

```cpp
#define WIFI_SSID "TuRedWiFi"
#define WIFI_PASSWORD "TuPassword"
```

Pon ahí tus credenciales reales antes de compilar.

## Compilar el firmware

```bash
cd /media/edu/trabajo/Aplica/Clawdmeter/firmware
/tmp/pio-venv/bin/pio run
```

La primera compilación tarda más porque descarga dependencias.

## Flashear a la ESP32-S3

```bash
/tmp/pio-venv/bin/pio run -t upload --upload-port /dev/ttyACM0
```

Si no está en `/dev/ttyACM0`, búscala con:

```bash
ls /dev/ttyACM* /dev/ttyUSB*
```

## Ver la IP de la ESP32

Abre el monitor serie:

```bash
/tmp/pio-venv/bin/pio device monitor --port /dev/ttyACM0
```

Busca la línea donde aparezca la IP del dispositivo:

```text
WiFi IP: 192.168.1.XXX
```

## Configurar la IP para el daemon

Guarda la IP en el fichero que usa el daemon:

```bash
mkdir -p ~/.config/claude-usage-monitor
printf '192.168.1.169\n' > ~/.config/claude-usage-monitor/esp-ip
```

La IP puede cambiar si tu router reasigna DHCP.

## Ejecutar el daemon actual

El daemon principal hoy es el script Python multi-provider:

```bash
cd /media/edu/trabajo/Aplica/Clawdmeter
python3 daemon/clawdmeter-daemon.py
```

Comportamiento esperado:

- **Claude** funciona si existe `~/.claude/.credentials.json`
- **Codex** saldrá como **Not configured** hasta ejecutar `codex login`
- Si luego configuras Codex, reinicia el daemon para que vuelva a comprobar credenciales

Para que Codex deje de salir como no configurado:

```bash
codex login
python3 daemon/clawdmeter-daemon.py
```

### Opción opcional: demo con datos simulados

Si solo quieres probar el flujo end-to-end:

```bash
cd /media/edu/trabajo/Aplica/Clawdmeter
./daemon/claude-usage-wifi-demo.sh
```

## Ver el panel web

Abre en tu navegador:

```text
http://192.168.1.169/
```

Verás un panel con:

- Estado de conexión WiFi
- Últimas métricas recibidas de Claude/Codex
- Uptime del dispositivo

Se actualiza automáticamente cada 5 segundos.

## Probar manualmente (sin daemon)

```bash
curl -X POST http://192.168.1.169/api/usage \
  -H "Content-Type: application/json" \
  -d '{"s":45,"sr":120,"w":28,"wr":7200,"st":"allowed","ok":true}'

curl http://192.168.1.169/api/status
curl http://192.168.1.169/api/health
```

## Funcionamiento de botones físicos

Los botones físicos siguen usando BLE HID para atajos del host:

- **Izquierdo (GPIO 0)**: mantiene pulsado para enviar Space
- **Derecho (GPIO 18)**: envía Shift+Tab
- **Central (PWR)**: cambia entre pantallas (Splash ↔ Usage ↔ Network)

## Solución de problemas

### No se conecta al WiFi

- Verifica que las credenciales en `config.h` sean correctas
- Asegúrate de que es una red 2.4GHz
- Revisa el monitor serie para ver errores

### El daemon no encuentra la IP

- Verifica que existe `~/.config/claude-usage-monitor/esp-ip`
- Comprueba que puedes hacer ping a la IP de la ESP32

### No se muestran datos en la pantalla

- Verifica que el daemon está ejecutándose
- Prueba el envío manual con `curl`
- Revisa la salida del daemon

### La compilación falla

- Asegúrate de usar una versión reciente de PlatformIO
- Borra `.pio/build` y recompila

## Archivos clave

```text
Clawdmeter/
├── firmware/
│   ├── src/
│   │   ├── config.h              # Credenciales WiFi
│   │   ├── wifi_manager.h/cpp    # Gestión WiFi
│   │   ├── web_server.h/cpp      # Servidor HTTP y panel web
│   │   ├── main.cpp              # Inicializa WiFi + web
│   │   ├── ui.h/cpp              # UI en pantalla
│   │   └── ble.h/cpp             # BLE HID para botones físicos
│   └── platformio.ini            # Configuración de build
└── daemon/
    ├── clawdmeter-daemon.py      # Daemon principal actual (Claude + Codex)
    ├── claude-usage-wifi.sh      # Script WiFi antiguo / alternativo
    └── claude-usage-wifi-demo.sh # Demo con datos simulados
```

## Notas

- Las credenciales WiFi siguen hardcodeadas en `firmware/src/config.h`
- El panel web es sencillo, pero útil para comprobar IP, señal y estado de providers
- El BLE HID sigue funcionando para los botones físicos
- Si cambias de red WiFi, debes recompilar y reflashear
- La carpeta local puede seguir llamándose `Clawdmeter`, aunque el branding visible sea **PocketMeter**
