# CAN OBD Logger – ESP32 (LILYGO T-CAN485) / PlatformIO

Este projeto coleta tráfego CAN via TWAI (controlador CAN interno do ESP32) e grava no microSD.
Além do log RAW, ele envia requisições de diagnóstico (OBD-II / UDS) para gerar um conjunto de dados com *request/response*.

## Objetivo (coleta sem PC)
- **LED WS2812**: indica vida/RX/erro.
- **Portal Wi‑Fi (AP)**: permite checar status e baixar logs usando um celular, sem computador.
- **Arquivos auxiliares**:
  - `stats_XXX.csv` (1 linha/seg): ajuda a auditar qualidade da coleta (fps, bus_off, overruns...).
  - `events_XXX.csv`: marcações de experimento (ex.: início/fim de ataque) via endpoint.

## Como usar
1. Abra o projeto no VS Code + PlatformIO.
2. Faça upload para a ESP32.
3. Ligue a placa com microSD inserido.
4. Conecte no Wi‑Fi **CAN-LOGGER** (senha padrão `12345678`).
5. Acesse no navegador: `http://192.168.4.1/`

### Endpoints úteis
- `/` : status (HTML)
- `/api/status` : status em JSON
- `/api/frames` : últimos frames vistos (texto/CSV leve)
- `/files` : lista arquivos no SD (com links de download)
- `/download?name=/logs/canlog_000.csv` : baixa arquivo
- `/api/mark?tag=attack_on&msg=flood_1000fps` : grava um marcador em `events_XXX.csv`

## Formato dos logs
### RAW (`canlog_XXX.csv`)
`ts_us,can_id,is_ext,is_rtr,dlc,data_hex,dir`

- `ts_us`: timestamp (micros) usando `esp_timer_get_time()`.
- `dir`: `R` (RX) / `T` (TX)

### OBD (`obd_XXX.csv`)
`ts_us,src_id,service,pid_or_did,name,value,unit`

### UDS (`uds_XXX.csv`)
`ts_us,src_id,did,data_hex`

### Stats (`stats_XXX.csv`)
`uptime_ms,rx_frames,tx_frames,fps,can_state,bus_err,rx_missed,rx_overrun,sd_ok`

### Events (`events_XXX.csv`)
`ts_us,tag,msg`

## Ajustes rápidos
As constantes principais estão em `lib/Mylibrary/app_config.h`.

- Intervalos de polling OBD/UDS
- SSID/senha do AP
- Verbosidade de prints no Serial

## Observações
- O firmware tenta **recovery automático em BUS_OFF**.
- Para máxima taxa de coleta, mantenha `SERIAL_VERBOSE_* = false`.
