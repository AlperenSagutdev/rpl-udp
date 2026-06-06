# rpl-udp

A simple RPL network with UDP communication. This is a self-contained example:
it includes a DAG root (`udp-server.c`) and DAG nodes (`udp-clients.c`).
This example runs without a border router -- this is a stand-alone RPL network.

The DAG root also acts as UDP server. The DAG nodes are UDP client. The clients
send a UDP request periodically, that simply includes a counter as payload.
When receiving a request, The server sends a response with the same counter
back to the originator.

The `.csc` files show example networks in the Cooja simulator, for sky motes and
for cooja motes.

For this example a "renode" make target is available, to run a 3 node
emulation in the Renode framework. For further instructions on installing and
using Renode please refer to [the documentation][1].

[1]: https://docs.contiki-ng.org/en/develop/doc/tutorials/Running-Contiki-NG-in-Renode.html

The rpl-udp.robot is a Robot framework test for renode. To run that do:

    >make TARGET=cc2538dk
    >renode-test rpl-udp.robot

1. Gerçeklenen Yöntemler (Implemented Methods)
Dur-ve-Bekle (Stop-and-Wait) ARQ Protokolü
İstemci ve sunucu arasındaki dosya aktarımı, güvenilir bir iletişim sağlamak amacıyla Dur-ve-Bekle ARQ mantığıyla tasarlanmıştır. İstemci bir bloğu gönderir ve bir sonraki bloğa geçmeden önce sunucudan o bloğa ait ACK (Onay) paketini bekler. Zaman aşımı durumunda paketi tekrar iletir.

C
// Client: Beklenen ACK gelene kadar aynı paketin belirli aralıklarla tekrar gönderimi
while (expected_ack == target_ack) {
  if (NETSTACK_ROUTING.node_is_reachable() && NETSTACK_ROUTING.get_root_ipaddr(&dest_ipaddr)) {
    simple_udp_sendto(&udp_conn, &current_pkt, sizeof(struct firmware_packet), &dest_ipaddr);
    etimer_set(&timeout_timer, CLOCK_SECOND * 1);
  }
  // ACK gelene veya timer dolana kadar bekle
  PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&timeout_timer) || ev == PROCESS_EVENT_POLL);
}
Dosya Sistemi Entegrasyonu (CFS)
İşletim sisteminin sanal disk yönetimi, Contiki Dosya Sistemi (CFS) kullanılarak gerçeklenmiştir. İstemci cihaz diskte new-firmware.z1 isimli dosyayı okuyup bloklara bölerken, sunucu cihaz ağ üzerinden aldığı bu doğrulanmış blokları received.elf adıyla belleğe yazar.

C
// Client: İlgili bloğa konumlanma ve veriyi okuma
cfs_seek(file_fd, (uint32_t)expected_ack * BLOCK_SIZE, CFS_SEEK_SET);
int read_bytes = cfs_read(file_fd, current_pkt.data, BLOCK_SIZE);

2. Paket Yapıları ve Uzunlukları
Sistemde iletişim için üç farklı paket yapısı (struct) tanımlanmıştır. Bellek hizalaması (padding) nedeniyle oluşabilecek uyumsuzlukları ve gereksiz bayt israfını önlemek için veri yapıları __attribute__((packed)) ile işaretlenmiştir.

Paket Tipi,Görev ve İçerik,Toplam Boyut
firmware_packet,"Sıra numarası (2), veri boyutu (1), CRC (2) ve ham veri (64) taşır.",69 Bayt
ack_packet,Alınan bloğun numarası (2) ve işlem başarı durumu (1) taşır.,3 Bayt
hash_packet,Magic number (2) ve SHA-256 özet değerini (32) taşır.,34 Bayt

Örnek Veri Paketi Gösterimi:
Veri iletiminin kalbini oluşturan paket yapısı ve uzunluk dağılımı aşağıdaki gibidir:

C
#define BLOCK_SIZE 64

