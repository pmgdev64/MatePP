<p align="center">
  <img src="icons.png" alt="MatePP Icon" width="128">
</p>

<h1 align="center">MatePP</h1>

<p align="center">
Lightweight animated wallpaper engine for Windows.
</p>

<p align="center">
The goal of this project is to provide a simple wallpaper engine that works well even on older and low-end computers.
</p>

<p align="center">
  <a href="https://github.com/pmgdev64/matepp/releases">
    <img src="https://img.shields.io/github/v/release/pmgdev64/matepp" alt="Release">
  </a>
  <a href="https://github.com/pmgdev64/matepp/releases">
    <img src="https://img.shields.io/github/downloads/pmgdev64/matepp/total" alt="Downloads">
  </a>
  <a href="https://github.com/pmgdev64/matepp/stargazers">
    <img src="https://img.shields.io/github/stars/pmgdev64/matepp" alt="Stars">
  </a>
  <a href="https://github.com/pmgdev64/matepp/forks">
    <img src="https://img.shields.io/github/forks/pmgdev64/matepp" alt="Forks">
  </a>
  <a href="https://github.com/pmgdev64/matepp/issues">
    <img src="https://img.shields.io/github/issues/pmgdev64/matepp" alt="Issues">
  </a>
  <a href="https://github.com/pmgdev64/matepp/pulls">
    <img src="https://img.shields.io/github/issues-pr/pmgdev64/matepp" alt="Pull Requests">
  </a>
  <a href="https://github.com/pmgdev64/matepp/blob/main/LICENSE">
    <img src="https://img.shields.io/github/license/pmgdev64/matepp" alt="License">
  </a>
  <a href="https://github.com/pmgdev64/matepp/commits/main">
    <img src="https://img.shields.io/github/last-commit/pmgdev64/matepp" alt="Last Commit">
  </a>
</p>

---

> ## 🚀 Project Status
>
> **Development has officially resumed!**
>
> The development environment is restored, and active work is underway. **Stay tuned for new updates!**
> 
> **⚠️ CRITICAL WARNING FOR WINDOWS 11 24H2:** 
> Do **NOT** use MatePP on Windows 11 version 24H2. Microsoft completely changed the internal `WorkerW` desktop window structure. Running the engine on this version will cause rendering failures, blank backgrounds, or shell instability. 

---

# 📥 Installation

1. Go to the [Releases](https://github.com) page.
2. Download the latest installer package (`MatePP_Setup.exe`) or the standard standalone archive.
3. Run the installer and follow the on-screen instructions to integrate MatePP with your desktop.

---

# 📸 Screenshots

<p align="center">
  <img src="screenshot (1).png" width="32%" alt="Screenshot 1" />
  <img src="screenshot (6).png" width="32%" alt="Screenshot 2" />
  <img src="screenshot (4).png" width="32%" alt="Screenshot 3" />
</p>

---

# 📝 About

MatePP is a lightweight animated wallpaper engine for Microsoft Windows.

Unlike many wallpaper applications that focus on visual effects and extensive features, MatePP focuses on efficiency, stability, and low resource usage.

The project aims to deliver smooth animated wallpapers while remaining usable on older and low-end computers.

---

# ✨ Features

- Lightweight design
- Direct2D rendering
- FFmpeg video decoding & playback
- Hardware accelerated decoding (where supported)
- Low CPU usage
- Low memory usage
- Designed for low-end hardware
- Open source
- Easy setup via dedicated installer
- Simple desktop integration

---

# 🎯 Goals

- Support broad range of video formats via FFmpeg
- High performance on older hardware
- Stable wallpaper rendering
- Small installation size
- Easy-to-use wallpaper manager
- Minimal resource consumption
- Open source development

---

# 💻 Windows Compatibility

###  Recommended
- Windows 10
- Windows 11 21H2
- Windows 11 22H2
- Windows 11 23H2

### ⚠️ Limited Support
- Windows 11 24H2

Windows 11 version 24H2 introduced changes to the Explorer desktop shell. These changes may affect wallpaper rendering and can cause problems such as:
- Wallpaper not appearing
- Blank desktop background
- WorkerW rendering issues

*Support for Windows 11 24H2 remains highly experimental.*

---

# 📂 Project Structure

```text
MatePP/
    └── Wallpaper Engine (Core Core)

MatePP-Manager/
    └── Wallpaper Manager (UI & Settings)
```

---

# 🛠️ Build Environment

Current development environment tools:

- **IDE:** Code::Blocks
- **Compiler:** MinGW-w64 / GCC
- **API/Libraries:** Windows SDK, Direct2D, FFmpeg

---

# 🗺️ Future Plans

Planned improvements include:

- Improved installer setup and seamless updates
- Improved playback performance with FFmpeg
- Better hardware video decoding
- Additional wallpaper formats
- More stable rendering pipeline
- Improved Windows compatibility
- DirectX rendering improvements

*The roadmap may change in future releases.*

---

# 🤝 Contributing

Contributions, bug reports, and pull requests are welcome! Feel free to check the [Issues](https://github.com) page or submit a pull request.

---

# 📜 License

Licensed under the **GNU General Public License v3.0**.

See the `LICENSE` file for more information.

---

<p align="center">
Made with ❤️ by <a href="https://github.com">PmgDev64</a>
</p>
