# PocketMeter

PocketMeter es un medidor físico de uso para herramientas de IA: un pequeño panel AMOLED con ESP32-S3 que se queda en el escritorio y muestra, en tiempo real, cómo van tus límites, créditos o estado de distintos proveedores.

Este repo empezó como un fork de **Clawdmeter**: <https://github.com/HermannBjorgvin/Clawdmeter>. La base original estaba centrada en Claude y en transporte BLE; este fork ha cambiado bastante la arquitectura, la interfaz y el alcance del proyecto.

Ahora la ruta principal es **WiFi + HTTP + daemon local en Python**. BLE sigue existiendo, pero en el estado actual del repo se usa para atajos HID de teclado, no como transporte principal de datos de uso.

<!-- TODO: add demo video -->
<!-- TODO: add final device photos -->

| Medidor de uso | Pantalla de mascota |
| :---: | :---: |
| ![PocketMeter showing usage](assets/demo.jpeg) | ![PocketMeter splash animation](assets/demo.gif) |

## Qué hace

PocketMeter junta tres piezas:

| Pieza | Dónde corre | Función |
| --- | --- | --- |
| Firmware ESP32 | Waveshare ESP32-S3-Touch-AMOLED-2.16 | Se conecta a WiFi, renderiza la UI, recibe datos por HTTP y expone una API local. |
| Daemon Python | Tu PC | Lee credenciales/configuración de proveedores, consulta APIs y envía el payload al ESP32. |
| Panel web | Tu PC, `http://localhost:8080` | Configura proveedores, API keys, visibilidad, mascota/splash y proxy hacia el ESP32. |

El objetivo ya no es solo ver Claude Code. El daemon está preparado para trabajar con varios proveedores, dependiendo de tus credenciales y de lo que cada API exponga.

| Proveedor | Configuración actual |
| --- | --- |
| Claude | Credenciales de Claude CLI / Claude Code. |
| Codex | `codex login`. |
| Gemini | OAuth local de Gemini CLI. |
| Copilot | GitHub Copilot vía `gh auth` o token compatible. |
| Grok | Credenciales locales de Grok CLI. |
| OpenAI | `OPENAI_API_KEY`, también configurable desde el panel web. |
| DeepSeek | `DEEPSEEK_API_KEY`, también configurable desde el panel web. |
| Windsurf | Lectura local de datos de Windsurf. |
| Cursor | `CURSOR_SESSION_TOKEN`, también configurable desde el panel web. |
| Kimi / Moonshot | `KIMI_API_KEY` o `MOONSHOT_API_KEY`, también configurable desde el panel web. |

Algunos proveedores muestran porcentajes de uso. Otros muestran saldo, plan, crédito o estado disponible según la API. Si un proveedor no está configurado, aparece como pendiente en lugar de romper el flujo.

## Hardware

El firmware está adaptado para la placa **Waveshare ESP32-S3-Touch-AMOLED-2.16**:

| Componente | Detalle |
| --- | --- |
| Pantalla | CO5300 AMOLED 480 x 480 por QSPI. |
| Touch | CST9220 por I2C. |
| PMU | AXP2101 para batería, USB/VBUS y botón central. |
| IMU | QMI8658 para auto-rotación. |
| Botones | GPIO 0, GPIO 18 y PKEY de AXP2101. |
| Alimentación | USB-C y batería Li-Po opcional. |

La pantalla usa rotación por CPU porque el CO5300 no permite intercambio de ejes por MADCTL. Esto ya está resuelto en el firmware.

## Flujo rápido

Este es el camino feliz para levantar el proyecto desde cero.

1. Configura el WiFi del ESP32 en `firmware/src/config.h`.
2. Compila y flashea el firmware con PlatformIO.
3. Arranca el ESP32 y confirma que responde por `pocketmeter.local` o por IP.
4. Arranca el panel web local en tu PC.
5. Arranca el daemon multi-proveedor.
6. Configura proveedores y API keys desde CLI, variables de entorno o panel web.
7. Verifica que aparecen datos en el panel y en la pantalla física.

