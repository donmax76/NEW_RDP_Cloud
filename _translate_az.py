#!/usr/bin/env python3
"""Replace all Ukrainian text in .t.az spans with Azerbaijani translations."""
import re, sys

with open("PRESENTATION.html", "r", encoding="utf-8") as f:
    html = f.read()

# Each pair: (Ukrainian, Azerbaijani)
translations = [
    # ── Nav ──
    ("Головна", "Ana Səhifə"),
    ("Архітектура", "Arxitektura"),
    ("Функціонал", "Funksionallıq"),
    ("Безпека", "Təhlükəsizlik"),

    # ── Cover eyebrow ──
    ("Системна документація · v1.0.196", "Sistem Sənədləşməsi · v1.0.196"),
    ("Система прихованого віддаленого адміністрування", "Gizli Uzaqdan İdarəetmə Sistemi"),

    # Cover sub paragraph
    ("Повний віддалений контроль над комп'ютером через звичайний браузер. Працює непомітно для користувача. Всі дані зашифровані. Сервер-посередник в іншій країні приховує оператора.",
     "Adi brauzer vasitəsilə kompüter üzərində tam uzaqdan nəzarət. İstifadəçiyə görünmədən işləyir. Bütün məlumatlar şifrələnib. Başqa ölkədəki relay server operatoru gizlədir."),

    # Cover chips
    ("🔒 Повне шифрування трафіку", "🔒 Tam trafik şifrələməsi"),
    ("🧩 Модулі завантажуються в пам'ять", "🧩 Modullar yaddaşa yüklənir"),
    ("🔑 Надійна аутентифікація", "🔑 Güclü autentifikasiya"),
    ("🛡️ Обхід антивірусів", "🛡️ Antivirus bypass"),
    ("👻 Невидимий для користувача", "👻 İstifadəçiyə görünməz"),
    ("🌐 Стримінг екрану", "🌐 Ekran yayımı"),
    ("прокрутіть вниз", "aşağı sürüşdürün"),

    # ── Part 1 ──
    ("Частина 1", "Hissə 1"),
    ("Як влаштована система", "Sistem necə işləyir"),

    # sec-desc part1
    ("Три учасники: <b style=\"color:var(--fg)\">Об'єкт</b> (комп'ютер під управлінням), <b style=\"color:var(--fg)\">VPS-сервер</b> (посередник в іншій країні) та <b style=\"color:var(--fg)\">Оператор</b> (ви, у браузері). Об'єкт і оператор ніколи не з'єднуються напряму — лише через сервер-посередник. Це ключове для анонімності.",
     "Üç iştirakçı: <b style=\"color:var(--fg)\">Obyekt</b> (idarə olunan kompüter), <b style=\"color:var(--fg)\">VPS-server</b> (başqa ölkədə vasitəçi) və <b style=\"color:var(--fg)\">Operator</b> (siz, brauzerdə). Obyekt və operator heç vaxt birbaşa bağlanmır — yalnız vasitəçi server vasitəsilə. Bu anonimliyə əsasdır."),

    # Journey — Target node
    ("Об'єкт", "Obyekt"),
    ("Комп'ютер під управлінням<br>(Windows, непомітно для власника)",
     "İdarə olunan kompüter<br>(Windows, sahibinə görünmədən)"),
    ("Агент:", "Agent:"),
    ("невидимий сервіс WPnpSvc", "görünməz servis WPnpSvc"),
    ("Файл:", "Fayl:"),
    ("Запуск:", "Başlatma:"),
    ("автоматично при старті Windows", "Windows başladıqda avtomatik"),
    ("Видно?", "Görünür?"),
    ("Ні — ні іконки, ні вікна", "Xeyr — nə ikon, nə pəncərə"),
    ("Конфіг зашифровано:", "Konfiq şifrəli:"),
    ("pnpext.sys нечитабельний без ключа", "pnpext.sys açar olmadan oxunmaz"),

    # Arrow labels
    ("зашифрований канал<br>порт 443 (HTTPS)", "şifrəli kanal<br>port 443 (HTTPS)"),

    # VPS node
    ("Сервер-посередник в іншій країні<br>(не зберігає дані, лише ретранслює)",
     "Başqa ölkədə relay server<br>(məlumat saxlamır, yalnız ötürür)"),
    ("Роль:", "Rol:"),
    ("міст між об'єктом та оператором", "obyekt ilə operator arasında körpü"),
    ("Користувачі:", "İstifadəçilər:"),
    ("логіни/паролі з правами доступу", "giriş məlumatları və icazələr"),
    ("Модулі:", "Modullar:"),
    ("зашифровані блоки функцій (в пам'яті)", "şifrəli funksiya blokları (yaddaşda)"),
    ("Прямий зв'язок об'єкт↔оператор:", "Birbaşa obyekt↔operator əlaqəsi:"),
    ("Ні", "Xeyr"),
    ("Скріншоти/аудіо:", "Ekran görüntüsü/audio:"),
    ("зберігаються на VPS", "VPS-də saxlanılır"),

    # Operator node
    ("Ви — керуєте через браузер<br>з будь-якого пристрою",
     "Siz — brauzer vasitəsilə idarə edirsiniz<br>istənilən cihazdan"),
    ("Інтерфейс:", "İnterfeys:"),
    ("браузер, жодного ПЗ не потрібно", "brauzer, heç bir proqram tələb olunmur"),
    ("Вхід:", "Giriş:"),
    ("ім'я користувача + пароль", "istifadəçi adı + parol"),
    ("Ролі:", "Rollar:"),
    ("Адміністратор або Оператор", "Administrator və ya Operator"),
    ("IP оператора видний об'єкту?", "Operatorun IP-si obyektə görünür?"),

    # Flow steps
    ("Що відбувається крок за кроком", "Addım-addım nə baş verir"),

    ("Об'єкт запускає агент при старті Windows", "Obyekt Windows başladıqda agenti işə salır"),
    ("Сервіс WPnpSvc автоматично стартує як частина ОС. Жодного вікна, іконки чи сповіщення — власник комп'ютера нічого не помічає.",
     "WPnpSvc servisi əməliyyat sisteminin bir hissəsi kimi avtomatik başlayır. Heç bir pəncərə, ikon və ya bildiriş yoxdur — kompüter sahibi heç nə fərq etmir."),

    ("Об'єкт підключається до VPS через зашифрований канал",
     "Obyekt şifrəli kanal vasitəsilə VPS-ə qoşulur"),
    ("Вихідне з'єднання за протоколом HTTPS (WebSocket) на порт 443. Це стандартний веб-порт — не відрізняється від звичайного серфінгу. Файрволи і роутери його не блокують.",
     "HTTPS (WebSocket) protokolu ilə 443 portuna çıxış əlaqəsi. Bu standart veb portudur — adi brauzerdən fərqlənmir. Firewall-lar və routerlər onu bloklamır."),

    ("Оператор входить у браузері на сторінку VPS",
     "Operator brauzerdə VPS səhifəsinə daxil olur"),
    ("Вводить логін і пароль. VPS перевіряє облікові дані (пароль зберігається в зашифрованому вигляді). Після входу відкривається панель управління з доступними вкладками.",
     "İstifadəçi adı və parolu daxil edir. VPS məlumatları yoxlayır (parol şifrəli saxlanılır). Giriş etdikdən sonra mövcud tablarla idarə paneli açılır."),

    ("Всі команди проходять через VPS-посередник",
     "Bütün əmrlər VPS vasitəçisi ilə keçir"),
    ("Оператор натискає кнопку → команда шифрується → VPS передає її об'єкту → об'єкт виконує → результат повертається тим самим ланцюжком. Прямого з'єднання оператор↔об'єкт немає ніколи.",
     "Operator düyməyə basır → əmr şifrələnir → VPS onu obyektə göndərir → obyekt icra edir → nəticə eyni zəncir ilə geri qayıdır. Operator↔obyekt arasında birbaşa əlaqə heç vaxt olmur."),

    ("Додаткові модулі завантажуються з VPS у пам'ять об'єкта",
     "Əlavə modullar VPS-dən obyektin yaddaşına yüklənir"),
    ("Коли оператор відкриває файловий менеджер або список процесів — об'єкт завантажує зашифрований модуль з VPS та запускає його прямо в оперативній пам'яті. На жорсткий диск об'єкта нічого не записується.",
     "Operator fayl menecerini və ya proseslər siyahısını açdıqda — obyekt VPS-dən şifrəli modulu yükləyir və onu birbaşa RAM-da işə salır. Obyektin sabit diskine heç nə yazılmır."),

    # Modules section
    ("Основний агент та додаткові модулі", "Əsas agent və əlavə modullar"),
    ("Система поділена на частини: головний агент (завжди в пам'яті) та окремі зашифровані модулі (завантажуються за потребою).",
     "Sistem hissələrə bölünüb: əsas agent (həmişə yaddaşda) və ayrıca şifrəli modullar (tələb üzrə yüklənir)."),
    ("Головний агент: pnpext.dll", "Əsas agent: pnpext.dll"),
    ("Завжди працює у фоні як сервіс Windows", "Həmişə fonda Windows servisi kimi işləyir"),
    ("Трансляція екрану оператору", "Operatora ekran yayımı"),
    ("Передача керування мишею та клавіатурою", "Siçan və klaviatura idarəsinin ötürülməsi"),
    ("Запис та трансляція аудіо/мікрофону", "Audio/mikrofon yazılması və canlı yayım"),
    ("Автоматичні скріншоти", "Avtomatik ekran görüntüləri"),
    ("Відстеження стану (сон/блокування/запуск)", "Vəziyyət izləməsi (yuxu/kilidli/başlatma)"),
    ("Віддалене самооновлення", "Uzaqdan öz-özünü yeniləmə"),
    ("Зашифровані модулі (в пам'яті)", "Şifrəli modullar (yaddaşda)"),
    ("Завантажуються лише коли потрібні. Зникають з пам'яті при зупинці.",
     "Yalnız lazım olduqda yüklənir. Dayandırıldıqda yaddaşdan silinir."),
    ("файловий менеджер (перегляд, завантаження)", "fayl meneceri (baxış, yükləmə)"),
    ("процеси та сервіси Windows", "Windows prosesləri və servisləri"),
    ("керування антивірусом та журналами", "antivirus və jurnal idarəetməsi"),
    ("інформація про залізо об'єкта", "obyektin aparat məlumatları"),

    # Infobox orange
    ("💡 Навіщо ділити на модулі? Найбільш \"підозрілі\" команди (керування файлами, процесами, антивірусом) винесені з головного файлу. Антивірус сканує файл на диску і нічого зайвого не знаходить. Модулі живуть лише в пам'яті і зникають при перезавантаженні.",
     "💡 Niyə modullara bölünür? Ən \"şübhəli\" əmrlər (fayl, proses, antivirus idarəetməsi) əsas fayldan çıxarılıb. Antivirus diskdəki faylı skan edir və heç nə şübhəli tapmır. Modullar yalnız yaddaşda yaşayır və yenidən başladıldıqda silinir."),

    # ── Part 2 ──
    ("Частина 2", "Hissə 2"),
    ("Що вміє система", "Sistemin imkanları"),
    ("Всі функції доступні через браузер. Не потрібно встановлювати жодного ПЗ на комп'ютер оператора.",
     "Bütün funksiyalar brauzer vasitəsilə əlçatandır. Operatorun kompüterinə heç bir proqram quraşdırmaq lazım deyil."),

    # Feature cards
    ("Перегляд екрану в реальному часі", "Real vaxt rejimində ekrana baxış"),
    ("Бачите екран об'єкта прямо в браузері, наче сидите поруч. Зображення оновлюється безперервно. Налаштовуються якість, частота кадрів та масштаб.",
     "Obyektin ekranını birbaşa brauzerdə görürsünüz, sanki yanındasınız. Görüntü fasiləsiz yenilənir. Keyfiyyət, kadr sürəti və miqyas tənzimlənir."),
    ("Потік іде через VPS-сервер. WebRTC P2P можна увімкнути окремо, налаштувавши STUN/TURN-сервер.",
     "Axın VPS-server vasitəsilə keçir. WebRTC P2P STUN/TURN serveri konfiqurasiya etməklə ayrıca aktivləşdirilə bilər."),

    ("Керування мишею та клавіатурою", "Siçan və klaviatura idarəsi"),
    ("Клікаєте прямо по зображенню екрану в браузері — об'єкт реагує. Можна натискати клавіші, переміщати мишу, прокручувати сторінки, відкривати програми.",
     "Brauzerdəki ekran görüntüsünə birbaşa klikləyirsiniz — obyekt reaksiya verir. Düymələrə basa, siçanı hərəkət etdirə, səhifələri sürüşdürə, proqramlar aça bilərsiniz."),

    ("Файловий менеджер", "Fayl meneceri"),
    ("Переглядайте будь-які папки на комп'ютері об'єкта, включаючи системні та приховані. Завантажуйте потрібні файли собі, завантажуйте файли на об'єкт, видаляйте та перейменовуйте.",
     "Sistem və gizli qovluqlar da daxil olmaqla obyekt kompüterin istənilən qovluğunu nəzərdən keçirin. Lazımlı faylları özünüzə yükləyin, obyektə fayl yükləyin, silin və adını dəyişdirin."),

    ("Список процесів та сервісів", "Proseslər və servislərin siyahısı"),
    ("Бачите всі запущені програми та служби Windows в реальному часі. Можна завершити будь-яку програму, запустити нову або змінити тип запуску служби.",
     "Real vaxtda işləyən bütün proqramları və Windows servislərini görürsünüz. İstənilən proqramı bitirə, yeni birini başlada və ya servisin başlanma növünü dəyişə bilərsiniz."),

    ("Командний рядок", "Əmr sətri (terminal)"),
    ("Повноцінний термінал (cmd/PowerShell) прямо в браузері. Виконуйте будь-які команди від імені системи з максимальними правами. Результат видно миттєво.",
     "Birbaşa brauzerdə tam terminal (cmd/PowerShell). Maksimum icazələrlə sistem adından istənilən əmrləri icra edin. Nəticə dərhal görünür."),

    ("Реєстр Windows", "Windows Reyestri"),
    ("Повний доступ до реєстру Windows: переглядайте, створюйте, змінюйте та видаляйте будь-які ключі та значення. Всі типи даних підтримуються.",
     "Windows reyestrinə tam giriş: istənilən açar və dəyərləri baxın, yaradın, dəyişdirin və silin. Bütün məlumat növləri dəstəklənir."),

    # "Автоматичні скріншоти" → already translated above, duplicate skip
    ("Об'єкт автоматично робить знімки екрану через заданий інтервал та зберігає їх на VPS. Налаштовується: кожні N секунд, лише певні програми, якість та розмір.",
     "Obyekt müəyyən edilmiş aralıqda avtomatik ekran görüntüsü çəkir və onları VPS-də saxlayır. Tənzimlənir: hər N saniyədə bir, yalnız müəyyən proqramlar, keyfiyyət və ölçü."),

    ("Аудіо: запис та прослуховування", "Audio: yazma və canlı dinləmə"),
    ("Запис мікрофону та системного звуку. Файли зберігаються на VPS сегментами. Live-режим: слухайте прямий звук через браузер. Іконка мікрофону в треї <b>не з'являється</b>.",
     "Mikrofon və sistem sesinin yazılması. Fayllar VPS-də seqmentlər şəklində saxlanılır. Canlı rejim: brauzer vasitəsilə birbaşa səsi dinləyin. Tepsidə mikrofon ikonu <b>görünmür</b>."),

    ("Історія активності об'єкта", "Obyektin aktivlik tarixi"),
    ("Система фіксує: коли об'єкт увімкнений, вимкнений, пішов у сон, заблокував екран. Аналітика: загальний час роботи, кількість сесій, статистика за період.",
     "Sistem qeyd edir: obyektin nə vaxt yandığını, söndüyünü, yuxuya getdiyini, ekranı kiliddlədiyini. Analitika: ümumi işləmə müddəti, sessiya sayı, dövr statistikası."),

    ("Керування операторами", "Operator idarəetməsi"),
    ("Створюйте кількох операторів з різними правами: хтось бачить лише екран та файли, хтось має повний доступ. Кожен входить під своїм логіном.",
     "Müxtəlif icazələrlə bir neçə operator yaradın: kimisi yalnız ekran və faylları görür, kimisi tam girişə malikdir. Hər biri öz girişi ilə daxil olur."),

    ("Оновлення агента через браузер", "Agent yeniləməsi brauzer vasitəsilə"),
    ("Завантажте нову версію агента через браузер — об'єкт завантажить її, замінить стару та перезапуститься. Фізичний доступ до комп'ютера об'єкта не потрібен.",
     "Brauzerdən agentin yeni versiyasını yükləyin — obyekt onu yükləyib köhnəni əvəz edərək yenidən başlayacaq. Obyektin kompüterinə fiziki giriş lazım deyil."),

    ("Редактор налаштувань об'єкта", "Obyekt parametrləri redaktoru"),
    ("Змінюйте налаштування агента на об'єкті прямо з браузера: якість стриму, параметри аудіо, інтервал скріншотів. Зміни застосовуються без перевстановлення.",
     "Brauzerdən birbaşa obyektdəki agent parametrlərini dəyişdirin: axın keyfiyyəti, audio parametrləri, ekran görüntüsü aralığı. Dəyişikliklər yenidən quraşdırma olmadan tətbiq edilir."),

    ("Самознищення", "Öz-özünü məhvetmə"),
    ("Одна кнопка — і агент повністю видаляється з об'єкта: сервіс зупинено, файли видалено, записи реєстру очищено. Операція незворотна.",
     "Bir düymə — agent obyektdən tamamilə silinir: servis dayandırılır, fayllar silinir, reyestr qeydləri təmizlənir. Əməliyyat geri alınmazdır."),

    ("Встановлені програми", "Quraşdırılmış proqramlar"),
    ("Повний список встановленого програмного забезпечення на об'єкті з версіями та датами встановлення.",
     "Obyektdə quraşdırılmış bütün proqram təminatının versiyalar və quraşdırılma tarixləri ilə tam siyahısı."),

    ("Керування захистом", "Mühafizə idarəetməsi"),
    ("Керування антивірусом Windows Defender, очищення системних журналів за заданими шаблонами. Прибирає сліди активності із системних логів.",
     "Windows Defender antivirusunun idarə edilməsi, müəyyən edilmiş şablonlara görə sistem jurnallarının təmizlənməsi. Sistem jurnallarından aktivlik izlərini silir."),

    ("Журнал подій Windows", "Windows Hadisə Jurnalı"),
    ("Перегляд системних журналів Windows (Application, System, Security). Фільтрація за джерелом та датою. Автоматичне очищення небажаних записів.",
     "Windows sistem jurnallarına (Application, System, Security) baxış. Mənbəyə və tarixə görə filtrasiya. Arzuolunmaz qeydlərin avtomatik təmizlənməsi."),

    # ── Part 3 ──
    ("Частина 3", "Hissə 3"),
    ("Безпека та прихованість", "Təhlükəsizlik və gizlilik"),
    ("Кілька незалежних шарів захисту: від шифрування всього трафіку до неможливості встановити особу оператора навіть при повному мережевому перехопленні.",
     "Bir neçə müstəqil mühafizə layı: bütün trafikin şifrələnməsindən tam şəbəkə tutulması zamanı belə operatorun şəxsiyyətinin müəyyən edilməsinin qeyri-mümkünlüyünə qədər."),

    # Network anonymity
    ("🌍 Мережева анонімність: оператор невидимий",
     "🌍 Şəbəkə anonimliyi: operator görünməzdir"),
    ("Що бачить сніфер / слідчий", "Sniffer / müstəntiq nə görür"),

    ("<b>IP об'єкта</b> — видно лише з'єднання з IP адресою VPS. Хто на іншому кінці — невідомо.",
     "<b>Obyektin IP-si</b> — yalnız VPS IP ünvanı ilə əlaqə görünür. Digər tərəfdə kimin olduğu məlum deyil."),
    ("<b>IP оператора</b> — об'єкт не знає і не може дізнатися IP оператора. Він ніколи не передається об'єкту.",
     "<b>Operatorun IP-si</b> — obyekt operatorun IP-ni bilmir və öyrənə bilmir. O heç vaxt obyektə göndərilmir."),
    ("<b>Зміст трафіку</b> — весь потік даних зашифрований (TLS 1.3). Сніфер бачить випадкові байти.",
     "<b>Trafik məzmunu</b> — bütün məlumat axını şifrələnib (TLS 1.3). Sniffer təsadüfi baytlar görür."),
    ("<b>Тип активності</b> — трафік виглядає як звичайний HTTPS. Неможливо визначити, що це віддалене керування.",
     "<b>Aktivlik növü</b> — trafik adi HTTPS kimi görünür. Bunun uzaqdan idarəetmə olduğunu müəyyən etmək mümkün deyil."),

    ("Що робить VPS в іншій країні", "Başqa ölkədəki VPS nə edir"),

    ("<b>Розрив прямого зв'язку</b> — об'єкт знає лише IP VPS. Оператор знає лише IP VPS. Вони не знають адрес один одного.",
     "<b>Birbaşa əlaqənin kəsilməsi</b> — obyekt yalnız VPS IP-sini bilir. Operator yalnız VPS IP-sini bilir. Onlar bir-birinin ünvanını bilmir."),
    ("<b>Інша юрисдикція</b> — VPS в іншій країні означає, що місцева влада не може просто запросити логи. Потрібен міжнародний запит.",
     "<b>Fərqli yurisdiksiya</b> — başqa ölkədə VPS deməkdir ki, yerli hakimiyyət jurnalları sadəcə tələb edə bilməz. Beynəlxalq sorğu lazımdır."),
    ("<b>Немає даних про користувачів</b> — VPS зберігає паролі лише в зашифрованому вигляді. Навіть при вилученні сервера паролі не розкриваються.",
     "<b>İstifadəçi məlumatı yoxdur</b> — VPS parolları yalnız şifrəli formada saxlayır. Hətta server müsadirə edilsə belə, parollar açılmır."),
    ("<b>Немає журналів дій</b> — сесійні токени зберігаються лише в RAM сервера і губляться при перезапуску. Довготривалих логів дій немає.",
     "<b>Əməliyyat jurnalı yoxdur</b> — sessiya tokenləri yalnız server RAM-ında saxlanılır və yenidən başladıldıqda itirilir. Uzunmüddətli əməliyyat jurnalları saxlanılmır."),

    # Infobox blue
    ("Максимум що можна дізнатися при розслідуванні:",
     "Araşdırmada maksimum öyrənilə bilənlər:"),
    ("На об'єкті — що він періодично з'єднується з IP-адресою VPS по порту 443 (HTTPS). Це не відрізняється від підключення до будь-якого іншого веб-сайту. <b>Ні мету, ні оператора, ні зміст</b> встановити неможливо без фізичного доступу до VPS з активною сесією.",
     "Obyektdə — onun 443 portunda (HTTPS) VPS IP ünvanına müntəzəm əlaqə qurduğu. Bu hər hansı digər veb-sayta qoşulmaqdan fərqlənmir. Aktiv sessiya ilə VPS-ə fiziki giriş olmadan <b>nə məqsədi, nə operatoru, nə məzmunu</b> müəyyən etmək mümkün deyil."),

    # Security layers header
    ("🔐 Шари захисту", "🔐 Mühafizə layları"),

    # Layer 1
    ("🌐 Шифрування мережі — TLS 1.3", "🌐 Şəbəkə şifrələməsi — TLS 1.3"),
    ("Весь трафік між об'єктом, VPS та оператором шифрується за найсучаснішим стандартом. Перехопити та прочитати дані з інтернет-каналу технічно неможливо.",
     "Obyekt, VPS və operator arasındakı bütün trafik ən müasir standartla şifrələnir. İnternet kanalından məlumatları tutub oxumaq texniki cəhətdən mümkün deyil."),
    ("це HTTPS-з'єднання. Трафік невідрізнюваний від відкриття веб-сайту.",
     "bu HTTPS-əlaqəsidir. Trafik veb-sayt açmaqdan fərqlənmir."),
    ("стандартний веб-порт. Не блокується файрволами. Не викликає підозр.",
     "standart veb portu. Firewall-lar tərəfindən bloklanmır. Şübhə doğurmur."),

    # Layer 2
    ("🔑 Надійний захист паролів", "🔑 Güclü parol mühafizəsi"),
    ("Паролі операторів не зберігаються у відкритому вигляді. Навіть якщо хтось отримає файл з паролями — прочитати їх неможливо. Кожна сесія використовує унікальний одноразовий токен доступу.",
     "Operator parolları açıq mətnlə saxlanılmır. Hətta kimisi parol faylını əldə etsə də — onları oxumaq mümkün deyil. Hər sessiya unikal birdəfəlik giriş tokeni istifadə edir."),
    ("перебір паролів зайняв би роки навіть на потужному комп'ютері.",
     "güclü kompüterdə belə parolların qaba güc ilə tapılması illərlə çəkərdi."),
    ("для кожного користувача свій випадковий ключ. Однакові паролі дають різні хеші.",
     "hər istifadəçi üçün öz təsadüfi açarı. Eyni parollar fərqli hashlar verir."),
    ("одноразовий токен в пам'яті сервера. Губиться при перезапуску. Вкрадений токен застаріє.",
     "server yaddaşında birdəfəlik token. Yenidən başladıqda itirilir. Oğurlanmış token köhnəlir."),

    # Layer 3
    ("🧩 Зашифровані модулі лише в пам'яті", "🧩 Şifrəli modullar yalnız yaddaşda"),
    ("Найбільш \"підозрілі\" команди (робота з файлами, процесами, антивірусом) зберігаються на VPS у зашифрованому вигляді та завантажуються в оперативну пам'ять об'єкта лише за запитом. На жорсткому диску їх немає — антивірус не знаходить.",
     "Ən \"şübhəli\" əmrlər (fayllar, proseslər, antivirus ilə iş) VPS-də şifrəli formada saxlanılır və yalnız sorğu üzrə obyektin RAM-ına yüklənir. Sabit diskdə yoxdur — antivirus tapmır."),
    ("військовий стандарт шифрування. 256-бітний ключ неможливо підібрати перебором за час існування всесвіту.",
     "hərbi şifrələmə standartı. 256-bit açarı kainatın mövcudluğu müddətinə qədər qaba güc ilə tapmaq mümkün deyil."),
    ("у кожного об'єкта свій унікальний ключ розшифровки модулів. Витік одного не компрометує інших.",
     "hər obyektin öz unikal modul şifrəsizləşdirmə açarı var. Birinin sızması digərlərini kompromatlara uğratmır."),
    ("модуль завантажується в пам'ять, минаючи жорсткий диск. Це як запустити програму, яку ніколи не завантажували.",
     "modul sabit diski keçərək yaddaşa yüklənir. Bu, heç vaxt yükləmədiyiniz proqramı işə salmaq kimidir."),
    ("немає слідів на диску: немає файлів, немає записів, немає нічого що можна знайти сканером.",
     "diskdə iz yoxdur: fayl yoxdur, qeyd yoxdur, skaner ilə tapıla biləcək heç nə yoxdur."),

    # Layer 4
    ("🛑 Невидимість для антивірусів", "🛑 Antiviruslara görünməzlik"),
    ("Головний файл агента (pnpext.dll) спеціально підготовлений так, щоб не викликати підозр у антивірусів при статичному та динамічному аналізі. Перевірено на VirusTotal: Elastic — не виявлено, THOR YARA — не виявлено, Windows Defender — не виявлено.",
     "Agentin əsas faylı (pnpext.dll) statik və dinamik analiz zamanı antivirusları şübhəyə salmamaq üçün xüsusi hazırlanıb. VirusTotal-da yoxlanılıb: Elastic — aşkarlanmayıb, THOR YARA — aşkarlanmayıb, Windows Defender — aşkarlanmayıb."),
    ("17 системних бібліотек приховані з таблиці імпортів файлу. Сканер не бачить підозрілих функцій.",
     "17 sistem kitabxanası faylın import cədvəlindən gizlədilmişdir. Skaner şübhəli funksiyalar görmür."),
    ("файл підписаний цифровим підписом і містить метадані Microsoft. У властивостях файлу відображається \"Microsoft Corporation\".",
     "fayl rəqəmsal imzalanmış və Microsoft metadata-sı ehtiva edir. Fayl xüsusiyyətlərində \"Microsoft Corporation\" göstərilir."),
    ("внутрішні рядки, характерні для антивірусних сигнатур, перезаписані. Сканер не знаходить збігів.",
     "antivirus imzalarına xas daxili sətrlər yenidən yazılmışdır. Skaner uyğunluq tapmır."),

    # Layer 5
    ("👻 Повна невидимість для користувача об'єкта",
     "👻 Obyekt istifadəçisinə tam görünməzlik"),
    ("Власник комп'ютера-об'єкта не бачить жодного ознаки роботи агента: немає іконок, спливаючих вікон, записів у системному треї, немає іконки мікрофону при записі аудіо. Агент замаскований під стандартний системний компонент Windows.",
     "Kompüter-obyektin sahibi agentin işləməsinin heç bir əlaməsini görmür: ikon yoxdur, açılan pəncərə yoxdur, sistem tepsi qeydləri yoxdur, audio yazarkən mikrofon ikonu görünmür. Agent standart Windows sistem komponenti kimi maskalanır."),
    ("Немає іконки мікрофону в треї", "Tepsidə mikrofon ikonu yoxdur"),
    ("запис через WASAPI з контексту SYSTEM: індикатор конфіденційності Windows працює лише для користувацьких сесій.",
     "SYSTEM kontekstindən WASAPI vasitəsilə yazma: Windows məxfilik göstəricisi yalnız istifadəçi sessiyaları üçün işləyir."),
    ("Сервіс замаскований", "Servis maskalanmış"),
    ("у диспетчері задач відображається як \"WPnpSvc\" (Plug and Play Extension). Системний компонент питань не викликає.",
     "tapşırıq menecerində \"WPnpSvc\" (Plug and Play Extension) kimi görünür. Sistem komponenti sual doğurmur."),
    ("Автоочистка журналів", "Jurnal avtotəmizliyi"),
    ("системні журнали Windows автоматично очищаються від записів, пов'язаних з агентом.",
     "Windows sistem jurnalları agentlə əlaqəli qeydlərdən avtomatik təmizlənir."),

    # Layer 6
    ("💾 Конфіг на диску зашифровано", "💾 Diskdəki konfiq şifrəlidir"),
    ("Файл налаштувань агента (pnpext.sys) зберігається в зашифрованому вигляді. Навіть при фізичному доступі до жорсткого диску прочитати конфіг без ключа неможливо. Сервер, пароль, токен кімнати — нічого у відкритому вигляді.",
     "Agentin parametr faylı (pnpext.sys) şifrəli formada saxlanılır. Sabit diskə fiziki giriş olsa belə, açar olmadan konfiqin oxunması mümkün deyil. Server, parol, otaq tokeni — heç nə açıq mətnlə."),
    ("подвійний захист: спочатку ключ виводиться з пароля методом PBKDF2 (повільний, стійкий до перебору), потім дані шифруються AES.",
     "ikiqat qorunma: əvvəlcə açar PBKDF2 metoduyla paroldan əldə edilir (yavaş, qaba gücə davamlı), sonra məlumatlar AES ilə şifrələnir."),

    # Layer 7
    ("🧹 Самоочистка при зупинці", "🧹 Dayandırıldıqda öz-özünü təmizləmə"),
    ("При зупинці сервісу всі завантажені модулі вивантажуються з пам'яті, тимчасові файли видаляються. Не залишається слідів оновлень, кешованих даних або робочих файлів.",
     "Servis dayandırıldıqda bütün yüklənmiş modullar yaddaşdan boşaldılır, müvəqqəti fayllar silinir. Yeniləmələrin, keşlənmiş məlumatların və ya iş fayllarının heç bir izi qalmır."),

    # Terms
    ("📖 Технічні терміни простою мовою", "📖 Texniki terminlər sadə dildə"),
    ("Шифрування військового рівня", "Hərbi səviyyəli şifrələmə"),
    ("Алгоритм, яким користуються банки та армія. Ключ довжиною 256 біт. Підібрати його перебором неможливо за час існування всесвіту.",
     "Banklar və ordunun istifadə etdiyi alqoritm. 256-bit açar. Onu qaba güc ilə tapmaq kainatın mövcudluğu müddətinə qədər mümkün deyil."),
    ("Унікальний ключ для кожного об'єкта", "Hər obyekt üçün unikal açar"),
    ("Кожен об'єкт має свій особистий ключ шифрування. Якщо витече ключ одного — всі інші об'єкти залишаться захищеними.",
     "Hər obyektin öz şifrələmə açarı var. Bir açar sızsa — bütün digər obyektlər qorunaqlı qalar."),
    ("Запуск програми без запису на диск", "Diska yazmadan proqramın işə salınması"),
    ("Модуль завантажується зашифрованим, розшифровується і запускається прямо в оперативній пам'яті. Наче програма ніколи не існувала на диску.",
     "Modul şifrəli yüklənir, şifrəsi açılır və birbaşa RAM-da işə salınır. Sanki proqram heç vaxt diskdə mövcud olmayıb."),
    ("Лише в оперативній пам'яті", "Yalnız RAM-da"),
    ("Дані існують лише поки комп'ютер працює. При вимкненні або перезавантаженні все зникає безслідно — як напис на воді.",
     "Məlumatlar yalnız kompüter işləyərkən mövcuddur. Söndürüldükdə və ya yenidən başladıldıqda hər şey iz buraxmadan yox olur — suda yazı kimi."),
    ("Немає слідів на жорсткому диску", "Sabit diskdə iz yoxdur"),
    ("Модулі не записуються на диск. Криміналістична експертиза жорсткого диску не виявить слідів роботи додаткових компонентів.",
     "Modullar diska yazılmır. Sabit diskin məhkəmə ekspertizası əlavə komponentlərin işləməsinin izini aşkar etməyəcək."),
    ("Одностороннє перетворення пароля", "Parolun birtərəfli çevrilməsi"),
    ("Пароль перетворюється на довгий код. Назад із коду пароль не відновити. Для перевірки правильності пароля код обчислюється знову і порівнюється.",
     "Parol uzun koda çevrilir. Koddan parol bərpa edilə bilməz. Parolu yoxlamaq üçün kod yenidən hesablanır və müqayisə edilir."),
    ("Безпечний веб-канал", "Təhlükəsiz veb kanalı"),
    ("WSS — це WebSocket через HTTPS. TLS 1.3 — останній стандарт шифрування інтернет-з'єднань. Той самий протокол використовується в онлайн-банкінгу.",
     "WSS — HTTPS vasitəsilə WebSocket. TLS 1.3 — internet bağlantılarını şifrələmənin ən son standartı. Onlayn bankçılıqda eyni protokoldan istifadə olunur."),
    ("Приховані системні виклики", "Gizli sistem çağırışları"),
    ("17 бібліотек завантажуються динамічно і не видні в \"змісті\" файлу. Антивірус, що переглядає список функцій файлу, нічого підозрілого не бачить.",
     "17 kitabxana dinamik yüklənir və faylın \"mündəricatında\" görünmür. Faylın funksiyalar siyahısını taran edən antivirus heç nə şübhəli görmür."),

    # AV table
    ("🔬 Статус виявлення антивірусами", "🔬 Antivirus aşkarlama statusu"),
    ("Рушій", "Mühərrik"),
    ("Як досягнуто", "Necə əldə edilib"),
    ("Не виявлено", "Aşkarlanmayıb"),
    ("Метадані Microsoft + приховані імпорти + очистка цифрових відбитків",
     "Microsoft metadata + gizli importlar + rəqəmsal izlərin təmizlənməsi"),
    ("Підозрілі команди винесені в модулі в пам'яті (не на диску)",
     "Şübhəli əmrlər yaddaş modullarına köçürülüb (diskdə deyil)"),
    ("Цифровий підпис + правдоподібні метадані + відкладене завантаження",
     "Rəqəmsal imza + inandırıcı metadata + gecikdirilmiş yükləmə"),
    ("MITRE ATT&CK мітки", "MITRE ATT&CK etiketləri"),
    ("Поведінкові мітки — це інформація, не детект. Само по собі не шкідливе.",
     "Davranış etiketləri məlumat xarakteri daşıyır, aşkarlama deyil. Özlüyündə zərərli sayılmır."),

    # Footer
    # Already covered: "Система прихованого віддаленого адміністрування" → "Gizli Uzaqdan İdarəetmə Sistemi"
]

count = 0
for (ua, az) in translations:
    if ua in html:
        html = html.replace(ua, az)
        count += 1
    else:
        print(f"NOT FOUND: {ua[:60]!r}")

print(f"\nDone. {count}/{len(translations)} translations applied.")

with open("PRESENTATION.html", "w", encoding="utf-8") as f:
    f.write(html)

print("File written successfully.")
