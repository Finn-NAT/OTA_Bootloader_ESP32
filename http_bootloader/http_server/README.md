# Flask web app (simple)

Một app web Flask tối giản để bạn chạy nhanh trong môi trường ảo.

## Tạo môi trường ảo + cài thư viện (Windows PowerShell)

Nếu PowerShell chặn `Activate.ps1` (ExecutionPolicy), bạn vẫn có thể chạy bằng cách gọi thẳng python trong `.venv`.

```powershell
cd "c:\Users\Tuan\esp\v5.3.4\esp-idf\examples\my_project\ota_advanced\py_update_firmware"
py -m venv .venv
.\.venv\Scripts\python.exe -m pip install --upgrade pip
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
```

## Chạy app

```powershell
cd "c:\Users\Tuan\esp\v5.3.4\esp-idf\examples\my_project\ota_advanced\py_update_firmware"
.\.venv\Scripts\python.exe app.py
```

Mở trình duyệt:
- http://127.0.0.1:5000/

Health check:
- http://127.0.0.1:5000/health

## (Tuỳ chọn) Bật activate cho PowerShell

```powershell
Set-ExecutionPolicy -ExecutionPolicy RemoteSigned -Scope CurrentUser
```

.\.venv\Scripts\Activate.ps1  