#include "contiki.h"
#include "net/routing/routing.h"
#include "net/netstack.h"
#include "net/ipv6/simple-udp.h"
#include "cfs/cfs.h"
#include "lib/crc16.h"
#include "lib/sha-256.h"
#include <string.h>
#include <stdbool.h>
#include "sys/log.h"
#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO

#define WITH_SERVER_REPLY 1
#define UDP_CLIENT_PORT 8765
#define UDP_SERVER_PORT 5678
#define BLOCK_SIZE 64
#define HASH_PKT_MAGIC 0x5348u
#define HASH_ACK_BLOCK 0xFFFFu

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

static int out_fd = -1;
static uint16_t next_expected_block = 0;
static struct simple_udp_connection udp_conn;
static bool transfer_finalized = false;
static uint8_t final_status = 0;

PROCESS(udp_server_process, "UDP server");
AUTOSTART_PROCESSES(&udp_server_process);
/*---------------------------------------------------------------------------*/
static void
udp_rx_callback(struct simple_udp_connection *c,
                const uip_ipaddr_t *sender_addr, uint16_t sender_port,
                const uip_ipaddr_t *receiver_addr, uint16_t receiver_port,
                const uint8_t *data, uint16_t datalen)
{
  struct ack_packet response;

  if (datalen == sizeof(struct hash_packet))
  {
    struct hash_packet *hp = (struct hash_packet *)data;
    if (hp->magic != HASH_PKT_MAGIC)
    {
      return;
    }

    if (!transfer_finalized)
    {
      uint8_t local_digest[SHA_256_DIGEST_LENGTH];
      SHA_256.finalize(local_digest);
      if (out_fd >= 0)
      {
        cfs_close(out_fd);
        out_fd = -1;
      }
      if (memcmp(local_digest, hp->sha256, SHA_256_DIGEST_LENGTH) == 0)
      {
        LOG_INFO("Tum imaj DOGRULANDI: SHA-256 eslesti (%u blok).\n", next_expected_block);
        final_status = 1;
      }
      else
      {
        LOG_ERR("Imaj BOZUK: SHA-256 ozeti uyusmadi!\n");
        final_status = 0;
      }
      transfer_finalized = true;
    }

    response.block_num = HASH_ACK_BLOCK;
    response.status = final_status;
    simple_udp_sendto(&udp_conn, &response, sizeof(struct ack_packet), sender_addr);
    return;
  }

  if (datalen == sizeof(struct firmware_packet))
  {
    struct firmware_packet *pkt = (struct firmware_packet *)data;

    if (pkt->data_len > BLOCK_SIZE)
    {
      LOG_ERR("Blok %u gecersiz boyut (%u), atildi.\n", pkt->block_num, pkt->data_len);
      return;
    }

    uint16_t calc_crc = crc16_data(pkt->data, pkt->data_len, 0);
    if (calc_crc != pkt->crc16)
    {
      LOG_ERR("Blok %u bozuk! (CRC-16 hatasi)\n", pkt->block_num);
      response.block_num = pkt->block_num;
      response.status = 0;
      simple_udp_sendto(&udp_conn, &response, sizeof(struct ack_packet), sender_addr);
      return;
    }

    if (pkt->block_num == next_expected_block)
    {
      if (out_fd >= 0)
      {
        cfs_write(out_fd, pkt->data, pkt->data_len);
        SHA_256.update(pkt->data, pkt->data_len);
        LOG_INFO("Blok %u diske yazildi (%d bayt).\n", pkt->block_num, pkt->data_len);
      }
      next_expected_block++;
    }
    else if (pkt->block_num < next_expected_block)
    {
      LOG_INFO("Eski/Mükerrer blok %u alindi, tekrar ACK yollaniyor.\n", pkt->block_num);
    }
    else
    {
      LOG_WARN("Beklenmeyen blok %u geldi. Beklenen: %u\n", pkt->block_num, next_expected_block);
      return;
    }

    response.block_num = pkt->block_num;
    response.status = 1;
    simple_udp_sendto(&udp_conn, &response, sizeof(struct ack_packet), sender_addr);
  }
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(udp_server_process, ev, data)
{
  PROCESS_BEGIN();

  NETSTACK_ROUTING.root_start();

  cfs_remove("received.elf");
  out_fd = cfs_open("received.elf", CFS_WRITE);
  if (out_fd < 0)
  {
    LOG_ERR("HATA: received.elf olusturulamadi!\n");
  }
  LOG_INFO("UDP sunucu baslatildi, gelen veriler received.elf dosyasina kaydedilecek.\n");

  SHA_256.init();

  simple_udp_register(&udp_conn, UDP_SERVER_PORT, NULL,
                      UDP_CLIENT_PORT, udp_rx_callback);

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
