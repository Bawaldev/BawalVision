# Rencana Anggaran Biaya (RAB) & Analisis Unit Economics

**Proyek:** BawalVision — Sistem Inspeksi Kualitas & Kebusukan Makanan Berbasis AI Edge  
**Kompetisi:** OpenCV AI Competition 2026 (AWS Sponsor Track)  
**Dokumen:** Analisis Kelayakan Industri & Unit Economics (Edge-Cloud Hybrid Architecture)  

---

## 1. Executive Summary

BawalVision menerapkan arsitektur **Edge-Local Hybrid Computing**. Pemrosesan citra komputer (Computer Vision), penajaman visual (SIMD Fast ISP), inferensi deteksi objek (YOLOv8s INT8 QDQ), dan evaluasi kebusukan mikroskopis (*micro-texture spoilage evaluation*) dieksekusi 100% secara lokal pada node edge (ESP32-S3 + C++20 Vision Service). 

Cloud AWS hanya difungsikan untuk agregasi telemetri terkompresi, audit trail, sinkronisasi model, dan pelaporan kepatuhan higienitas dengan pola **Scale-to-Zero**. Pendekatan ini memangkas biaya operasional cloud sebesar **>97.8%** dibandingkan arsitektur *Full Cloud Streaming*.

---

## 2. Hardware Bill of Materials (BOM) — Biaya per Node Edge

Komponen perangkat keras dirancang modular, berbiaya rendah (*low-cost off-the-shelf*), dan dapat direplikasi secara massal untuk industri katering, dapur umum, maupun rantai pasok makanan:

| No | Komponen | Spesifikasi Teknis | Estimasi Harga (IDR) | Estimasi Harga (USD) |
| :---: | :--- | :--- | :---: | :---: |
| 1 | **MCU Edge Controller** | ESP32-S3-WROOM-1 (Dual-Core Xtensa LX7 240MHz, 8MB PSRAM, 16MB Flash, Wi-Fi 2.4GHz + BLE 5) | Rp 68.000 | $4.25 |
| 2 | **Sensor Kamera** | Omnivision OV5640 5MP DVP CSP Module (Lens FOV 65°, Hardware JPEG compression, Auto Exposure/WB) | Rp 65.000 | $4.06 |
| 3 | **Display On-Node** | Waveshare 0.96" / 1.8" SPI TFT LCD (ST7735 driver, RGB565 128x160 resolution) | Rp 32.000 | $2.00 |
| 4 | **Sistem Daya & Kabel** | 5V 2A USB-C Power Adapter + 1m High-Flex Braided Type-C Cable | Rp 25.000 | $1.56 |
| 5 | **Indikator & I/O Fisik** | WS2812B NeoPixel RGB LED + Tactile Tact Switch Button + Passive Pull-ups | Rp 6.000 | $0.38 |
| 6 | **Enclosure / Casing** | Custom 3D Printed Food-Grade PETG/ABS Enclosure with Ventilation & Mounting Bracket | Rp 18.000 | $1.13 |
| **Total** | **Hardware Cost per Inspection Station** | **1 Unit Lengkap Edge Inspection Node** | **Rp 214.000** | **~$13.38 USD** |

---

## 3. Analisis Biaya Operasional: Edge vs. Full-Cloud Streaming

Komparasi biaya operasional bulanan (30 hari operasional, 12 jam/hari streaming aktif) per 1 stasiun kamera:

### A. Skenario 1: Full-Cloud Streaming (Arsitektur Tradisional)
- **Bandwidth Video:** Kamera VGA 640x480 @ 20 FPS mentransmisikan video terkompresi (~1.5 Mbps) langsung ke cloud.
  - Data transfer per bulan: `1.5 Mbps * 3600 s * 12 jam * 30 hari = 243 GB/bulan`.
- **AWS Kinesis Video Streams:** Ingestion ($0.0085/GB) = `$2.07/bulan`.
- **AWS Cloud Compute (EC2 c6i.xlarge untuk YOLOv8 Inference):** `$0.17/jam * 360 jam = $61.20/bulan`.
- **Total Biaya Operasional Cloud:** **~$63.27 USD / node / bulan** (Rp 1.012.000 / bulan).

### B. Skenario 2: BawalVision Hybrid Architecture (Edge-First + AWS Scale-to-Zero)
- **Local Edge Inference:** Pemrosesan YOLOv8s INT8 QDQ + OpenCV Fast ISP berjalan pada C++ CPU runtime lokal ($0 cloud compute).
- **Data ke Cloud (AWS S3 & DynamoDB/RDS):** Hanya metadata JSON ringkas (status tray, timestamp, skor kebusukan, box coordinates) dan telemetri perangkat dikirim setiap deteksi perubahan signifikan (~2 KB/transaksi).
  - Data transfer per bulan: `~15 MB / bulan`.
- **AWS Lambda (Ingestion API):** ~50.000 request/bulan (Free Tier / < $0.01).
- **AWS S3 Standard (Arsip Laporan Audit & Sampel Gagal):** ~500 MB/bulan ($0.01).
- **AWS API Gateway (HTTP API):** $0.05/bulan.
- **Total Biaya Operasional Cloud:** **~$0.10 USD / node / bulan** (Rp 1.600 / bulan).

> **Efisiensi Biaya:** Arsitektur BawalVision memberikan penghematan biaya cloud operasional sebesar **99.84%** sekaligus menghilangkan ketergantungan latensi internet (*zero field latency* ~14 ms).

---

## 4. Proyeksi Total Cost of Ownership (TCO) 12 Bulan

Proyeksi investasi total pengadaan dan operasional untuk berbagai skala deployment:

| Skala Penggunaan | Jumlah Node | Belanja Modal Hardware (CAPEX) | Biaya Cloud AWS 1 Tahun (OPEX) | Total TCO Tahun Pertama |
| :--- | :---: | :---: | :---: | :---: |
| **Pilot / UMKM Dapur** | 1 Node | Rp 214.000 ($13.38) | Rp 19.200 ($1.20) | **Rp 233.200 ($14.58)** |
| **Katering Komersial** | 10 Node | Rp 2.140.000 ($133.80) | Rp 192.000 ($12.00) | **Rp 2.332.000 ($145.80)** |
| **Pabrik / Rantai Dingin** | 100 Node | Rp 21.400.000 ($1.338,00) | Rp 1.920.000 ($120.00) | **Rp 23.320.000 ($1.458,00)** |

---

## 5. Kesimpulan Penilaian Juri & Nilai Tambah Kompetisi

1. **Kelayakan Finansial (Bankable & Scalable):**
   Dengan biaya hardware di bawah $15 USD dan biaya komputasi bulanan mendekati nol per unit, BawalVision sangat layak diterapkan di negara berkembang untuk menekan angka keracunan makanan dan susut pangan (*food loss*).
2. **Kesesuaian Ekosistem AWS:**
   Memanfaatkan layanan serverless AWS (Lambda, API Gateway, S3, RDS PostgreSQL) secara bijak dengan arsitektur *event-driven* yang hemat energi (*Green Computing*).
3. **Ketahanan Jaringan Industri (*Offline-First Resilience*):**
   Sistem tetap menginspeksi makanan dengan FPS tinggi (20+ FPS) meskipun koneksi internet terputus, dan secara otomatis melakukan sinkronisasi data (*batch queue flush*) saat koneksi cloud kembali online.
