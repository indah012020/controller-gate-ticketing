const express = require('express');
const app = express();
const PORT = 3000;

// Middleware untuk membaca JSON dan URL-encoded data jika diperlukan
app.use(express.json());
app.use(express.urlencoded({ extended: true }));

// Simulasi database atau daftar tiket yang valid
// Di dunia nyata, Anda bisa menghubungkannya ke PostgreSQL, MySQL, MongoDB, dll.
const validTickets = {
    "TICKET-12345": { jum_tiket: 1 },
    "TICKET-GROUP-3": { jum_tiket: 3 }, // Contoh tiket untuk 3 orang
    "3858425114": { jum_tiket: 4 }, // Contoh tiket untuk 3 orang
    "TEST": { jum_tiket: 1 }             // Untuk uji coba fungsi tesApi() dari ESP32
};

/**
 * Endpoint 1: Scan QR (Validasi Tiket & Buka Gate)
 * URL: http://<ip-server>:3000/api/v1/scan-qr?qr-code=<kode>
 * 
 * Skenario Respon:
 * 1. Berhasil/Valid -> status: true, jum_tiket: <angka>
 * 2. Gagal/Tidak Valid -> status: false (atau tidak ada status: true)
 */
app.get('/api/v1/scan-qr', (req, res) => {
    const qrCode = req.query['qr-code'];

    console.log(`[SCAN-QR] Menerima QR Code: "${qrCode}"`);

    // Validasi apakah parameter qr-code dikirim
    if (!qrCode) {
        return res.status(400).json({
            status: false,
            message: "Parameter qr-code wajib diisi!"
        });
    }

    // Cek apakah tiket terdaftar di database simulasi
    const ticket = validTickets[qrCode];

    if (ticket) {
        // Skenario 1: Tiket Valid / Berhasil
        console.log(`[SCAN-QR] Status: VALID. Jumlah tiket: ${ticket.jum_tiket}`);
        return res.status(200).json({
            status: true,
            jum_tiket: ticket.jum_tiket, // Bisa berupa angka atau string angka (ESP32 sudah menghandle .toInt())
            message: "Tiket valid, silakan masuk."
        });
    } else {
        // Skenario 2: Tiket Ditolak / Tidak Terdaftar / Expired
        console.log(`[SCAN-QR] Status: DITOLAK (Tiket tidak ditemukan atau sudah tidak berlaku)`);
        return res.status(200).json({
            status: false,
            message: "Tiket tidak valid atau sudah kedaluwarsa."
        });
    }
});

/**
 * Endpoint 2: Capture (Pencatatan Orang Masuk Melewati Sensor)
 * URL: http://<ip-server>:3000/api/v1/capture?qr-code=<kode>
 * 
 * Skenario:
 * Pada kode Arduino Anda, balasan dari /capture diabaikan (hanya untuk logging HTTP code),
 * namun server tetap harus memberikan respon balik (misal HTTP 200 OK).
 */
app.get('/api/v1/capture', (req, res) => {
    const qrCode = req.query['qr-code'];

    console.log(`[CAPTURE] Orang melewati sensor untuk QR Code: "${qrCode}"`);

    if (!qrCode) {
        return res.status(400).json({
            success: false,
            message: "QR code tidak disertakan pada capture."
        });
    }

    // Lakukan pencatatan ke database bahwa pemilik tiket ini telah melewati gate
    // Contoh: update log history / status kedatangan pengunjung

    return res.status(200).json({
        success: true,
        message: "Data orang lewat berhasil dicatat."
    });
});

// Menjalankan server
app.listen(PORT, '0.0.0.0', () => {
    console.log(`Server Gate Ticketing berjalan di http://localhost:${PORT}`);
    console.log(`Gunakan IP lokal komputer Anda (contoh: http://10.10.1.104:${PORT}) untuk diakses ESP32.`);
});