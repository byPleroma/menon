# Menon
Written in C with GTK and GLib, I present "Menon", a lightweight application menu for Linux. It manages interface lifecycle, asynchronous search, app pinning, and recent history. It focuses on efficiency through dynamic caching, multithreaded I/O, and CSS customization for a fluid experience.

---

# 🚀 Menon

**The high-performance alternative to Whisker Menu.**
<img width="440" height="450" alt="Captura de tela_2026-04-25_01-42-44 (1)" src="https://github.com/user-attachments/assets/9029caac-1485-421c-9d6c-af6751c35fbf" />


**Menon** is an application launcher for Linux (specifically optimized for XFCE) written in pure C and GTK. It was designed for users seeking the perfect balance between **Windows 11 aesthetic minimalism** and **raw Unix efficiency**.

![License](https://img.shields.io/badge/license-GPLv3-blue.svg)
![Language](https://img.shields.io/badge/language-C-orange.svg)
![Toolkit](https://img.shields.io/badge/toolkit-GTK3-green.svg)

---

## 🔥 Why choose Menon?

While Whisker Menu is a classic, Menon was built with a focus on modern hardware and workflows that demand instantaneous speed.

### ⚡ 25% Lighter
Thanks to its native C architecture and rigorous memory management, Menon consumes, on average, **25% fewer resources (RAM/CPU)** than the traditional Whisker Menu.
- **Zero Interpreters:** No Python, no JS. Just compiled code.
- **Asynchronous I/O:** The menu never "stutters" while loading icons, as the disk is read in separate threads.

### 🎨 "Next-Gen" Aesthetic (Windows 11 Inspired)
Say goodbye to dated menus. Menon brings an interface inspired by the fluid and centralized design of Windows 11:
- **Favorites Side Panel (Pins):** Your essential apps are always visible, just like the modern Start menu.
- **CSS Customization:** Change colors, transparency, and spacing without touching the source code.

---

## ✨ Key Features

- **Debounced Search:** Search is only triggered milliseconds after you stop typing, saving processing power.
- **Pinning System:** Pin and unpin applications with a simple right-click.
- **Smart History:** Menon learns which apps you use most and highlights them.
- **Safety and Stability:** Implementation of `RWLocks` (Read-Write Locks) to ensure your data is never corrupted during use.
- **Desktop Integration:** Add shortcuts to the desktop or panel directly through the context menu.

---

## 🛠 Installation (Quick Build)

**Dependencies:** `gtk+-3.0`, `gio-2.0`, `pango`.

```bash
# Clone the repository
git clone https://github.com/your-user/menon.git

# Enter the folder
cd menon

# Compile the project
gcc -o menon main.c ui.c core.c pinned.c `pkg-config --cflags --libs gtk+-3.0`

# Run
./menon
```

---

## 💡 Project Philosophy

**Menon** doesn't try to be a Swiss Army knife. It was created with a single purpose: **to be the fastest and most beautiful path between your thought and opening a program.** > "If you can feel the menu opening, it’s not fast enough yet."

---

## 📄 License

Distributed under the **GPLv3** license (Full open-source protection). See `LICENSE` for more details.

---
*Developed with ❤️ for the Linux community.*
