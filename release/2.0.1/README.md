# 2.0.1 — başarılı kartın ses düzeltmesi

`esp32-rfid-v2-2.0.1-ota.bin` dosyasını ESP32 web arayüzünden OTA ile yükleyin. Dosya 964.592 bayttır; imaj checksum/hash doğrulaması geçerlidir. Kullanıcı/ayar verilerini ve bölüm tablosunu korur. Format veya Nano güncellemesi gerekmez. Önce mevcut kullanıcı/ayar yedeğini indirin.

Başarılı kart için 2.0.0'da eklenen `A,<reader>` komutu kaldırıldı; okuyucunun kendi okuma sesiyle ikinci bir bip oluşturulmaz. Reddedilen/başarısız erişimde `D,<reader>` uyarısı ve elle okuyucu testindeki sesler korunur. Diğer 2.0.0 düzeltmeleri dahildir.

45 host testi (42 Python, 3 Node) ve ESP32 derlemesi başarılı. İki doğrudan regresyon testi: `test_decision_relay_precedes_rtc_without_extra_beep`, `test_decision_denied_card_keeps_warning_feedback`. İlk test düzeltme öncesinde ek UART komutu nedeniyle başarısızdı; düzeltmeden sonra başarılıdır. Testler UART kuyruğunu denetler; fiziksel okuyucu sesini ölçmez. Cihaza yükleme yapılmadı.

Kontroller: `python3 -m unittest discover -s tests -p 'test_*.py' -v`, `node --test tests/web.test.cjs`, `pio run -e esp32dev`, `git diff --check`. Bütünlük ve kaynak izleri bu klasördeki manifest/SHA-256 dosyalarındadır.
