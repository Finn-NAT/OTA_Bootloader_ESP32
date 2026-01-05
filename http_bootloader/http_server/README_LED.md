# LED control server (Flask)

Server này dùng để ESP32/ESP32-S3 polling HTTP và lấy trạng thái **on/off** để bật/tắt LED.

## Endpoint

- `GET /led` → trả về plain text: `on` hoặc `off`
- `POST /led` → set state
  - JSON: `{ "state": "on" }` hoặc `{ "state": "off" }`
  - hoặc `{ "on": true }`
- `GET /` → trang web bấm ON/OFF

## Chạy trên Windows PowerShell

Trong thư mục `http_server` của project.

```powershell
cd "c:\Users\Tuan\esp\v5.3.4\esp-idf\examples\my_project\ota_bootloader\http_bootloader\http_server"
py -m venv .venv
.\.venv\Scripts\python.exe -m pip install --upgrade pip
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
.\.venv\Scripts\python.exe led_server.py
```

Mặc định server listen: `http://0.0.0.0:8000/`

## ESP32 cấu hình URL

Trên ESP32, set URL (ví dụ):
- `http://<IP-PC-của-bạn>:8000/led`

ESP32 sẽ GET `/led` và bật/tắt LED theo response `on/off`.
