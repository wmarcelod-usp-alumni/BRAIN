# BRAIN — Bus Research and Analysis for In-vehicle Networks

Research tools, firmware, and datasets for automotive cybersecurity research, focusing on CAN bus analysis and OBD-II diagnostics.

Developed as part of a Master's dissertation at ICMC-USP on cybersecurity of ground vehicles.

## Repository Structure

```
BRAIN/
├── notebooks/          # Jupyter notebooks for data analysis
│   ├── obd_analysis.ipynb          # OBD-II wheel speed & GPS analysis
│   └── sensor_exploration.ipynb    # Sensor exploration by unit type
├── data/               # OBD-II telemetry datasets (CSV)
│   ├── obd_june2024/   # Data collected June 2024
│   └── obd_dec2024/    # Data collected December 2024
├── firmware/           # Embedded firmware for CAN bus tools
│   ├── ecu_portal/     # ECU simulator (Arduino + Python web portal)
│   └── lilygo_can/     # CAN bus sniffer/injector (ESP32 LilyGo T-CAN485)
└── docs/               # Supporting documentation and assets
```

## Data Format

OBD-II telemetry CSVs use semicolon-delimited format with columns:

| Column | Description |
|--------|-------------|
| SECONDS | Timestamp (seconds) |
| PID | OBD-II parameter name |
| VALUE | Sensor reading |
| UNITS | Unit of measurement |
| LATITUDE | GPS latitude |
| LONGTITUDE | GPS longitude |

## Firmware

### ECU Portal (`firmware/ecu_portal/`)

Hybrid ECU simulator: Arduino-based CAN node + Python Flask web interface for monitoring and control.

- `ecu_portal_arduino/` — PlatformIO project (Arduino UNO + CAN shield)
- `app.py` — Flask web server
- `templates/` — Web UI

### LilyGo CAN (`firmware/lilygo_can/`)

CAN bus sniffer and OBD-II poller for the LilyGo T-CAN485 (ESP32). Supports real-time monitoring, SD card logging, and web portal.

- PlatformIO project targeting `esp32dev`
- Custom libraries in `lib/Mylibrary/` (CAN TWAI driver, OBD poller, web portal, SD logger, LED status)

## Requirements

- **Notebooks**: Python 3.x, pandas, seaborn, matplotlib, numpy
- **Firmware**: [PlatformIO](https://platformio.org/) CLI or IDE
- **ECU Portal**: Python 3.x, Flask (`pip install -r firmware/ecu_portal/requirements.txt`)

## License

This project is part of academic research at ICMC-USP. Contact the author for licensing inquiries.

## Author

Marcelo Duchene — [marcelo.duchene@alumni.usp.br](mailto:marcelo.duchene@alumni.usp.br)
