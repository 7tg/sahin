# Sahin Kullanım Kılavuzu

Sahin, PC için çıkmış *Blur* (Bizarre Creations, 2010) oyununun **kendi kopyanızı**,
jailbreak yapılmış bir PS5'te ana ekranda kendi kutucuğu olan bir homebrew uygulaması olarak
çalıştırır (`PPSA99404`). Oyunun PC sürümünü [prospero-win](https://github.com/mpereiraesaa/prospero-win)
üzerinden, yani Wine, DXVK ve PS5'in Vulkan sürücüsüyle çalıştırır.

**Oyunun kendi kopyanız gerekir. Sahin hiçbir oyun dosyası içermez.** Uygulamada yalnızca
kendi kodumuz, ayarlarımız ve çizimlerimiz ile kaynak koddan derlenen açık kaynaklı projeler
bulunur. Sahin'in Activision, Bizarre Creations veya Sony ile bir bağlantısı yoktur.

## Neler yapar

- Blur'u kendi kopyanızdan, kendine ait bir Wine önekine (prefix) kurar. Kopyanız hiçbir
  zaman değiştirilmez.
- Kutucuğa bastığınızda oyun doğrudan açılır.
- **Bölünmüş ekran:** dört DualSense kumandaya kadar, oturum açmış her kullanıcı için bir
  kumanda.
- **Çevrimiçi oyun:** Amax topluluk sunucuları üzerinden (varsayılan olarak açık).
- Dokunmatik yüzey fare gibi çalışır.
- İsterseniz oyuna kendi kayıt dosyanızla başlayabilirsiniz.

## Gerekenler

- Homebrew çalıştırabilen bir PS5: bir FTP sunucusu, 9021 numaralı yerel portta elfldr
  çalışan bir ELF yükleyici ve `/data/homebrew` klasöründen uygulama kuran
  [ShadowMount+](https://github.com/drakmor/ShadowMountPlus). Uygulama, kstuff-lite,
  ShadowMount+ ve Payload Manager kurulu 13.60 yazılımlı bir konsolda geliştirildi.
- **Blur klasörünüz.** Diskteki ya da eski Steam sürümündeki orijinal exe, Wine altında
  çalışmayan SecuROM kopya korumasını taşır: böyle bir kopya kurulur ama oyun açılır açılmaz
  kapanır ve Sahin bir sonraki açılışta bunu size söyler. Sahin SecuROM'u kaldırmaz, aşmaz ve
  bunu yapan bir şeye yönlendirmez.

## Kurulum

1. [Sürümler](https://github.com/7tg/sahin/releases) sayfasından `PPSA99404.zip` dosyasını
   indirin ve açın.
2. İçindeki `PPSA99404` klasörünü FTP ile konsolda `/data/homebrew/PPSA99404/` konumuna
   kopyalayın. ShadowMount+ kutucuğu ekler; kutucuk görünmezse konsolu yeniden başlatın.
3. Blur klasörünüzü FTP ile `/data/sahin/base-games/Blur` konumuna kopyalayın. Ya da bir USB
   belleğe `Blur` adlı bir klasör olarak koyun; USB'deki kopya kurulum sırasında konsola
   aktarılır.
4. Sahin'i açın ve **Install**'a basın. Sahin modları indirir (yaklaşık 4 MB, Amax istemci
   eklentileriyle 7 MB), oyununuzu öneke kopyalar ve modları ekler.

Bundan sonra kutucuk oyunu doğrudan başlatır.

### Kendi kayıt dosyanızla başlamak

Elinizdeki bir kayıtla (kendinizinki ya da tamamlanmış bir kayıt) başlamak isterseniz,
**kurulumdan önce** Blur'un `blursave` dosyasını `/data/sahin/saves/Blur/` klasörüne koyun.
Yeni bir kurulum bu dosyayı Blur'un kaydını tuttuğu yere yerleştirir. Blur'un zaten kendi
kaydı varsa kurulum ya da onarım bu kaydı **asla** değiştirmez, yani ilerlemeniz kaybolmaz.

## Ayarlar sayfası

Kutucuk açılırken **□ (Kare)** tuşunu basılı tutarsanız oyun yerine Sahin'in sayfası açılır:

- **Çözünürlük:** 3840×2160, 2560×1440 veya 1920×1080
- **Performans göstergesi:** ekranda kare hızı ve kare süreleri (DXVK HUD)
- **Online through Amax:** Amax sunucularıyla çevrimiçi oyun, açık ya da kapalı
- **Amax client extras:** Amax istemcisi, profil resimleri ve Lua eklentileriyle, açık ya da
  kapalı (varsayılan olarak kapalı)
- **Repair (△):** kurulumu onarır; kayıtlarınıza dokunmaz
- **Play:** oyunu başlatır

## Kontroller

- Oyunda kumanda bir Xbox 360 kumandası gibi görünür.
- **Dokunmatik yüzey** fare imlecini hareket ettirir, basınca tıklar.
- **Çıkmak için** Options tuşunu ve dokunmatik yüzeyi birlikte yaklaşık bir saniye basılı
  tutun.
- **Titreşim:** Blur istediğinde kumanda titrer; L2 ve R2 tetikleri de onunla birlikte
  titrer. Tetiklerdeki etkiyi PS5 ayarlarındaki tetik efekti yoğunluğu ile azaltabilir ya
  da kapatabilirsiniz.
- **Dil:** kopyanızda varsa Blur PS5'in dilinde açılır. Kopyanın dilleri klasöründeki dil
  dosyasında yazar (İngilizce kopyada `english.txt`: İngilizce, Fransızca ve İtalyanca);
  Sahin oyun açılırken bu dosyanın adını değiştirir. Blur'un Türkçesi yoktur; kopyanızda
  olmayan bir dil seçiliyse oyunun dili değişmez.

## Bölünmüş ekran

Blur'un bölünmüş ekran modu dört oyuncuya kadar çalışır:

1. Her ek kumandayı açın ve PS tuşuna basın.
2. O kumanda için bir kullanıcı ya da misafir seçerek oturum açın. Oturum açılmamış bir
   kumanda oyuna görünmez.
3. Blur'da bölünmüş ekranı başlatın. Oyun sırasında oturum açan bir oyuncu iki saniye içinde
   algılanır.

Her kumanda kendi titreşimini alır. Dokunmatik yüzeyle fare kullanımı ve çıkış tuş
kombinasyonu yalnızca birinci oyuncuya aittir.

## Çevrimiçi oyun

Blur'un resmi sunucuları kapalıdır. Sahin, çok oyunculu bağlantıları
[Amax](https://amax-emu.com) topluluk sunucularına yönlendirir. Bu ayar varsayılan olarak
açıktır. Amax hesabı ve giriş için Amax'ın kendi sayfasına bakın. Yönlendirme Blur'un 1.2
sürümündeki exe için yazılmıştır; başka bir exe ile çevrimiçi oyun çalışmayabilir, çevrimdışı
oyun bundan etkilenmez.

## Sorun giderme

- **Kutucuk görünmüyor:** konsolu yeniden başlatın ve klasörün tam olarak
  `/data/homebrew/PPSA99404/` konumunda olduğunu kontrol edin.
- **Oyun açılır açılmaz kapanıyor:** `Blur.exe` büyük olasılıkla SecuROM korumalıdır ve
  SecuROM Wine altında çalışmaz. Sahin bir sonraki açılışta bunu söyler.
- **Kurulum hata veriyor:** Blur klasörünün `/data/sahin/base-games/Blur` konumunda olduğunu
  ve `Blur.exe`, `binkw32.dll`, `cache`, `levels` ve `UI` öğelerini içerdiğini kontrol edin.
  Sorun sürerse sayfadaki Repair (△) ile onarın.
- **İkinci kumanda bölünmüş ekranda görünmüyor:** o kumandayla bir kullanıcı ya da misafir
  olarak oturum açın.
- **Çözünürlük değişmiyor:** ayarlar sayfasından seçin; Sahin bunu Blur'un kendi
  `settings.xml` dosyasına da yazar.

## Bilinen sınırlamalar

- Blur, 32 bit x86 çevirmeni üzerinden çalıştığı için kare hızları bir PC'dekinden düşüktür.
- Konsol testleri sürüyor. Sorunları depo sayfasındaki Issues bölümüne bildirin.

## Lisans

GPL-3.0 (`LICENSE`). Derleme, katkıda bulunanlar ve dosyaların yeri için İngilizce
[README](../README.md) dosyasına bakın.
