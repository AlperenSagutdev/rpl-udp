#include "contiki.h"
#include "net/routing/routing.h"
#include "net/netstack.h"
#include "net/ipv6/simple-udp.h"
#include "cfs/cfs.h"
#include "lib/crc16.h"
#include "lib/sha-256.h"
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#include "sys/node-id.h"
#include "sys/log.h"
#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO

#define UDP_CLIENT_PORT 8765
#define UDP_SERVER_PORT 5678
#define BLOCK_SIZE 64
#define TOTAL_BLOCKS 2080
#define HASH_PKT_MAGIC 0x5348u
#define HASH_ACK_BLOCK 0xFFFFu
#define MAX_HASH_RETRIES 5

struct __attribute__((packed)) firmware_packet
{
  uint16_t block_num;
  uint8_t data_len;
  uint16_t crc16;
  uint8_t data[BLOCK_SIZE];
};

struct __attribute__((packed)) ack_packet
{
  uint16_t block_num;
  uint8_t status;
};

struct __attribute__((packed)) hash_packet
{
  uint16_t magic;
  uint8_t sha256[SHA_256_DIGEST_LENGTH];
};

static struct simple_udp_connection udp_conn;
static struct firmware_packet current_pkt;
static uint16_t expected_ack = 0;
static bool file_transfer_complete = false;
static int file_fd = -1;
static bool hash_ack_received = false;
static uint8_t hash_ack_status = 0;

PROCESS(udp_client_process, "UDP client");
AUTOSTART_PROCESSES(&udp_client_process);

static void
udp_rx_callback(struct simple_udp_connection *c,
                const uip_ipaddr_t *sender_addr, uint16_t sender_port,
                const uip_ipaddr_t *receiver_addr, uint16_t receiver_port,
                const uint8_t *data, uint16_t datalen)
{

  if (node_id != 2)
  {
    return;
  }

  if (sender_addr->u8[15] != 1)
  {
    LOG_INFO("Uyari: Yabanci bir dugumden paket alindi, yok sayiliyor.\n");
    return;
  }

  if (datalen == sizeof(struct ack_packet))
  {
    struct ack_packet *ack = (struct ack_packet *)data;

    if (ack->block_num == HASH_ACK_BLOCK)
    {
      hash_ack_received = true;
      hash_ack_status = ack->status;
      process_poll(&udp_client_process);
      return;
    }

    if (ack->status == 1 && ack->block_num == expected_ack)
    {
      LOG_INFO("ACK alindi: Blok %u\n", ack->block_num);
      expected_ack++;
      process_poll(&udp_client_process);
    }
  }
}

