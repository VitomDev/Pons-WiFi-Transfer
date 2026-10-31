<p align="center"><img src="assets/pons.png" width="96" alt="Pons WiFi Transfer"></p>

# 🌉 Pons — WiFi Transfer
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

Move files between your PC and your phone over Wi-Fi. 📲💻  
Open Pons, scan the QR code with your phone's camera and a simple page opens in its browser: download what you dragged into Pons, or send photos and files straight to a folder on your PC.

**Nothing to install on the phone**, no accounts, no cloud: everything stays on your local network.

---

## 🚀 Features

- 📷 **Scan and go**: a QR code with the PC's address; any phone camera opens it (iPhone and Android)
- ⬇️ **PC → phone**: drag files (or a folder) into Pons and download them on the phone
- ⬆️ **Phone → PC**: pick several photos or files at once, with a progress bar for each, saved to **Downloads** or any folder you choose
- 📝 **Shared text**: send a link or some text either way, and copy it on the other side
- 📊 **Transfers list** on the PC with live progress; click a received file to show it in its folder
- 🔒 **Private by design**
  - Every request needs a random per-session key, included in the QR code. **New link** changes it at once
  - Only devices on your private network (home Wi-Fi) can connect
  - The server stops after **15 minutes** without use (5, 15, 30, 60 minutes or never) and when you close Pons
  - Half-received files never look complete, and names sent by the phone can't escape the chosen folder
- 🌐 Changed Wi-Fi? Pons notices and shows a new code
- 🌙 Clean Windows 11 look that follows your light or dark theme; the phone page follows the phone's theme too
- 🪶 Tiny: a ~0.7 MB program, a ~2.3 MB installer, no .NET or Visual C++ runtime needed

---

## 📦 Install

Download `PonsSetup.exe` from [Releases](https://github.com/VitomDev/Pons-WiFi-Transfer/releases/latest) and run it.

> 🧰 **Recommended:** install it with [Vitool Box](https://github.com/VitomDev/Vitool-Box), which keeps all VitomDev apps in one place and installs or removes them with a switch.

- Per-user install, no admin rights needed
- 🛡️ The first time Pons starts, **Windows Firewall** asks whether it may use the network: allow it on **Private networks** (home Wi-Fi). Public networks aren't needed

Uninstalling (Settings → Apps) removes Pons and its settings. Files you received stay where they were saved.

---

## 🖱️ Usage

| Action | How |
| --- | --- |
| Connect the phone | Scan the QR code with the camera (same Wi-Fi as the PC) |
| Share files with the phone | Drag them into Pons, or **Add files** |
| Stop sharing a file | Select it → **Remove** (or `Delete`) |
| Send files to the PC | On the phone: **Choose photos or files** |
| Where files arrive | **Change…** picks the folder, **Open** shows it |
| Send text or a link | Type it and **Send to phone** / **Send to PC** |
| Invalidate the old QR code | **New link** |
| Copy the link (e.g. for a laptop) | **Copy link** |
| Port and idle timeout | Bottom left of the window |

💡 Phone and PC must be on the same network. Guest Wi-Fi networks often isolate devices from each other; use the main network if the phone can't connect.

---

## ⚡ Performance

Measured on the development PC (Windows 11), idle with the server running:

| | |
| --- | --- |
| Idle CPU (60 s) | ~16 ms |
| Idle memory | 19.6 MB working set, 2.9 MB private |
| Idle threads | 4 |
| `Pons.exe` / installer | 0.7 MB / 2.3 MB |

---

## 🔧 Build

Requirements: Visual Studio 2022 or newer with the C++ x64 tools and Inno Setup 6 (`winget install JRSoftware.InnoSetup`).

```powershell
.\build.ps1
```

It downloads the pinned dependencies into `.deps/` (Windows SDK, CMake, Ninja and ZXing-C++, all verified by hash), builds everything, runs the tests and writes the installer to `dist/PonsSetup.exe`.

`.\build.ps1 -SkipTests` and `.\build.ps1 -SkipInstaller` skip those steps.

---

## 📚 Dependencies

- [ZXing-C++](https://github.com/zxing-cpp/zxing-cpp) — creating the QR code
- Windows Sockets — the small built-in web server, no HTTP library

See [third-party notices](THIRD-PARTY-NOTICES.md) for licenses.

---

## 🪟 Compatibility

Windows 10 (version 1809 or newer) and Windows 11, 64-bit. Any phone with a modern browser.

---

## 🧪 Project Status

✅ Stable.  
This project is fully functional as-is and ready to use.
Future updates to improve features or the interface are not ruled out.

---

## 📄 License

[MIT](LICENSE) © VitomDev

---

## 📲 Contact

Feel free to reach out or follow me:

- 🐙 GitHub: [@VitomDev](https://github.com/VitomDev)
- 🐦 Twitter/X: [@VitomDev](https://twitter.com/VitomDev)
- 🎮 Twitch: [@VitomDev](https://www.twitch.tv/VitomDev)
- 💼 LinkedIn: [VitomDev](https://www.linkedin.com/in/víctor-escós-batalla/)
