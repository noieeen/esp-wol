# 🔌 ESP32 Wake-on-LAN (WOL) with Firebase Queue

A project that enables remote Wake-on-LAN for devices using an ESP32 board, triggered via Firebase Realtime Database — with both **instant WOL via stream** and **queued delayed tasks**.

---

## 🚀 Features

- ✅ WOL via Firebase RTDB stream (`/wol/proxmox`)
- ✅ Task queue with delay support (`/wol/queue`)
- ✅ Compatible with Firebase Auth REST API
- ✅ SSL handshake-safe version (requires specific ESP32 core)
- ✅ Auto initializes missing Firebase keys

---

## 🔧 Requirements

- ESP32 (recommend Dev Module)
- Arduino IDE with:
  - ESP32 Arduino Core `v3.2.0`
  - Firebase ESP Client Library `v4.4.17`
- Firebase Project:
  - Enabled Realtime Database
  - User `xx@mail.com` with password `xxxxxxxx`
  - API key from Firebase Console

---

## 📁 Firebase Structure

```json
"wol": {
  "proxmox": false,
  "queue": {
    "task_001": {
      "mac": "xx:xx:xx:xx:xx:xx",
      "delay": 3000,
      "status": "pending"
    }
  }
}
