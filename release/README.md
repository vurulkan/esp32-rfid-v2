# Güncel: 2.0.1

Başarılı kartta ek onay bipi kaldırılmış OTA: [esp32-rfid-v2-2.0.1-ota.bin](2.0.1/esp32-rfid-v2-2.0.1-ota.bin). Okuyucunun kendi sesi korunur; reddedilen kart uyarısı devam eder. Kullanıcı/ayar verileri korunur, format veya Nano güncellemesi gerekmez. Bu yama yalnız ESP32 uygulamasını değiştirir.

## Önceki 2.0.0 dosyaları

`esp32-rfid-v2-2.0.0-ota.bin` dosyasını ESP32 web arayüzündeki Firmware Update bölümünden yükleyin. Yüklemeden önce kullanıcı/ayar yedeğini indirin. Bu güncelleme uygulamayı değiştirir; mevcut kullanıcı/ayar dosyalarını ve bölüm tablosunu korur. Format veya dosya sistemi yükleme gerekmez.

Hedef ESP32-WROOM, 4 MB flash, varsayılan OTA bölümüdür. Dosya 964.608 bayttır ve 1.310.720 baytlık uygulama yuvasına sığar. `SHA256SUMS.txt` ve `manifest.json` bütünlük/derleme bilgilerini içerir; `source-files.sha256` derlenen çalışma ağacındaki kaynakların izini tutar.

`wiegand-nano-2.0.0.hex` isteğe bağlı, ayrı Nano güncellemesidir. ESP32 önceki Nano yazılımıyla da çalışır. Nano dosyasını ESP32'nin OTA ekranına yüklemeyin; Nano USB/seri bağlantısıyla programlanır. Nano üzerindeki bootloader'a uygun yükleme seçeneği fiziksel cihazda belirlenmelidir.

44 host testi ve iki hedef derlemesi başarılıdır. Fiziksel cihaza yükleme veya saha testi yapılmadı. Ayrıntılar: [düzeltmeler ve test kanıtı](../docs/duzeltmeler-2026-11-20.md).