## Requisitos

| Necesitas | Para qué |
| --- | --- |
| Python 3 | Ejecutar daemon y panel web local. |
| `curl` | Proxy robusto del panel web hacia el ESP32. |
| PlatformIO | Compilar y flashear firmware. |
| USB-C | Flash y logs serie. |
| CLI de cada proveedor | Solo si quieres activar ese proveedor. |
| Pillow opcional | Conversión de mascotas de PetDex desde el panel web. |

No hay `requirements.txt` en el repo en este momento. La ruta principal del daemon usa librería estándar de Python; Pillow solo es necesario para convertir sprites de PetDex.

## 1. Configura WiFi

Edita `firmware/src/config.h` antes de compilar:

```cpp
#define WIFI_SSID "TuRedWiFi"
#define WIFI_PASSWORD "TuPassword"
#define WIFI_HOSTNAME "pocketmeter"
```

No publiques credenciales reales en commits. En este proyecto el WiFi está compilado dentro del firmware.

## 2. Flashea el firmware

Linux:

```bash
pio run -d firmware
pio run -d firmware -t upload --upload-port /dev/ttyACM0
```

También puedes usar el helper:

```bash
./flash.sh /dev/ttyACM0
```

macOS:

```bash
./flash-mac.sh
# o indicando puerto
./flash-mac.sh /dev/cu.usbmodem1101
```

Si quieres ver logs:

```bash
pio device monitor -p /dev/ttyACM0 -b 115200
```

Busca algo parecido a:

```text
WiFi: connected, IP=...
mDNS: responder started at http://pocketmeter.local/
```

## 3. Arranca el panel web

El panel actual se sirve desde el PC, no desde el ESP32.

```bash
cd daemon
./web-server.sh
```

Abre:

```text
http://localhost:8080
```

Si tu ESP32 no está en `192.168.1.180`, indica la IP o hostname:

```bash
ESP32_HOST=pocketmeter.local ./web-server.sh
# o
ESP32_HOST=192.168.1.180 ./web-server.sh
```

Para dejarlo en segundo plano:

```bash
./web-server.sh --daemon
```

## 4. Arranca el daemon

Desde la raíz del repo:

```bash
python3 daemon/clawdmeter-daemon.py
```

El daemon intenta descubrir el ESP32 en este orden:

1. `POCKETMETER_ESP_HOST` si está definido.
2. Cache en `~/.config/claude-usage-monitor/esp-ip`.
3. Hostnames `pocketmeter.local`, `pocketmeter`, `clawdmeter.local`, `clawdmeter`.
4. Escaneo de la subred local validando `GET /api/status`.

Si todo va bien, verás logs de descubrimiento, estado de proveedores y envíos periódicos al ESP32.

## 5. Configura proveedores

Hay dos familias de proveedores.

### Proveedores con login local

| Proveedor | Acción típica |
| --- | --- |
| Claude | Inicia sesión con Claude Code / Claude CLI. |
| Codex | `codex login` |
| Gemini | Ejecuta `gemini` y completa OAuth si lo pide. |
| Copilot | `gh auth login --scopes copilot` o token compatible. |
| Grok | Login local de Grok CLI. |
| Windsurf | Instala Windsurf; el daemon lee datos locales si están disponibles. |

Después de configurar un login, reinicia el daemon.

### Proveedores con API key

Puedes guardar claves desde `http://localhost:8080` o exportarlas antes de arrancar el daemon.

| Proveedor | Variable |
| --- | --- |
| OpenAI | `OPENAI_API_KEY` |
| DeepSeek | `DEEPSEEK_API_KEY` |
| Kimi / Moonshot | `KIMI_API_KEY` o `MOONSHOT_API_KEY` |
| Cursor | `CURSOR_SESSION_TOKEN` |

Las claves guardadas desde el panel se almacenan en:

```text
~/.config/pocketmeter/api-keys.json
```

## Personalización de mascota y splash

PocketMeter no tiene por qué quedarse con la mascota por defecto.