PROCESS_THREAD(udp_client_process, ev, data)
{
  static struct etimer timeout_timer;
  uip_ipaddr_t dest_ipaddr;
  static uint16_t target_ack = 0;
  static struct hash_packet hpkt;
  static uint8_t hash_retries;

  PROCESS_BEGIN();

  simple_udp_register(&udp_conn, UDP_CLIENT_PORT, NULL, UDP_SERVER_PORT, udp_rx_callback);

  if (node_id == 2)
  {
    int check_fd = cfs_open("new-firmware.z1", CFS_READ);
    if (check_fd < 0)
    {
      LOG_INFO("Disk bos! 130 KB boyutunda sanal 'new-firmware.z1' olusturuluyor, lutfen bekleyin...\n");
      int write_fd = cfs_open("new-firmware.z1", CFS_WRITE);
      if (write_fd >= 0)
      {
        uint8_t dummy_data[BLOCK_SIZE];
        for (int i = 0; i < BLOCK_SIZE; i++)
          dummy_data[i] = i;

        for (int i = 0; i < TOTAL_BLOCKS; i++)
        {
          cfs_write(write_fd, dummy_data, BLOCK_SIZE);
        }
        cfs_close(write_fd);
        LOG_INFO("130 KB dosya CFS diske yazildi! Aktarim hazir.\n");
      }
    }
    else
    {
      cfs_close(check_fd);
    }
  }

  file_fd = cfs_open("new-firmware.z1", CFS_READ);

  if (node_id == 2)
  {
    SHA_256.init();
  }

  while (!file_transfer_complete && file_fd >= 0)
  {
    if (NETSTACK_ROUTING.node_is_reachable() && NETSTACK_ROUTING.get_root_ipaddr(&dest_ipaddr))
    {
      if (node_id == 2)
      {

        cfs_seek(file_fd, (uint32_t)expected_ack * BLOCK_SIZE, CFS_SEEK_SET);
        int read_bytes = cfs_read(file_fd, current_pkt.data, BLOCK_SIZE);

        if (read_bytes <= 0)
        {
          LOG_INFO("Aktarim bitti! Dosya basariyla gonderildi.\n");
          file_transfer_complete = true;
          cfs_close(file_fd);
          break;
        }

        current_pkt.block_num = expected_ack;
        current_pkt.data_len = read_bytes;
        current_pkt.crc16 = crc16_data(current_pkt.data, read_bytes, 0);

        SHA_256.update(current_pkt.data, read_bytes);

        target_ack = expected_ack;

        while (expected_ack == target_ack)
        {
          if (NETSTACK_ROUTING.node_is_reachable() &&
              NETSTACK_ROUTING.get_root_ipaddr(&dest_ipaddr))
          {
            LOG_INFO("Blok %u gonderiliyor (%d bayt)...\n", current_pkt.block_num, current_pkt.data_len);
            simple_udp_sendto(&udp_conn, &current_pkt, sizeof(struct firmware_packet), &dest_ipaddr);
            etimer_set(&timeout_timer, CLOCK_SECOND * 1);
          }
          else
          {
            LOG_INFO("Root erisilemez, blok %u icin RPL yakinsamasi bekleniyor...\n", current_pkt.block_num);
            etimer_set(&timeout_timer, CLOCK_SECOND * 2);
          }
          PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&timeout_timer) || ev == PROCESS_EVENT_POLL);
        }
      }
    }
    else
    {
      LOG_INFO("Root henuz erisilebilir degil...\n");
      etimer_set(&timeout_timer, CLOCK_SECOND * 2);
      PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&timeout_timer));
    }
  }

  if (node_id == 2 && file_transfer_complete)
  {
    hpkt.magic = HASH_PKT_MAGIC;
    SHA_256.finalize(hpkt.sha256);

    hash_ack_received = false;
    hash_retries = 0;
    while (!hash_ack_received && hash_retries < MAX_HASH_RETRIES)
    {
      if (NETSTACK_ROUTING.node_is_reachable() &&
          NETSTACK_ROUTING.get_root_ipaddr(&dest_ipaddr))
      {
        LOG_INFO("Tum imaj SHA-256 ozeti gonderiliyor (deneme %u/%u)...\n",
                 hash_retries + 1, MAX_HASH_RETRIES);
        simple_udp_sendto(&udp_conn, &hpkt, sizeof(hpkt), &dest_ipaddr);
        etimer_set(&timeout_timer, CLOCK_SECOND * 2);
        hash_retries++;
      }
      else
      {
        LOG_INFO("Hash gonderimi icin root bekleniyor...\n");
        etimer_set(&timeout_timer, CLOCK_SECOND * 2);
      }
      PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&timeout_timer) ||
                               ev == PROCESS_EVENT_POLL);
    }

    if (hash_ack_received)
    {
      if (hash_ack_status == 1)
      {
        LOG_INFO("Sunucu imaj butunlugunu DOGRULADI (SHA-256 eslesti).\n");
      }
      else
      {
        LOG_ERR("Sunucu imaj butunlugunu REDDETTI (SHA-256 uyusmadi)!\n");
      }
    }
    else
    {
      LOG_ERR("Hash ACK alinamadi; sunucu dogrulamasi belirsiz.\n");
    }
  }

  PROCESS_END();
}