struct __attribute__((packed)) firmware_packet {
  uint16_t block_num;        // 2 Bayt
  uint8_t data_len;          // 1 Bayt
  uint16_t crc16;            // 2 Bayt
  uint8_t data[BLOCK_SIZE];  // 64 Bayt
}; // Toplam: 69 Bayt

3. Alınan Önlemler ve Güvenlik Mekanizmaları
Ağ üzerinde oluşabilecek paket kayıpları, veri bozulmaları ve yetkisiz erişimlere karşı çeşitli koruma mekanizmaları gerçeklenmiştir.

A. Blok Bazında Veri Bütünlüğü (CRC-16)
Radyo frekansındaki parazitlerden dolayı havada bozulabilecek bitleri tespit etmek için her bir bloğa CRC-16 algoritması uygulanmıştır. Sunucu, veriyi teslim aldığında kendi CRC-16 değerini hesaplar. Eşleşme olmazsa paketi hatalı kabul edip reddeder.

C
// Server: Gelen paketin CRC-16 kontrolü
uint16_t calc_crc = crc16_data(pkt->data, pkt->data_len, 0);
if (calc_crc != pkt->crc16) {
  LOG_ERR("Blok %u bozuk! (CRC-16 hatasi)\n", pkt->block_num);
  response.block_num = pkt->block_num;
  response.status = 0; // Hata durumu
  simple_udp_sendto(&udp_conn, &response, sizeof(struct ack_packet), sender_addr);
  return;
}
B. Tüm Dosyanın Kriptografik Doğrulaması (SHA-256)
Bireysel bloklar doğru gelse bile dosyanın baştan sona eksiksiz tamamlandığını garanti etmek adına SHA-256 özet algoritması kullanılmıştır. İletim bitiminde istemci dosyanın hash'ini yollar, sunucu kendi hesapladığı hash ile bunu karşılaştırır.

C
// Server: Nihai dosya bütünlüğünün doğrulanması
if (memcmp(local_digest, hp->sha256, SHA_256_DIGEST_LENGTH) == 0) {
  LOG_INFO("Tum imaj DOGRULANDI: SHA-256 eslesti (%u blok).\n", next_expected_block);
  final_status = 1;
} else {
  LOG_ERR("Imaj BOZUK: SHA-256 ozeti uyusmadi!\n");
  final_status = 0;
}
C. Sıra Dışı Paket ve Çift Kopya (Duplicate) Önlemi
Dur-ve-bekle protokolünde aynı paket ağ gecikmesi nedeniyle birden fazla kez ulaşabilir veya sıralar karışabilir. Sunucu, her zaman next_expected_block değerini takip eder.

Beklenen Blok Gelirse: Kabul edilir ve disk yazılır.

Eski Blok Gelirse (Kopya): İstemcinin ACK'yi kaçırdığı varsayılır, blok tekrar diske yazılmaz ama ACK tekrar gönderilir.

İlerideki Blok Gelirse: Doğrudan reddedilir.

C
// Server: Akış ve sıra kontrol mekanizması
if (pkt->block_num == next_expected_block) {
  cfs_write(out_fd, pkt->data, pkt->data_len);
  next_expected_block++;
} else if (pkt->block_num < next_expected_block) {
  LOG_INFO("Eski blok %u alindi, tekrar ACK yollaniyor.\n", pkt->block_num);
} else {
  LOG_WARN("Beklenmeyen blok %u geldi. Beklenen: %u\n", pkt->block_num, next_expected_block);
  return; // Hatalı paket akışını kes
}
D. İletişim Güvenliği ve Düğüm Doğrulama
İletişime istenmeyen düğümlerin dahil olup sahte paket göndermesini engellemek için, istemci cihaz sadece beklediği kök cihazdan (RPL root veya IP adresinin sonu 1 olan düğümden) gelen mesajları dikkate alır.

C
// Client: Yabancı düğümlerden gelen paketleri filtreleme
if (sender_addr->u8[15] != 1) {
  LOG_INFO("Uyari: Yabanci bir dugumden paket alindi, yok sayiliyor.\n");
  return;
}
