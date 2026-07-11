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

# Screenshots

<img src="screenshot (1).png">
<img src="screenshot (6).png">
<img src="screenshot (4).png">

---

## Features

* Lightweight design
* Direct2D rendering
* Media Foundation video playback
* Low memory usage
* Built for Windows desktop

---

## Current Status

This project is currently under active development.

Some features are incomplete and APIs may change between releases.

As an independent personal project, development may be paused or discontinued in the future depending on available time and resources.

---

## Windows Compatibility

### Recommended

* Windows 10
* Windows 11 (21H2, 22H2, 23H2)

### Not Recommended

* Windows 11 24H2

Windows 11 version 24H2 introduced changes to Explorer's desktop shell behavior. During testing, these changes may cause wallpaper rendering issues such as a blank desktop or loss of the rendering surface.

Support for Windows 11 24H2 is currently under investigation.

---

## Goals

* Support common video formats
* High performance on low-end hardware
* Stable wallpaper rendering
* Simple manager application
* Open source

---

## Project Structure

```text
MatePP/
    Wallpaper Engine

MatePP-Manager/
    Wallpaper Manager
```

---

## Build

Current development environment:

* Code::Blocks
* MinGW-w64
* Windows SDK

---

## License

GPL-3.0