El repo incluye animaciones pixel-art de Clawd obtenidas desde [claudepix.vercel.app](https://claudepix.vercel.app), creadas por [@amaanbuilds](https://x.com/amaanbuilds). También hay integración en el panel web para buscar mascotas en PetDex y convertir sprites compatibles cuando Pillow está disponible.

Desde el panel web puedes:

- Elegir qué proveedor se muestra.
- Activar u ocultar proveedores.
- Asignar mascotas por proveedor.
- Fijar una animación de splash.
- Enviar animaciones personalizadas al ESP32.

El pipeline local de animaciones vive en `tools/`:

```bash
node tools/scrape_claudepix.js
node tools/convert_to_c.js
```

`firmware/src/splash_animations.h` es generado; no lo edites a mano.

## Controles físicos

| Botón | Función actual |
| --- | --- |
| Izquierdo | Envía Space por BLE HID mientras se mantiene pulsado. |
| Central | Cambia de pantalla; en splash cambia animación. |
| Derecho | Envía Shift+Tab por BLE HID. |
| Touch | Alterna splash/uso y permite zonas interactivas según pantalla. |

## API útil

El ESP32 expone endpoints HTTP para integración y pruebas:

```bash
curl http://pocketmeter.local/api/health
curl http://pocketmeter.local/api/status

curl -X POST http://pocketmeter.local/api/usage \
  -H "Content-Type: application/json" \
  -d '{"providers":[{"provider":"claude","session_pct":45,"weekly_pct":28,"status":"allowed","ok":true}],"ok":true}'
```

El panel local del PC también hace de proxy hacia endpoints del ESP32 y añade otros para API keys, PetDex y configuración de splash.

## Estructura del repo

| Ruta | Contenido |
| --- | --- |
| `firmware/` | Firmware ESP32-S3 con LVGL, WiFi, servidor HTTP, BLE HID, UI y splash. |
| `daemon/clawdmeter-daemon.py` | Daemon multi-proveedor que consulta APIs y envía datos al dispositivo. |
| `daemon/web-server.py` | Panel web local y proxy hacia el ESP32. |
| `daemon/index.html` | UI web de configuración y estado. |
| `tools/` | Scraping/conversión de animaciones pixel-art. |
| `assets/` | Fotos, GIFs, iconos y fuentes usadas por el proyecto. |
| `screenshots/` | Capturas del dispositivo. |
| `WIFI_SETUP.md` | Notas específicas de la ruta WiFi + panel web. |
| `CLAUDE.md` | Contexto técnico interno para futuras sesiones de desarrollo. |

## Estado y advertencias

- La ruta actual documentada es **WiFi + HTTP + daemon Python**.
- Los scripts BLE antiguos siguen en el repo como referencia histórica.
- BLE sigue siendo útil para atajos HID de teclado.
- Algunos instaladores (`install.sh`, `install-mac.sh`) pertenecen a la ruta BLE legacy y avisan de ello al ejecutarse.
- El soporte de cada proveedor depende de credenciales locales, APIs disponibles y cambios externos de cada plataforma.
- Hay assets visuales y fuentes ligados a marcas de terceros. Revisa licencias antes de publicar builds o redistribuir el proyecto.

## Créditos

- Proyecto base original: [HermannBjorgvin/Clawdmeter](https://github.com/HermannBjorgvin/Clawdmeter).
- Fork y remodel actual: [edunavajas/PocketMeter](https://github.com/edunavajas/PocketMeter).
- Animaciones Clawd: [claudepix.vercel.app](https://claudepix.vercel.app), por [@amaanbuilds](https://x.com/amaanbuilds).
- Iconos Lucide: [lucide.dev](https://lucide.dev), MIT.
- Hardware: [Waveshare ESP32-S3-Touch-AMOLED-2.16](https://www.waveshare.com/esp32-s3-touch-amoled-2.16.htm?&aff_id=149786).

## Media pendiente

<!-- TODO: add YouTube video link -->
<!-- TODO: add assembly photos -->
<!-- TODO: add final desk setup photo -->
<!-- TODO: add short demo GIF of provider switching -->
